// wwe13 career mode - bridge between the game and the career companion app. See career_bridge.h.
//
// Events (one line each, grep for "[career]"):
//   [career] seq=N event=entrance_start index=I
//   [career] seq=N event=entrance_end index=I duration=SECONDS
//   [career] seq=N event=match_end match=SECONDS
//   [career] seq=N event=match_summary entrances=I entrance_time=SECONDS match=SECONDS
//   [career] seq=N event=post_match_start
//   [career] seq=N event=post_match_end duration=SECONDS
//   [career] seq=N event=cinematic_start|cinematic_end caller=0xXXXXXXXX ...   (raw events, kept for research)
//
// "Entrance" here means any cinematic before the bell (entrances and ring introductions). The match is the first
// gameplay stretch longer than kMatchMinSeconds; the cinematic that ends it is the post-match scene. After the
// post-match scene ends, the next cinematic starts a new match.

#include "career_bridge.h"

#include <chrono>
#include <cstdint>
#include <mutex>

#include <rex/logging.h>

namespace wwe13::career {
namespace {

// Gameplay longer than this is the match. Gaps between pre-match cinematics were about 1 s in every test show.
constexpr double kMatchMinSeconds = 30.0;

// The game sends "return to gameplay" twice about 0.2 s apart; a second end within this window is the same event.
constexpr double kDuplicateEndSeconds = 1.0;

using Clock = std::chrono::steady_clock;

enum class Phase {
  Idle,              // menus, or nothing seen yet
  PreMatchCinematic, // an entrance or introduction is playing
  PreMatchGameplay,  // gameplay after a pre-match cinematic: a short gap, or the match once it runs long enough
  PostMatch,         // the post-match cinematic is playing
};

struct State {
  std::mutex mutex;
  Phase phase = Phase::Idle;
  uint64_t seq = 0;
  int entrances = 0;
  double entrance_time = 0.0;
  Clock::time_point cinematic_start{};
  Clock::time_point gameplay_start{};
  Clock::time_point last_end{};
  bool has_last_end = false;
};

State& GetState() {
  static State state;
  return state;
}

double SecondsBetween(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double>(to - from).count();
}

}  // namespace

void OnCinematicStart(uint32_t return_address) {
  State& state = GetState();
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(state.mutex);

  REXLOG_INFO("[career] seq={} event=cinematic_start caller=0x{:08X}", ++state.seq, return_address);

  if (state.phase == Phase::PreMatchGameplay) {
    const double gameplay = SecondsBetween(state.gameplay_start, now);
    if (gameplay >= kMatchMinSeconds) {
      // The long gameplay stretch was the match, and this cinematic is the post-match scene.
      REXLOG_INFO("[career] seq={} event=match_end match={:.1f}", ++state.seq, gameplay);
      REXLOG_INFO("[career] seq={} event=match_summary entrances={} entrance_time={:.1f} match={:.1f}", ++state.seq,
                  state.entrances, state.entrance_time, gameplay);
      REXLOG_INFO("[career] seq={} event=post_match_start", ++state.seq);
      state.phase = Phase::PostMatch;
      state.cinematic_start = now;
      return;
    }
    // A short gap: another pre-match cinematic follows.
  } else if (state.phase == Phase::PostMatch) {
    // A second cinematic inside the post-match scene: still the post-match scene.
    return;
  } else if (state.phase == Phase::Idle) {
    // First cinematic of a new match.
    state.entrances = 0;
    state.entrance_time = 0.0;
  }

  ++state.entrances;
  REXLOG_INFO("[career] seq={} event=entrance_start index={}", ++state.seq, state.entrances);
  state.phase = Phase::PreMatchCinematic;
  state.cinematic_start = now;
}

void OnCinematicEnd(uint32_t return_address) {
  State& state = GetState();
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(state.mutex);

  // The repeated "return to gameplay" request: same event, already handled.
  if (state.has_last_end && SecondsBetween(state.last_end, now) < kDuplicateEndSeconds) {
    return;
  }
  state.last_end = now;
  state.has_last_end = true;

  REXLOG_INFO("[career] seq={} event=cinematic_end caller=0x{:08X}", ++state.seq, return_address);

  switch (state.phase) {
    case Phase::PreMatchCinematic: {
      const double duration = SecondsBetween(state.cinematic_start, now);
      state.entrance_time += duration;
      REXLOG_INFO("[career] seq={} event=entrance_end index={} duration={:.1f}", ++state.seq, state.entrances,
                  duration);
      state.phase = Phase::PreMatchGameplay;
      state.gameplay_start = now;
      break;
    }
    case Phase::PostMatch: {
      REXLOG_INFO("[career] seq={} event=post_match_end duration={:.1f}", ++state.seq,
                  SecondsBetween(state.cinematic_start, now));
      state.phase = Phase::Idle;
      break;
    }
    case Phase::Idle:
    case Phase::PreMatchGameplay:
      // A return to gameplay with no cinematic open (start-up, menus): nothing to track.
      break;
  }
}

}  // namespace wwe13::career
