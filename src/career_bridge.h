// wwe13 career mode - bridge between the game and the career companion app.
//
// Phase 0: turns the game's frame-rate switches into match-phase events. The game drops to 30 FPS for every
// entrance, cutscene and cinematic (sub_82AA65B0) and returns to 60 FPS for gameplay (sub_82AA6650).
//
// Test shows established:
//   - Every cinematic comes through the same generic scene code: the direct caller (0x822EF57C to start,
//     0x822ED7C8 to end) and the six levels above it are identical for entrances and the post-match scene, so
//     callers can't tell scenes apart. The order of the phases can:
//   - before the bell: one cinematic per entrance (two or three in total), about 1 s of gameplay between them;
//     then one long gameplay stretch (the match); then the post-match cinematic.
//   - On the launcher's Classic frame rate, entrance lengths repeat exactly (CM Punk 75.9 s, Cody Rhodes 61.8 s).
//     60 Everywhere froze the game twice and stretched entrances, so career shows run on Classic.
//
// Every event goes to the game log (tagged "[career]") and to the companion app over the pipe in career_pipe.h, as
// one JSON object per line: {"seq":N,"t":SECONDS_SINCE_START,"event":"NAME", ...fields}.

#pragma once

#include <cstdint>
#include <string_view>

namespace wwe13::career {

// The game asked for a cinematic (30 FPS) phase. `return_address` is ctx.lr, logged for reference.
void OnCinematicStart(uint32_t return_address);

// The game asked to return to gameplay (60 FPS). The game sends this twice in a row; the repeat is ignored.
void OnCinematicEnd(uint32_t return_address);

// Publishes one event. `fields` are extra JSON members without the braces, e.g. R"("index":1,"duration":75.9)".
void Emit(std::string_view event, std::string_view fields = {});

// Called about 20 times a second by the background thread: emits match_start once the gameplay after the last
// entrance has run long enough to be the match.
void Tick();

}  // namespace wwe13::career
