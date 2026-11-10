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
constexpr size_t kPointerSearchMax = 8;    // F9 searches for pointer paths when this few candidates remain

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

// ---- Pointer search -------------------------------------------------------------------------------------------
// A value found by a scan usually lives in an object the game allocates fresh each session, so its address changes
// (the first pause candidate held animation data in the next session). What stays put is a pointer in the game's
// globals (the XEX image heaps) that leads to the object. For each candidate this finds:
//   level 1:  global G such that  [G] + offset == candidate
//   level 2:  global G such that  [[G] + o1] + o2 == candidate   (one object in between)
// Offsets are limited to kMaxPointerOffset. Physical memory is visible at 0xA0000000, 0xC0000000 and 0xE0000000;
// pointers into any of those views are compared by physical address.

constexpr uint32_t kMaxPointerOffset = 0x2000;
constexpr size_t kMaxIntermediates = 8192;
constexpr size_t kMaxPointerResults = 200;

bool IsPhysicalView(uint32_t address) { return address >= 0xA0000000u; }

// If `pointer` points at or below `target` by at most kMaxPointerOffset, returns true and the offset.
bool PointsNear(uint32_t pointer, uint32_t target, uint32_t* offset) {
  uint32_t p = pointer, t = target;
  if (IsPhysicalView(p) && IsPhysicalView(t)) {
    p &= 0x1FFFFFFFu;
    t &= 0x1FFFFFFFu;
  } else if (IsPhysicalView(p) != IsPhysicalView(t)) {
    return false;
  }
  if (p == 0 || p > t || t - p > kMaxPointerOffset) {
    return false;
  }
  *offset = t - p;
  return true;
}

struct PointerPath {
  uint32_t candidate;
  uint32_t global;
  std::vector<uint32_t> offsets;  // [G] + offsets[0] (level 1); [[G] + offsets[0]] + offsets[1] (level 2)
};

// Calls visit(address, value) for every aligned word in `regions` (values as the game reads them).
template <typename Visit>
void ForEachWord(rex::memory::Memory* memory, const std::vector<Region>& regions, Visit visit) {
  std::vector<uint32_t> buffer;
  for (const Region& region : regions) {
    buffer.resize(region.size / 4);
    if (!SafeCopy(buffer.data(), memory->TranslateVirtual<const uint8_t*>(region.base), region.size)) {
      continue;
    }
    for (size_t w = 0; w < buffer.size(); ++w) {
      visit(region.base + static_cast<uint32_t>(w * 4), SwapBe(buffer[w]));
    }
  }
}

std::vector<PointerPath> FindPointerPaths(rex::memory::Memory* memory, const std::vector<Candidate>& candidates) {
  std::vector<PointerPath> paths;
  const std::vector<Region> all = FindRegions(memory);
  std::vector<Region> globals;
  for (const Region& region : all) {
    if (RangeRank(region.base) == 0) {
      Region copy;
      copy.base = region.base;
      copy.size = region.size;
      globals.push_back(std::move(copy));
    }
  }

  // Level 1: globals that point straight at (or just below) a candidate.
  ForEachWord(memory, globals, [&](uint32_t address, uint32_t value) {
    for (const Candidate& c : candidates) {
      uint32_t offset = 0;
      if (paths.size() < kMaxPointerResults && PointsNear(value, c.address, &offset)) {
        paths.push_back({c.address, address, {offset}});
      }
    }
  });

  // Level 2: words anywhere that point near a candidate (intermediates), then globals that point near those.
  struct Intermediate {
    uint32_t address;  // where the pointer to the candidate's object is stored
    uint32_t key;      // that address, physical-normalised, for matching
    uint32_t offset;   // candidate - pointer
    uint32_t candidate;
  };
  std::vector<Intermediate> intermediates;
  ForEachWord(memory, all, [&](uint32_t address, uint32_t value) {
    if (intermediates.size() >= kMaxIntermediates) {
      return;
    }
    for (const Candidate& c : candidates) {
      uint32_t offset = 0;
      if (PointsNear(value, c.address, &offset)) {
        intermediates.push_back({address, IsPhysicalView(address) ? (address & 0x1FFFFFFFu) : address, offset,
                                 c.address});
      }
    }
  });
  std::sort(intermediates.begin(), intermediates.end(),
            [](const Intermediate& x, const Intermediate& y) { return x.key < y.key; });
  ForEachWord(memory, globals, [&](uint32_t address, uint32_t value) {
    if (value == 0 || paths.size() >= kMaxPointerResults) {
      return;
    }
    const bool physical = IsPhysicalView(value);
    const uint32_t low = physical ? (value & 0x1FFFFFFFu) : value;
    const uint64_t high = uint64_t(low) + kMaxPointerOffset;
    auto it = std::lower_bound(intermediates.begin(), intermediates.end(), low,
                               [](const Intermediate& x, uint32_t key) { return x.key < key; });
    for (; it != intermediates.end() && it->key <= high && paths.size() < kMaxPointerResults; ++it) {
      if (IsPhysicalView(it->address) != physical) {
        continue;
      }
      paths.push_back({it->candidate, address, {it->key - low, it->offset}});
    }
  });
  return paths;
}

std::string FormatPath(const PointerPath& path) {
  std::string text = fmt::format("[0x{:08X}]", path.global);
  for (size_t i = 0; i < path.offsets.size(); ++i) {
    text = (i == 0 ? text : "[" + text + "]") + fmt::format("+0x{:X}", path.offsets[i]);
  }
  return text;
}

std::string DumpCandidates(const Scan& scan, const std::vector<PointerPath>& paths) {
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
  if (!paths.empty()) {
    out << "# pointer paths from the game's globals: [G]+o means the value at G, plus o\n";
    for (const PointerPath& p : paths) {
      out << fmt::format("pointer 0x{:08X} {}\n", p.candidate, FormatPath(p));
    }
  }
  return path.filename().string();
}

std::vector<PointerPath> g_last_paths;  // from the last F9, for F10

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
  std::vector<PointerPath> paths;
  if (g_scan.candidates.size() <= kPointerSearchMax) {
    if (rex::memory::Memory* const memory = GuestMemory()) {
      paths = FindPointerPaths(memory, g_scan.candidates);
    }
  }
  g_last_paths = paths;
  const std::string file = DumpCandidates(g_scan, paths);
  Emit("scan_list", fmt::format(R"("candidates":{},"flag_like":{},"image":{},"heap":{},"physical":{},"file":"{}")",
                                g_scan.candidates.size(), flag_like, by_range[0], by_range[1], by_range[2], file));
  Report(g_scan, kReportMax);
  if (g_scan.candidates.size() <= kPointerSearchMax) {
    Emit("scan_pointers", fmt::format(R"("count":{})", paths.size()));
    for (size_t i = 0; i < paths.size() && i < kReportMax; ++i) {
      Emit("scan_pointer", fmt::format(R"("candidate":"0x{:08X}","path":"{}","levels":{})", paths[i].candidate,
                                       FormatPath(paths[i]), paths[i].offsets.size()));
    }
  }
}

#ifdef _WIN32
bool GameWindowFocused() {
  HWND window = GetForegroundWindow();
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  return window && process == GetCurrentProcessId();
}

// Live watches: values read every tick, with each change sent as a "watch" event. Used to confirm a scan result
// (does it flip exactly when the state does, and nothing else?). A watch is either a plain address or a pointer path
// from the game's globals ([G]+o, [[G]+o1]+o2), which finds the value again in every session.
//
// A watch that changes on almost every read (animation data, a timer) would flood the log, so after kNoisyChanges
// changes within one second it is reported once as noisy and muted for a second.
struct Watch {
  uint32_t global;                // plain address, or the global the pointer path starts from
  std::vector<uint32_t> offsets;  // empty for a plain address
  uint32_t last = 0;
  bool has_last = false;
  int changes_this_second = 0;
  std::chrono::steady_clock::time_point second_start{};
  std::chrono::steady_clock::time_point muted_until{};
};
std::vector<Watch> g_watches;  // none by default: the first pause candidate moved in the next session
constexpr size_t kMaxWatches = 8;
constexpr int kNoisyChanges = 5;

std::string WatchName(const Watch& watch) {
  if (watch.offsets.empty()) {
    return fmt::format("0x{:08X}", watch.global);
  }
  PointerPath path{0, watch.global, watch.offsets};
  return FormatPath(path);
}

// Follows the watch's pointer path; returns false if a pointer on the way is null or unreadable.
bool ResolveWatch(rex::memory::Memory* memory, const Watch& watch, uint32_t* address) {
  uint32_t current = watch.global;
  for (uint32_t offset : watch.offsets) {
    uint32_t raw = 0;
    if (!SafeCopy(&raw, memory->TranslateVirtual<const uint8_t*>(current), 4)) {
      return false;
    }
    const uint32_t pointer = SwapBe(raw);
    if (pointer == 0) {
      return false;
    }
    current = pointer + offset;
  }
  *address = current;
  return true;
}

void PollWatches() {
  rex::memory::Memory* const memory = GuestMemory();
  if (!memory) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  for (Watch& watch : g_watches) {
    uint32_t address = 0;
    uint32_t raw = 0;
    if (!ResolveWatch(memory, watch, &address) ||
        !SafeCopy(&raw, memory->TranslateVirtual<const uint8_t*>(address), 4)) {
      continue;
    }
    const uint32_t value = SwapBe(raw);
    const bool changed = !watch.has_last || value != watch.last;
    const uint32_t previous = watch.last;
    watch.last = value;
    if (!changed) {
      watch.has_last = true;
      continue;
    }
    if (now - watch.second_start >= std::chrono::seconds(1)) {
      watch.second_start = now;
      watch.changes_this_second = 0;
    }
    if (++watch.changes_this_second == kNoisyChanges) {
      Emit("watch_noisy", fmt::format(R"("watch":"{}")", WatchName(watch)));
      watch.muted_until = now + std::chrono::seconds(1);
    }
    if (now < watch.muted_until) {
      watch.has_last = true;
      continue;
    }
    if (!watch.has_last) {
      Emit("watch", fmt::format(R"("watch":"{}","address":"0x{:08X}","value":{},"first":true)", WatchName(watch),
                                address, value));
    } else {
      Emit("watch", fmt::format(R"("watch":"{}","address":"0x{:08X}","value":{},"previous":{})", WatchName(watch),
                                address, value, previous));
    }
    watch.has_last = true;
  }
}

// F10: watch the best-ranked scan candidate, through a pointer path when F9 found one (level 1 preferred).
void WatchTopCandidate() {
  std::lock_guard<std::mutex> lock(g_scan_mutex);
  if (!g_scan.list_mode || g_scan.candidates.empty()) {
    Emit("scan_error", R"("reason":"no candidate list to watch from yet")");
    return;
  }
  const uint32_t address = Ranked(g_scan).front().address;
  Watch watch{address, {}};
  for (const PointerPath& path : g_last_paths) {
    if (path.candidate == address && (watch.offsets.empty() || path.offsets.size() < watch.offsets.size())) {
      watch.global = path.global;
      watch.offsets = path.offsets;
    }
  }
  for (const Watch& existing : g_watches) {
    if (existing.global == watch.global && existing.offsets == watch.offsets) {
      return;
    }
  }
  if (g_watches.size() >= kMaxWatches) {
    g_watches.erase(g_watches.begin());
  }
  g_watches.push_back(watch);
  Emit("watch_added", fmt::format(R"("watch":"{}")", WatchName(watch)));
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

bool ReadGuestU32(uint32_t address, uint32_t* value) {
  rex::memory::Memory* const memory = GuestMemory();
  if (!memory) {
    return false;
  }
  const uint8_t* const source = memory->TranslateVirtual<const uint8_t*>(address);
  uint32_t raw = 0;
  if (!source || !SafeCopy(&raw, source, sizeof(raw))) {
    return false;
  }
  *value = SwapBe(raw);
  return true;
}

void StartScanner() {
#ifdef _WIN32
  std::call_once(g_started, [] { std::thread(BackgroundThread).detach(); });
#endif
}

}  // namespace wwe13::career
