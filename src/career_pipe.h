// wwe13 career mode - event link from the game to the companion app.
//
// The game runs a named pipe server, \\.\pipe\wwe13-career, and writes one JSON object per line to whichever app
// is connected (one at a time). Events produced while no app is connected are kept (up to kBacklog) and sent when
// one connects, so an app started mid-show still sees the show so far. Writing never blocks the game: events go
// into a queue that a background thread drains.
//
// tools/career-monitor.ps1 is a minimal app that prints the events as they arrive.

#pragma once

#include <string>

namespace wwe13::career {

// Starts the pipe server thread. Safe to call more than once.
void StartPipeServer();

// Queues one line (a JSON object, no trailing newline) for the connected app.
void SendLine(std::string line);

}  // namespace wwe13::career
