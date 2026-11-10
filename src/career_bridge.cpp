// wwe13 career mode - bridge between the game and the career companion app. See career_bridge.h.
//
// Events:
//   bridge_start                                     the game started (sent once)
//   entrance_start   index
//   entrance_end     index, duration
//   match_start      after                          the match began `after` seconds before this event (sent once
//                                                   the gameplay has run kMatchMinSeconds)
//   match_end        match, paused, pauses          match length in seconds without pauses; time spent paused;
//                                                   number of pauses
//   match_summary    entrances, entrance_time, match, paused
//   pause_start      phase                          the player paused ("idle" when no match is being tracked, e.g.
//                                                   a match that started without any entrance cinematic)
//   pause_end        phase, duration                the player unpaused after `duration` seconds
//   post_match_start
//   post_match_end   duration                       (entrance and post-match durations leave out pauses too)
//   cinematic_start / cinematic_end   caller        raw events, kept for research
//
// "Entrance" here means any cinematic before the bell (entrances and ring introductions). The match is the first
// gameplay stretch longer than kMatchMinSeconds; the cinematic that ends it is the post-match scene. After the
// post-match scene ends, the next cinematic starts a new match.

#include "career_bridge.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include <rex/logging.h>

#include "career_pipe.h"
#include "career_scanner.h"

namespace wwe13::career {
namespace {

// Gameplay longer than this is the match. Gaps between pre-match cinematics were about 1 s in every test show.
constexpr double kMatchMinSeconds = 30.0;

// The game sends "return to gameplay" twice about 0.2 s apart; a second end within this window is the same event.
constexpr double kDuplicateEndSeconds = 1.0;

using Clock = std::chrono::steady_clock;

enum class Phase {
  Idle,               // menus, or nothing seen yet
  PreMatchCinematic,  // an entrance or introduction is playing
  PreMatchGameplay,   // gameplay after a pre-match cinematic: a short gap, or the match once it runs long enough
  PostMatch,          // the post-match cinematic is playing
};

struct State {
  std::mutex mutex;
  Phase phase = Phase::Idle;
  bool match_started = false;  // match_start already sent for this gameplay stretch
  int entrances = 0;
  double entrance_time = 0.0;
  Clock::time_point cinematic_start{};
  Clock::time_point gameplay_start{};
  Clock::time_point last_end{};
  bool has_last_end = false;
  // pause tracking: time paused since the current phase began, and the pause in progress (if any)
  bool paused = false;
  bool pause_known = false;
  Clock::time_point pause_start{};   // start of the pause in progress, or of the phase if it began paused
  Clock::time_point pause_began{};   // when the player actually pressed pause (for the pause_end duration)
  double phase_paused = 0.0;
  int phase_pauses = 0;
};

State& GetState() {
  static State state;
  return state;
}

double SecondsBetween(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double>(to - from).count();
}

const Clock::time_point kBridgeStart = Clock::now();

// Seconds since `start`, leaving out time paused in the current phase.
double ActiveSeconds(const State& state, Clock::time_point start, Clock::time_point now) {
  double paused = state.phase_paused;
  if (state.paused) {
    paused += SecondsBetween(state.pause_start, now);
  }
  return SecondsBetween(start, now) - paused;
}

double PausedSeconds(const State& state, Clock::time_point now) {
  return state.phase_paused + (state.paused ? SecondsBetween(state.pause_start, now) : 0.0);
}

// A new phase starts its pause count from zero (a pause in progress carries over from now).
void ResetPauses(State& state, Clock::time_point now) {
  state.phase_paused = 0.0;
  state.phase_pauses = 0;
  if (state.paused) {
    state.pause_start = now;
  }
}

const char* PhaseName(Phase phase) {
  switch (phase) {
    case Phase::Idle: return "idle";
    case Phase::PreMatchCinematic: return "entrance";
    case Phase::PreMatchGameplay: return "match";
    case Phase::PostMatch: return "post_match";
  }
  return "idle";
}

// Starts the pipe server and the background thread (scanner hotkeys + Tick) when the game loads, then announces
// the bridge. Runs during static initialisation: no game state is touched here.
struct Startup {
  Startup() {
    StartPipeServer();
    StartScanner();
  }
} g_startup;

std::once_flag g_hello;

}  // namespace

void Emit(std::string_view event, std::string_view fields) {
  static std::mutex emit_mutex;
  static uint64_t seq = 0;
  std::call_once(g_hello, [] {
    SendLine(R"({"seq":0,"t":0.0,"event":"bridge_start","version":"phase0-6"})");
  });
  std::lock_guard<std::mutex> lock(emit_mutex);
  ++seq;
  const double t = SecondsBetween(kBridgeStart, Clock::now());
  REXLOG_INFO("[career] seq={} event={} {}", seq, event, fields);
  std::string line = fmt::format(R"({{"seq":{},"t":{:.3f},"event":"{}")", seq, t, event);
  if (!fields.empty()) {
    line += ',';
    line += fields;
  }
  line += '}';
  SendLine(std::move(line));
}

void OnCinematicStart(uint32_t return_address) {
  State& state = GetState();
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(state.mutex);

  Emit("cinematic_start", fmt::format(R"("caller":"0x{:08X}")", return_address));

  if (state.phase == Phase::PreMatchGameplay) {
    const double gameplay = ActiveSeconds(state, state.gameplay_start, now);
    if (gameplay >= kMatchMinSeconds) {
      // The long gameplay stretch was the match, and this cinematic is the post-match scene.
      if (!state.match_started) {
        Emit("match_start", fmt::format(R"("after":{:.1f})", gameplay));
      }
      const double paused = PausedSeconds(state, now);
      Emit("match_end", fmt::format(R"("match":{:.1f},"paused":{:.1f},"pauses":{})", gameplay, paused, state.phase_pauses));
      Emit("match_summary", fmt::format(R"("entrances":{},"entrance_time":{:.1f},"match":{:.1f},"paused":{:.1f})",
                                        state.entrances, state.entrance_time, gameplay, paused));
      Emit("post_match_start");
      state.phase = Phase::PostMatch;
      state.cinematic_start = now;
      ResetPauses(state, now);
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
  Emit("entrance_start", fmt::format(R"("index":{})", state.entrances));
  state.phase = Phase::PreMatchCinematic;
  state.cinematic_start = now;
  ResetPauses(state, now);
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

  Emit("cinematic_end", fmt::format(R"("caller":"0x{:08X}")", return_address));

  switch (state.phase) {
    case Phase::PreMatchCinematic: {
      const double duration = ActiveSeconds(state, state.cinematic_start, now);
      state.entrance_time += duration;
      Emit("entrance_end", fmt::format(R"("index":{},"duration":{:.1f})", state.entrances, duration));
      state.phase = Phase::PreMatchGameplay;
      state.match_started = false;
      state.gameplay_start = now;
      ResetPauses(state, now);
      break;
    }
    case Phase::PostMatch: {
      Emit("post_match_end", fmt::format(R"("duration":{:.1f})", ActiveSeconds(state, state.cinematic_start, now)));
      state.phase = Phase::Idle;
      ResetPauses(state, now);
      break;
    }
    case Phase::Idle:
    case Phase::PreMatchGameplay:
      // A return to gameplay with no cinematic open (start-up, menus): nothing to track.
      break;
  }
}

void Tick() {
  State& state = GetState();
  const auto now = Clock::now();
  uint32_t flag = 0;
  const bool flag_read = ReadGuestU32(kPauseFlagAddress, &flag);
  std::lock_guard<std::mutex> lock(state.mutex);

  // Pause flag: 0 playing, 1 paused. Anything else (loading, memory not set up yet) leaves the state as it was.
  if (flag_read && flag <= 1) {
    const bool paused = flag == 1;
    if (!state.pause_known) {
      state.pause_known = true;
      state.paused = paused;
      state.pause_start = now;
      state.pause_began = now;
    } else if (paused != state.paused) {
      state.paused = paused;
      if (paused) {
        state.pause_start = now;
        state.pause_began = now;
        ++state.phase_pauses;
        Emit("pause_start", fmt::format(R"("phase":"{}")", PhaseName(state.phase)));
      } else {
        state.phase_paused += SecondsBetween(state.pause_start, now);
        // The whole pause, even if a new phase began during it.
        const double duration = SecondsBetween(state.pause_began, now);
        Emit("pause_end", fmt::format(R"("phase":"{}","duration":{:.1f})", PhaseName(state.phase), duration));
      }
    }
  }

  if (state.phase != Phase::PreMatchGameplay || state.match_started) {
    return;
  }
  const double gameplay = ActiveSeconds(state, state.gameplay_start, now);
  if (gameplay >= kMatchMinSeconds) {
    state.match_started = true;
    Emit("match_start", fmt::format(R"("after":{:.1f})", gameplay));
  }
}

}  // namespace wwe13::career
