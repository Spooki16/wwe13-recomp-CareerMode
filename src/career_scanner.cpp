// wwe13 career mode - research memory scanner. See career_scanner.h.
//
// Scanned memory: every committed, writable region of guest virtual memory below the stacks (0x00000000-0x6FFFFFFF),
// the XEX image heaps (0x80000000-0x9FFFFFFF), and one view of physical memory (0xA0000000-0xBFFFFFFF; 0xC0000000
// and 0xE0000000 are other views of the same physical pages). Thread stacks and the GPU/MMIO range are skipped.
//
// Method, per 32-bit aligned word:
//   - the first mark of a state stores a full snapshot as that state's reference;
//   - every later mark of the same state kills words whose value moved since the reference;
//   - once both states have references, words equal in both are killed too.
// When the survivors drop below kListThreshold, the snapshots are replaced by a short candidate list (address,
// value in A, value in B), which later marks keep filtering. Memory use peaks at two snapshots (a few hundred MB).

#include "career_scanner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "career_bridge.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wwe13::career {
namespace {

constexpr size_t kListThreshold = 200000;  // survivors below this switch to a candidate list
constexpr size_t kReportMax = 40;          // candidates written out by F9 or when the list gets this short

struct Range {
  uint32_t low;
  uint32_t high;  // exclusive
};
constexpr Range kScanRanges[] = {
    {0x00000000u, 0x70000000u},
    {0x80000000u, 0xA0000000u},
    {0xA0000000u, 0xC0000000u},
};

struct Region {
  uint32_t base = 0;
  uint32_t size = 0;                  // bytes, multiple of 4
  std::vector<uint32_t> ref[2];       // reference snapshot per state (host order of the raw bytes)
  std::vector<uint64_t> alive;        // one bit per word
};

struct Candidate {
  uint32_t address;
  uint32_t value[2];  // raw word in state A and state B, as stored in guest memory (big-endian)
};

struct Scan {
  std::vector<Region> regions;          // snapshot mode
  std::vector<Candidate> candidates;    // list mode
  bool list_mode = false;
  bool has_ref[2] = {false, false};
  int marks[2] = {0, 0};
};

Scan g_scan;
std::mutex g_scan_mutex;

uint32_t SwapBe(uint32_t raw) {
  return ((raw & 0xFF) << 24) | ((raw & 0xFF00) << 8) | ((raw >> 8) & 0xFF00) | (raw >> 24);
}

// Copies guest memory; returns false instead of crashing if a page faults (e.g. freed during the copy).
bool SafeCopy(void* destination, const void* source, size_t bytes) {
#ifdef _WIN32
  __try {
    std::memcpy(destination, source, bytes);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#else
  std::memcpy(destination, source, bytes);
  return true;
#endif
}

// The scanner runs on its own host thread, which has no guest ThreadState, so it must not use REX_KERNEL_STATE():
// that reads the calling thread's PPC context and crashed the game (null context, fault at 0xAC0) on the first F7.
// The runtime singleton is safe from any thread.
rex::memory::Memory* GuestMemory() {
  rex::Runtime* const runtime = rex::Runtime::instance();
  rex::system::KernelState* const kernel_state = runtime ? runtime->kernel_state() : nullptr;
  return kernel_state ? kernel_state->memory() : nullptr;
}

// Committed, readable and writable regions inside the scan ranges.
std::vector<Region> FindRegions(rex::memory::Memory* memory) {
  std::vector<Region> regions;
  for (const Range& range : kScanRanges) {
    uint64_t address = range.low;
    while (address < range.high) {
      rex::memory::BaseHeap* const heap = memory->LookupHeap(static_cast<uint32_t>(address));
      if (!heap) {
        address += 0x10000;
        continue;
      }
      const uint64_t heap_end = uint64_t(heap->heap_base()) + heap->heap_size();
      rex::memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(static_cast<uint32_t>(address), &info) || info.region_size == 0) {
        address += 0x1000;
        continue;
      }
      const uint64_t start = std::max<uint64_t>(info.base_address, address);
      const uint64_t end = std::min<uint64_t>({uint64_t(info.base_address) + info.region_size, heap_end, range.high});
      const bool usable = (info.state & rex::memory::kMemoryAllocationCommit) &&
                          (info.protect & rex::memory::kMemoryProtectRead) &&
                          (info.protect & rex::memory::kMemoryProtectWrite);
      if (usable && end > start) {
        Region region;
        region.base = static_cast<uint32_t>(start);
        region.size = static_cast<uint32_t>((end - start) & ~uint64_t(3));
        if (region.size > 0) {
          regions.push_back(std::move(region));
        }
      }
      address = std::max<uint64_t>(end, address + 0x1000);
    }
  }
  return regions;
}

size_t CountAlive(const Scan& scan) {
  if (scan.list_mode) {
    return scan.candidates.size();
  }
  size_t alive = 0;
  for (const Region& region : scan.regions) {
    for (uint64_t bits : region.alive) {
      alive += static_cast<size_t>(__builtin_popcountll(bits));
    }
  }
  return alive;
}

// True if the two values differ in exactly one byte and that byte is small (0-15) in both: what a bool, an enum or a
// small counter looks like. Test show: the pause scan plateaued at ~13,000 candidates, most of them pause-menu
// graphics; flag-like words sort first so the real flag isn't buried.
bool FlagLike(const Candidate& c) {
  const uint32_t a = SwapBe(c.value[0]);
  const uint32_t b = SwapBe(c.value[1]);
  const uint32_t diff = a ^ b;
  for (int shift = 0; shift < 32; shift += 8) {
    if ((diff & ~(0xFFu << shift)) == 0) {
      return ((a >> shift) & 0xFF) <= 0x0F && ((b >> shift) & 0xFF) <= 0x0F;
    }
  }
  return false;
}

// 0 = XEX image heaps (the game's globals), 1 = other virtual heaps, 2 = physical memory (mostly GPU data).
int RangeRank(uint32_t address) {
  if (address >= 0x80000000u && address < 0xA0000000u) return 0;
  if (address < 0x70000000u) return 1;
  return 2;
}

const char* RangeName(uint32_t address) {
  switch (RangeRank(address)) {
    case 0: return "image";
    case 1: return "heap";
    default: return "physical";
  }
}

// Candidates in the order worth checking: flag-like first, then globals before heaps before GPU memory.
std::vector<Candidate> Ranked(const Scan& scan) {
  std::vector<Candidate> ranked = scan.candidates;
  std::stable_sort(ranked.begin(), ranked.end(), [](const Candidate& x, const Candidate& y) {
    const bool fx = FlagLike(x), fy = FlagLike(y);
    if (fx != fy) return fx;
    const int rx = RangeRank(x.address), ry = RangeRank(y.address);
    if (rx != ry) return rx < ry;
    return x.address < y.address;
  });
  return ranked;
}

void Report(const Scan& scan, size_t limit) {
  const std::vector<Candidate> ranked = Ranked(scan);
  const size_t shown = std::min(limit, ranked.size());
  for (size_t i = 0; i < shown; ++i) {
    const Candidate& c = ranked[i];
    Emit("scan_candidate", fmt::format(R"("address":"0x{:08X}","a":"0x{:08X}","b":"0x{:08X}","range":"{}","flag_like":{})",
                                       c.address, SwapBe(c.value[0]), SwapBe(c.value[1]), RangeName(c.address),
                                       FlagLike(c) ? "true" : "false"));
  }
}

// The game's logs folder (next to wwe13.exe), where the full candidate list is saved.
std::filesystem::path LogsDir() {
#ifdef _WIN32
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    return std::filesystem::path(buffer).parent_path() / "logs";
  }
#endif
  return std::filesystem::current_path() / "logs";
}

// Writes every candidate, ranked, to logs/career-scan-<time>.txt and returns the file name ("" on failure).
std::string DumpCandidates(const Scan& scan) {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
  const std::filesystem::path path = LogsDir() / (std::string("career-scan-") + stamp + ".txt");
  std::ofstream out(path);
  if (!out) {
    return {};
  }
  out << "# WWE 13 career scan: words with one value in every A mark and another in every B mark\n";
  out << "# marks_a=" << scan.marks[0] << " marks_b=" << scan.marks[1] << " candidates=" << scan.candidates.size()
      << "\n# address a b range flag_like (values as the game reads them)\n";
  for (const Candidate& c : Ranked(scan)) {
    out << fmt::format("0x{:08X} 0x{:08X} 0x{:08X} {} {}\n", c.address, SwapBe(c.value[0]), SwapBe(c.value[1]),
                       RangeName(c.address), FlagLike(c) ? 1 : 0);
  }
  return path.filename().string();
}

// Turns the surviving words into a candidate list and frees the snapshots.
void SwitchToList(Scan& scan) {
  for (const Region& region : scan.regions) {
    const size_t words = region.size / 4;
    for (size_t w = 0; w < words; ++w) {
      if (region.alive[w / 64] & (uint64_t(1) << (w % 64))) {
        scan.candidates.push_back({region.base + static_cast<uint32_t>(w * 4), {region.ref[0][w], region.ref[1][w]}});
      }
    }
  }
  scan.regions.clear();
  scan.regions.shrink_to_fit();
  scan.list_mode = true;
}

[[maybe_unused]] void Mark(int state) {
  rex::memory::Memory* const memory = GuestMemory();
  if (!memory) {
    Emit("scan_error", R"("reason":"game memory not ready")");
    return;
  }
  const auto started = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(g_scan_mutex);
  Scan& scan = g_scan;
  const int other = 1 - state;

  if (scan.list_mode) {
    // Filter the short list: the word must still hold this state's value.
    std::vector<Candidate> kept;
    for (const Candidate& c : scan.candidates) {
      uint32_t raw = 0;
      if (SafeCopy(&raw, memory->TranslateVirtual<const uint8_t*>(c.address), 4) && raw == c.value[state]) {
        kept.push_back(c);
      }
    }
    scan.candidates.swap(kept);
  } else {
    if (scan.regions.empty()) {
      scan.regions = FindRegions(memory);
      for (Region& region : scan.regions) {
        region.alive.assign((region.size / 4 + 63) / 64, ~uint64_t(0));
        const size_t words = region.size / 4;
        if (words % 64) {
          region.alive.back() = (uint64_t(1) << (words % 64)) - 1;
        }
      }
    }
    const bool first_of_state = !scan.has_ref[state];
    for (Region& region : scan.regions) {
      const size_t words = region.size / 4;
      if (!first_of_state && region.ref[state].empty()) {
        continue;  // this region was dropped earlier
      }
      std::vector<uint32_t> now(words);
      if (!SafeCopy(now.data(), memory->TranslateVirtual<const uint8_t*>(region.base), region.size)) {
        std::fill(region.alive.begin(), region.alive.end(), 0);  // region went away: nothing in it can be trusted
        region.ref[0].clear();
        region.ref[1].clear();
        continue;
      }
      if (first_of_state) {
        region.ref[state] = std::move(now);
        // With both references in hand, a word that is the same in A and B doesn't track the state.
        const std::vector<uint32_t>& theirs = region.ref[other];
        if (!theirs.empty()) {
          for (size_t w = 0; w < words; ++w) {
            if (region.ref[state][w] == theirs[w]) {
              region.alive[w / 64] &= ~(uint64_t(1) << (w % 64));
            }
          }
        }
      } else {
        // The word must hold the same value every time this state is marked.
        for (size_t w = 0; w < words; ++w) {
          if (now[w] != region.ref[state][w]) {
            region.alive[w / 64] &= ~(uint64_t(1) << (w % 64));
          }
        }
      }
    }
    scan.has_ref[state] = true;
    if (scan.has_ref[0] && scan.has_ref[1] && CountAlive(scan) < kListThreshold) {
      SwitchToList(scan);
    }
  }

  ++scan.marks[state];
  const size_t alive = CountAlive(scan);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  Emit("scan_mark", fmt::format(R"("state":"{}","marks_a":{},"marks_b":{},"candidates":{},"scan_seconds":{:.2f})",
                                state == 0 ? "A" : "B", scan.marks[0], scan.marks[1], alive, seconds));
  if (scan.list_mode && alive <= kReportMax) {
    Report(scan, kReportMax);
  }
}

[[maybe_unused]] void Reset() {
  std::lock_guard<std::mutex> lock(g_scan_mutex);
  g_scan = Scan{};
  Emit("scan_reset");
}

[[maybe_unused]] void ListCandidates() {
  std::lock_guard<std::mutex> lock(g_scan_mutex);
  if (!g_scan.list_mode) {
    Emit("scan_list", fmt::format(R"("candidates":{},"note":"still too many to list; keep marking")",
                                  CountAlive(g_scan)));
    return;
  }
  size_t flag_like = 0;
  size_t by_range[3] = {0, 0, 0};
  for (const Candidate& c : g_scan.candidates) {
    flag_like += FlagLike(c) ? 1 : 0;
    ++by_range[RangeRank(c.address)];
  }
  const std::string file = DumpCandidates(g_scan);
  Emit("scan_list", fmt::format(R"("candidates":{},"flag_like":{},"image":{},"heap":{},"physical":{},"file":"{}")",
                                g_scan.candidates.size(), flag_like, by_range[0], by_range[1], by_range[2], file));
  Report(g_scan, kReportMax);
}

#ifdef _WIN32
bool GameWindowFocused() {
  HWND window = GetForegroundWindow();
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  return window && process == GetCurrentProcessId();
}

// Live watches: addresses read every tick, with each change sent as a "watch" event. Used to confirm a scan result
// (does it flip exactly when the state does, and nothing else?) and to check whether it keeps its address in a new
// session. The default is the pause-flag candidate from the first successful pause scan.
struct Watch {
  uint32_t address;
  uint32_t last = 0;
  bool has_last = false;
};
std::vector<Watch> g_watches = {
    {0xAE37D3A8u},  // pause scan 2026-11-10: 2 while playing, 0 while paused (14 A / 11 B marks)
};
constexpr size_t kMaxWatches = 8;

void PollWatches() {
  rex::memory::Memory* const memory = GuestMemory();
  if (!memory) {
    return;
  }
  for (Watch& watch : g_watches) {
    uint32_t raw = 0;
    if (!SafeCopy(&raw, memory->TranslateVirtual<const uint8_t*>(watch.address), 4)) {
      continue;
    }
    const uint32_t value = SwapBe(raw);
    if (!watch.has_last) {
      Emit("watch", fmt::format(R"("address":"0x{:08X}","value":{},"first":true)", watch.address, value));
    } else if (value != watch.last) {
      Emit("watch", fmt::format(R"("address":"0x{:08X}","value":{},"previous":{})", watch.address, value,
                                watch.last));
    }
    watch.last = value;
    watch.has_last = true;
  }
}

// F10: watch the best-ranked scan candidate.
void WatchTopCandidate() {
  std::lock_guard<std::mutex> lock(g_scan_mutex);
  if (!g_scan.list_mode || g_scan.candidates.empty()) {
    Emit("scan_error", R"("reason":"no candidate list to watch from yet")");
    return;
  }
  const uint32_t address = Ranked(g_scan).front().address;
  for (const Watch& watch : g_watches) {
    if (watch.address == address) {
      return;
    }
  }
  if (g_watches.size() >= kMaxWatches) {
    g_watches.erase(g_watches.begin());
  }
  g_watches.push_back({address});
  Emit("watch_added", fmt::format(R"("address":"0x{:08X}")", address));
}

void BackgroundThread() {
  constexpr int kKeys[5] = {VK_F6, VK_F7, VK_F8, VK_F9, VK_F10};
  bool was_down[5] = {false, false, false, false, false};
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Tick();
    PollWatches();
    const bool focused = GameWindowFocused();
    for (int i = 0; i < 5; ++i) {
      const bool down = focused && (GetAsyncKeyState(kKeys[i]) & 0x8000) != 0;
      if (down && !was_down[i]) {
        switch (kKeys[i]) {
          case VK_F6: Reset(); break;
          case VK_F7: Mark(0); break;
          case VK_F8: Mark(1); break;
          case VK_F9: ListCandidates(); break;
          case VK_F10: WatchTopCandidate(); break;
        }
      }
      was_down[i] = down;
    }
  }
}
#endif

[[maybe_unused]] std::once_flag g_started;

}  // namespace

void StartScanner() {
#ifdef _WIN32
  std::call_once(g_started, [] { std::thread(BackgroundThread).detach(); });
#endif
}

}  // namespace wwe13::career
