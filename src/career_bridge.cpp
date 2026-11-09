// wwe13 career mode - bridge between the game and the career companion app. See career_bridge.h.
//
// Log format (one line per event, grep for "[career]"):
//   [career] seq=N event=cinematic_start callers=0xAAAAAAAA>0xBBBBBBBB>... [after_gameplay=SECONDS]
//   [career] seq=N event=cinematic_end   callers=0xAAAAAAAA>0xBBBBBBBB>... cinematic=SECONDS
//   [career] seq=N event=gameplay_segment duration=SECONDS [likely_match]
//
// A gameplay segment is the time between returning to 60 FPS and the next cinematic. In the first test show the
// match was one 9 min 36 s segment, bracketed by the pre-match cinematic and the winner's celebration. Short
// segments (the 1 s gaps between entrances) are logged too but not flagged as a match.

#include "career_bridge.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include <rex/logging.h>

namespace wwe13::career {
namespace {

// Gameplay longer than this is flagged as a likely match. Entrance gaps in the test show were about 1 s.
constexpr double kLikelyMatchSeconds = 30.0;

// The game sends "return to gameplay" twice about 0.2 s apart; a second end within this window is the same event.
constexpr double kDuplicateEndSeconds = 1.0;

enum class Phase { Unknown, Cinematic, Gameplay };

struct State {
  std::mutex mutex;
  Phase phase = Phase::Unknown;
  uint64_t seq = 0;
  std::chrono::steady_clock::time_point cinematic_start{};
  std::chrono::steady_clock::time_point gameplay_start{};
  std::chrono::steady_clock::time_point last_end{};
};

State& GetState() {
  static State state;
  return state;
}

// "0x822EF57C>0x82123456>..." innermost first, stopping at the first empty entry.
std::string FormatCallers(const CallerChain& callers) {
  std::string text;
  for (uint32_t address : callers) {
    if (address == 0) {
      break;
    }
    if (!text.empty()) {
      text += '>';
    }
    text += fmt::format("0x{:08X}", address);
  }
  return text.empty() ? "unknown" : text;
}

double SecondsBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
  return std::chrono::duration<double>(to - from).count();
}

}  // namespace

void OnCinematicStart(const CallerChain& callers) {
  State& state = GetState();
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(state.mutex);

  if (state.phase == Phase::Gameplay) {
    const double gameplay = SecondsBetween(state.gameplay_start, now);
    REXLOG_INFO("[career] seq={} event=gameplay_segment duration={:.1f}{}", ++state.seq, gameplay,
                gameplay >= kLikelyMatchSeconds ? " likely_match" : "");
    REXLOG_INFO("[career] seq={} event=cinematic_start callers={} after_gameplay={:.1f}", ++state.seq,
                FormatCallers(callers), gameplay);
  } else {
    REXLOG_INFO("[career] seq={} event=cinematic_start callers={}", ++state.seq, FormatCallers(callers));
  }

  state.phase = Phase::Cinematic;
  state.cinematic_start = now;
}

void OnCinematicEnd(const CallerChain& callers) {
  State& state = GetState();
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(state.mutex);

  // The repeated "return to gameplay" request: same event, already handled.
  if (state.phase == Phase::Gameplay && SecondsBetween(state.last_end, now) < kDuplicateEndSeconds) {
    return;
  }

  if (state.phase == Phase::Cinematic) {
    REXLOG_INFO("[career] seq={} event=cinematic_end callers={} cinematic={:.1f}", ++state.seq,
                FormatCallers(callers), SecondsBetween(state.cinematic_start, now));
  } else {
    REXLOG_INFO("[career] seq={} event=cinematic_end callers={}", ++state.seq, FormatCallers(callers));
  }

  state.phase = Phase::Gameplay;
  state.gameplay_start = now;
  state.last_end = now;
}

}  // namespace wwe13::career
