// wwe13 career mode - event link from the game to the companion app. See career_pipe.h.

#include "career_pipe.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wwe13::career {
namespace {

constexpr size_t kBacklog = 2000;

struct PipeState {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::string> queue;  // lines not yet written to an app
  std::once_flag started;
};

PipeState& GetPipeState() {
  static PipeState state;
  return state;
}

#ifdef _WIN32
// Writes every queued line to `pipe`. Returns when the app disconnects (a write fails); the unsent lines stay queued.
void ServeClient(HANDLE pipe) {
  PipeState& state = GetPipeState();
  for (;;) {
    std::vector<std::string> batch;
    {
      std::unique_lock<std::mutex> lock(state.mutex);
      state.wake.wait_for(lock, std::chrono::seconds(1), [&] { return !state.queue.empty(); });
      if (state.queue.empty()) {
        // Nothing to send: a zero-byte write still tells us whether the app is gone.
        DWORD written = 0;
        lock.unlock();
        if (!WriteFile(pipe, "", 0, &written, nullptr)) {
          return;
        }
        continue;
      }
      batch.assign(state.queue.begin(), state.queue.end());
    }
    size_t sent = 0;
    for (const std::string& line : batch) {
      const std::string framed = line + "\n";
      DWORD written = 0;
      if (!WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr) ||
          written != framed.size()) {
        break;
      }
      ++sent;
    }
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      for (size_t i = 0; i < sent && !state.queue.empty(); ++i) {
        state.queue.pop_front();
      }
    }
    if (sent != batch.size()) {
      return;
    }
  }
}

void PipeServerThread() {
  for (;;) {
    HANDLE pipe = CreateNamedPipeW(L"\\\\.\\pipe\\wwe13-career", PIPE_ACCESS_OUTBOUND,
                                   PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 64 * 1024,
                                   0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      Sleep(2000);
      continue;
    }
    const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (connected) {
      ServeClient(pipe);
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}
#endif

}  // namespace

void StartPipeServer() {
#ifdef _WIN32
  std::call_once(GetPipeState().started, [] { std::thread(PipeServerThread).detach(); });
#endif
}

void SendLine(std::string line) {
  PipeState& state = GetPipeState();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.queue.push_back(std::move(line));
    while (state.queue.size() > kBacklog) {
      state.queue.pop_front();
    }
  }
  state.wake.notify_one();
}

}  // namespace wwe13::career
