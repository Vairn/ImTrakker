#pragma once

#include "mod/module.hpp"

#include <array>
#include <mutex>
#include <string>
#include <vector>

namespace mod {

struct ChannelState {
    const Sample* sample = nullptr;
    double sample_pos = 0.0;
    int period = 0;
    int volume = 0;
    int instrument = 0;
    int effect = 0;
    int param = 0;
    int porta_speed = 0;
    int arp_period = 0;
    bool muted = false;

    float peak = 0.f;
    float peak_hold = 0.f;
    float peak_hold_age = 0.f;
    const char* last_note = "---";
    char last_fx[8] = "...";
    std::array<float, kScopeSamples> scope{};
};

class Player {
public:
    explicit Player(Module module);

    void load(Module module);
    void restart();
    void seek_order(int delta);
    void set_playing(bool on) { playing_ = on; }
    bool playing() const { return playing_; }

    // Interleaved stereo float32 in [-1,1]. Thread-safe vs UI snapshot.
    void render(float* interleaved_stereo, int n_frames);

    struct Snapshot {
        int order_pos = 0;
        int pattern_index = 0;
        int row = 0;
        int tick = 0;
        int speed = 6;
        int tempo = 125;
        int song_length = 0;
        int channels = 4;
        bool playing = false;
        bool row_event = false;
        std::string title;
        std::string magic;
        std::vector<ChannelState> channels_state;
        // pattern rows for UI: copy of current pattern (64 x ch)
        std::vector<std::vector<Note>> pattern;
        std::vector<int> orders;
    };

    Snapshot snapshot();
    Module& module() { return module_; }
    const Module& module() const { return module_; }
    std::mutex& mutex() { return mutex_; }

    void toggle_mute(int ch);
    void unmute_all();

private:
    void process_tick();
    void trigger(ChannelState& ch, const Note& note);
    void tick_fx(ChannelState& ch);
    void mix(float* left, float* right, int n);
    int samples_per_tick() const;
    int pattern_index_unlocked() const;

    Module module_;
    int order_pos_ = 0;
    int row_ = 0;
    int tick_ = 0;
    int speed_ = 6;
    int tempo_ = 125;
    int pattern_break_ = -1;
    int pattern_jump_ = -1;
    std::vector<ChannelState> channels_;
    bool playing_ = true;
    bool finished_ = false;
    bool row_event_ = false;
    int tick_left_ = 0;
    std::mutex mutex_;
};

}  // namespace mod
