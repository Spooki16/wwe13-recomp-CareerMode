// wwe13 career mode - bridge between the game and the career companion app.
//
// Phase 0: turns the game's frame-rate switches into match-phase events. The game drops to 30 FPS for every
// entrance, cutscene and cinematic (sub_82AA65B0) and returns to 60 FPS for gameplay (sub_82AA6650), so the gaps
// between those calls are the segments of a show: entrances, the match itself, the post-match scene.
//
// Every event is written to the game log with a "[career]" tag. Later phases add more events (match result, moves,
// pins) and send them to the companion app instead of only logging them.

#pragma once

#include <cstdint>

namespace wwe13::career {

// The game asked for a cinematic (30 FPS) phase. `return_address` is the guest return address (ctx.lr), which
// identifies the game code that asked; different callers should separate entrances from post-match scenes.
void OnCinematicStart(uint32_t return_address);

// The game asked to return to gameplay (60 FPS). The game sends this twice in a row; the repeat is ignored.
void OnCinematicEnd(uint32_t return_address);

}  // namespace wwe13::career
