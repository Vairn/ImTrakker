#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace midi {

struct Event {
    bool note_on = true;
    int note = 60;   // MIDI 0..127
    int velocity = 100;
    int channel = 0;
};

// Best-effort OS MIDI input. On Windows uses WinMM; elsewhere a no-op stub.
class Input {
public:
    Input();
    ~Input();

    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    bool open_default();
    void close();
    bool active() const { return active_; }
    std::string device_name() const { return name_; }

    // Drain queued note events (thread-safe).
    std::vector<Event> poll();
    void push(Event e);

private:

    std::mutex mutex_;
    std::deque<Event> queue_;
    bool active_ = false;
    std::string name_ = "none";
#ifdef _WIN32
    void* handle_ = nullptr;  // HMIDIIN
#endif
};

}  // namespace midi
