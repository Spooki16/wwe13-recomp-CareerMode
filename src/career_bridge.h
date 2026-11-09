// wwe13 career mode - bridge between the game and the career companion app.
//
// Phase 0: turns the game's frame-rate switches into match-phase events. The game drops to 30 FPS for every
// entrance, cutscene and cinematic (sub_82AA65B0) and returns to 60 FPS for gameplay (sub_82AA6650).
//
// Test shows 1-3 established:
//   - Every cinematic comes through the same generic scene code: the direct caller (0x822EF57C to start,
//     0x822ED7C8 to end) and the six levels above it are identical for entrances and the post-match scene, so
//     callers can't tell scenes apart. The order of the phases can:
//   - before the bell: two or three cinematics (each entrance, then the introductions), about 1 s of gameplay
//     between them; then one long gameplay stretch (the match); then the post-match cinematic.
//
// The bridge tracks that order and logs [career] events (see career_bridge.cpp). Later phases add the match result,
// moves and pins from the game's match code, and send the events to the companion app instead of only logging them.

#pragma once

#include <cstdint>

namespace wwe13::career {

// The game asked for a cinematic (30 FPS) phase. `return_address` is ctx.lr, logged for reference.
void OnCinematicStart(uint32_t return_address);

// The game asked to return to gameplay (60 FPS). The game sends this twice in a row; the repeat is ignored.
void OnCinematicEnd(uint32_t return_address);

}  // namespace wwe13::career
