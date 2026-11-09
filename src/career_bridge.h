// wwe13 career mode - bridge between the game and the career companion app.
//
// Phase 0: turns the game's frame-rate switches into match-phase events. The game drops to 30 FPS for every
// entrance, cutscene and cinematic (sub_82AA65B0) and returns to 60 FPS for gameplay (sub_82AA6650), so the gaps
// between those calls are the segments of a show: entrances, the match itself, the post-match scene.
//
// Every event is written to the game log with a "[career]" tag. Later phases add more events (match result, moves,
// pins) and send them to the companion app instead of only logging them.

#pragma once

#include <array>
#include <cstdint>

namespace wwe13::career {

// Guest return addresses, innermost first: [0] is ctx.lr (the direct caller), [1..] walk up the guest stack's back
// chain. Unused entries are 0. Test show 1 found the direct caller is the same for every cinematic (0x822EF57C to
// start, 0x822ED7C8 to end), so the outer callers are what should separate entrances from post-match scenes.
using CallerChain = std::array<uint32_t, 6>;

// The game asked for a cinematic (30 FPS) phase.
void OnCinematicStart(const CallerChain& callers);

// The game asked to return to gameplay (60 FPS). The game sends this twice in a row; the repeat is ignored.
void OnCinematicEnd(const CallerChain& callers);

}  // namespace wwe13::career
