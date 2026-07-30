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
    int period = 0;       // stored period (n_period)
    int out_period = 0;   // Paula period after vibrato/arpeggio
    int volume = 0;       // stored volume 0..64
    int out_volume = 0;   // after tremolo
    int instrument = 0;
    int finetune = 0;     // signed -8..7
    int effect = 0;
    int param = 0;
    bool muted = false;

    // Effect memory / state (ProTracker 2.3d)
    int porta_speed = 0;          // tone porta 3xx memory
    int wanted_period = 0;        // tone porta target
    int tone_porta_dir = 0;       // 0 = down (period+), 1 = up (period-)
    bool glissando = false;
    int vib_speed = 0;
    int vib_depth = 0;
    int vib_pos = 0;              // signed-ish 0..255 wrapping
    int trem_speed = 0;
    int trem_depth = 0;
    int trem_pos = 0;
    int wave_control = 0;         // low 2 = vib wave, bit2 = no retrig; high nibble = trem
    int sample_offset = 0;        // 9xx memory (high byte of byte offset)
    int loop_row = 0;             // E6 pattern loop start
    int loop_count = 0;
    int funk_speed = 0;           // EFx high nibble via glissfunk
    int funk_offset = 0;
    int funk_pos = 0;             // byte index into sample for invert loop
    bool delay_note = false;
    Note delayed{};

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
    void seek_row(int order, int row);
    void set_playing(bool on) { playing_ = on; }
    bool playing() const { return playing_; }

    // One-shot sample preview (mixed on top of song when playing, or alone when paused).
    void audition(int instrument_1based, int period);
    void stop_audition();
    bool auditioning() const { return audition_active_; }

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
    // Remap all patterns + magic and resize mixer voices (2–8).
    void set_channel_count(int channels);

private:
    void process_tick();
    void trigger(ChannelState& ch, const Note& note, bool force_retrig);
    void apply_row_fx(ChannelState& ch, const Note& note);
    void tick_fx(ChannelState& ch);
    void do_tone_porta(ChannelState& ch);
    void do_vibrato(ChannelState& ch);
    void do_tremolo(ChannelState& ch);
    void do_vol_slide(ChannelState& ch, int param);
    void do_arpeggio(ChannelState& ch);
    void do_retrig(ChannelState& ch);
    void update_funk(ChannelState& ch);
    int vib_wave(ChannelState& ch, int pos, int wave) const;
    void mix(float* left, float* right, int n);
    int samples_per_tick() const;
    int pattern_index_unlocked() const;
    void advance_row();

    void mix_audition(float* left, float* right, int n);

    Module module_;
    int order_pos_ = 0;
    int row_ = 0;
    int tick_ = 0;
    int speed_ = 6;
    int tempo_ = 125;
    int pattern_break_ = -1;
    int pattern_jump_ = -1;
    int pattern_delay_ = 0;  // remaining extra replays of current row (EEx)
    bool pattern_loop_ = false;
    int pattern_loop_to_ = 0;
    bool filter_on_ = true;  // Amiga LED filter; E0x
    float filter_l_ = 0.f;
    float filter_r_ = 0.f;
    std::vector<ChannelState> channels_;
    bool playing_ = true;
    bool finished_ = false;
    bool row_event_ = false;
    int tick_left_ = 0;
    std::mutex mutex_;

    bool audition_active_ = false;
    const Sample* audition_sample_ = nullptr;
    double audition_pos_ = 0.0;
    int audition_period_ = 0;
    int audition_volume_ = 64;
};

}  // namespace mod
