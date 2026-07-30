#include "midi.hpp"

#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace midi {

void Input::push(Event e) {
    std::lock_guard lock(mutex_);
    queue_.push_back(e);
    while (queue_.size() > 256) {
        queue_.pop_front();
    }
}

std::vector<Event> Input::poll() {
    std::lock_guard lock(mutex_);
    std::vector<Event> out(queue_.begin(), queue_.end());
    queue_.clear();
    return out;
}

#ifdef _WIN32

static void CALLBACK midi_callback(HMIDIIN, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1,
                                   DWORD_PTR) {
    if (wMsg != MIM_DATA || !dwInstance) {
        return;
    }
    auto* self = reinterpret_cast<Input*>(dwInstance);
    const uint32_t msg = uint32_t(dwParam1);
    const int status = msg & 0xFF;
    const int data1 = (msg >> 8) & 0xFF;
    const int data2 = (msg >> 16) & 0xFF;
    const int cmd = status & 0xF0;
    const int ch = status & 0x0F;
    if (cmd == 0x90 && data2 > 0) {
        self->push(Event{true, data1, data2, ch});
    } else if (cmd == 0x80 || (cmd == 0x90 && data2 == 0)) {
        self->push(Event{false, data1, 0, ch});
    }
}

Input::Input() = default;

Input::~Input() {
    close();
}

bool Input::open_default() {
    close();
    const UINT n = midiInGetNumDevs();
    if (n == 0) {
        name_ = "no MIDI devices";
        return false;
    }
    MIDIINCAPS caps{};
    midiInGetDevCaps(0, &caps, sizeof(caps));
    name_ = caps.szPname;
    HMIDIIN hin = nullptr;
    if (midiInOpen(&hin, 0, DWORD_PTR(&midi_callback), DWORD_PTR(this), CALLBACK_FUNCTION) !=
        MMSYSERR_NOERROR) {
        name_ = "midiInOpen failed";
        return false;
    }
    handle_ = hin;
    midiInStart(hin);
    active_ = true;
    return true;
}

void Input::close() {
    if (!handle_) {
        active_ = false;
        return;
    }
    auto* hin = static_cast<HMIDIIN>(handle_);
    midiInStop(hin);
    midiInReset(hin);
    midiInClose(hin);
    handle_ = nullptr;
    active_ = false;
}

#else

Input::Input() = default;
Input::~Input() = default;

bool Input::open_default() {
    name_ = "MIDI input not available on this platform";
    active_ = false;
    return false;
}

void Input::close() {
    active_ = false;
}

#endif

}  // namespace midi
