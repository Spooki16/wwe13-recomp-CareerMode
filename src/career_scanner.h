// wwe13 career mode - research tool: finds game memory that tracks a state you can switch on and off.
//
// Works like a "changed / unchanged" memory search. You mark the game as being in state A or state B with hotkeys,
// several times each; the scanner keeps the 32-bit words that hold one value every time you mark A and a different
// value every time you mark B. A few rounds narrow millions of words down to the handful that track that state.
// First target: the pause flag (A = playing, B = paused). The same tool will find the match clock, the winner, etc.
//
// Hotkeys (only while the game window is focused):
//   F7  mark state A (e.g. playing)          F8  mark state B (e.g. paused)
//   F9  list the current candidates          F6  start over
//   F10 watch the best candidate live (every change is reported)
//
// Value scan, for numbers rather than on/off states (a clock, a health bar, a meter, a counter):
//   Ctrl+F6  start (remembers every value)    Ctrl+F7  went up since the last check
//   Ctrl+F8  went down                        Ctrl+F9  didn't change          Ctrl+F10  changed
//   F9 and F10 then list and watch the survivors as usual; F6 starts over.
//
// Progress and results go to the game log ("[career]" lines, event=scan_*) and to the companion app.
// The scanner reads guest memory only; it never writes it. Reads that fault are caught and skipped.

#pragma once

#include <cstdint>

namespace wwe13::career {

// Reads one big-endian 32-bit word of guest memory from any thread. False if the game's memory isn't ready yet or
// the page faults.
bool ReadGuestU32(uint32_t address, uint32_t* value);

// Starts the background thread: hotkeys, scanning, and Tick() for the bridge. Safe to call more than once.
void StartScanner();

}  // namespace wwe13::career
