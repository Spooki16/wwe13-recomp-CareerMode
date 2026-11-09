// wwe13 career mode - bridge between the game and the career companion app. See career_bridge.h.
//
// Events:
//   bridge_start                                     the game started (sent once)
//   entrance_start   index
//   entrance_end     index, duration
//   match_start      after                          the match began `after` seconds before this event (sent once
//                                                   the gameplay has run kMatchMinSeconds)
//   match_end        match                          match length in seconds (paused time included, for now)
//   match_summary    entrances, entrance_time, match
//   post_match_start
//   post_match_end   duration
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
};

State& GetState() {
  static State state;
  return state;
}

double SecondsBetween(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double>(to - from).count();
}

const Clock::time_point kBridgeStart = Clock::now();

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
    SendLine(R"({"seq":0,"t":0.0,"event":"bridge_start","version":"phase0-3"})");
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
    const double gameplay = SecondsBetween(state.gameplay_start, now);
    if (gameplay >= kMatchMinSeconds) {
      // The long gameplay stretch was the match, and this cinematic is the post-match scene.
      if (!state.match_started) {
        Emit("match_start", fmt::format(R"("after":{:.1f})", gameplay));
      }
      Emit("match_end", fmt::format(R"("match":{:.1f})", gameplay));
      Emit("match_summary", fmt::format(R"("entrances":{},"entrance_time":{:.1f},"match":{:.1f})", state.entrances,
                                        state.entrance_time, gameplay));
      Emit("post_match_start");
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
  Emit("entrance_start", fmt::format(R"("index":{})", state.entrances));
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

  Emit("cinematic_end", fmt::format(R"("caller":"0x{:08X}")", return_address));

  switch (state.phase) {
    case Phase::PreMatchCinematic: {
      const double duration = SecondsBetween(state.cinematic_start, now);
      state.entrance_time += duration;
      Emit("entrance_end", fmt::format(R"("index":{},"duration":{:.1f})", state.entrances, duration));
      state.phase = Phase::PreMatchGameplay;
      state.match_started = false;
      state.gameplay_start = now;
      break;
    }
    case Phase::PostMatch: {
      Emit("post_match_end", fmt::format(R"("duration":{:.1f})", SecondsBetween(state.cinematic_start, now)));
      state.phase = Phase::Idle;
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
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.phase != Phase::PreMatchGameplay || state.match_started) {
    return;
  }
  const double gameplay = SecondsBetween(state.gameplay_start, Clock::now());
  if (gameplay >= kMatchMinSeconds) {
    state.match_started = true;
    Emit("match_start", fmt::format(R"("after":{:.1f})", gameplay));
  }
}

}  // namespace wwe13::career
