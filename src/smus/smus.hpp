#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace smus {

struct SEvent {
    uint8_t sid = 0;
    uint8_t data = 0;
};

struct Score {
    int tempo = 128 * 120;  // 128ths of a quarter-note per minute
    int volume = 127;
    std::string name;
    std::unordered_map<int, std::string> instruments;  // register -> name
    std::vector<std::vector<SEvent>> tracks;
    std::filesystem::path path;
};

enum class InstrKind { Synth, Sample, S8svx };

struct Instrument {
    std::string name;
    InstrKind kind = InstrKind::Synth;
    std::vector<float> wave;
    int loop_start = 0;
    int loop_end = 0;  // exclusive; 0 = oneshot
    int base_midi = 60;
    float base_rate = 8363.f;
    float volume = 1.f;

    // Synth
    std::vector<float> filter_banks;  // 64*128
    std::vector<float> mod_table;     // 256
    std::array<int, 4> env_levels{{255, 255, 200, 0}};
    std::array<int, 4> env_rates{{128, 128, 128, 64}};
    int f_base = 128;
    int f_env = 0;
    int f_mod = 0;
    int lfo_rate = 0;
    int lfo_inc = 0;
    bool lfo_enable = false;
    bool lfo_oneshot = true;
    bool vol_env = true;
    int vol_mod = 0;
    int pitch_mod = 0;

    // SampledSound
    int ss_oneshot = 0;
    int ss_repeat = 0;
    int ss_lo = 0;
    int ss_hi = 0;
    std::vector<float> ss_data;
    int vib_depth = 0;
    int vib_rate = 0;
    int vib_delay = 0;
};

Score parse_file(const std::filesystem::path& path);
Instrument load_instrument(const std::filesystem::path& folder, const std::string& name);
Instrument default_instrument(const std::string& name = "default");

// Display-only pattern cell (tracker-style row/channel grid).
struct PatternCell {
    int midi = 0;          // 0 = empty / no note
    int instrument = -1;   // register, -1 = none shown
    int volume = -1;       // 0..64-ish, -1 = none
    bool rest = false;
    char text[20]{"--- .. ..."};
};

struct DisplayPattern {
    int channels = 0;
    int rows = 0;
    // cells[row][ch]
    std::vector<std::vector<PatternCell>> cells;
};

DisplayPattern bake_display_pattern(const Score& score);

class Engine {
public:
    static std::unique_ptr<Engine> load(const std::filesystem::path& path);

    explicit Engine(Score score, std::unordered_map<int, Instrument> instruments,
                    int sample_rate = 44100, float master = 0.28f);

    void render(float* interleaved_stereo, int n_frames);
    void set_playing(bool on) { playing_ = on; }
    bool playing() const { return playing_; }
    void restart();
    bool finished() const;

    const Score& score() const { return score_; }
    const DisplayPattern& display_pattern() const { return pattern_; }
    float bpm() const { return bpm_; }
    int sample_rate() const { return sr_; }
    float beats_played() const { return beats_played_; }
    int playhead_row() const { return int(beats_played_ * 8.f); }

    // UI-ish snapshot
    struct ChannelSnap {
        bool active = false;
        int midi = 0;
        int instrument_reg = 0;
        char last_note[8]{"---"};
        char instrument_name[24]{"—"};
        float peak = 0.f;
        float peak_hold = 0.f;
        float env = 0.f;
    };
    struct Snapshot {
        std::string title;
        float bpm = 120.f;
        bool playing = false;
        bool finished = false;
        int tracks = 0;
        int playhead_row = 0;
        int pattern_rows = 0;
        std::array<int, 4> track_index{};
        std::array<int, 4> track_length{};
        std::array<bool, 4> track_done{};
        std::array<ChannelSnap, 4> channels{};
        bool event_flash = false;
    };
    Snapshot snapshot() const;

private:
    struct Voice {
        bool active = false;
        int channel = 0;
        const Instrument* instrument = nullptr;
        double pos = 0.0;
        double step = 0.0;
        float vol = 0.f;
        int samples_left = 0;
        bool release = false;
        float env_fixed = 0.f;
        int env_stage = 0;
        float lfo_phase = 0.f;
        bool lfo_frozen = false;
        float lfo_mod = 0.f;
        float vib_phase = 0.f;
        int vib_delay_left = 0;
        std::vector<float> sample_wave;
        int sample_loop_start = 0;
        int sample_loop_end = 0;
        float note_freq = 440.f;
        bool in_hold = false;
        float peak = 0.f;
        float peak_hold = 0.f;
        int midi = 0;
        int instrument_reg = 0;
        char last_note[8]{"---"};
    };

    struct TrackState {
        std::vector<SEvent> events;
        int index = 0;
        float wait = 0.f;
        int instrument_reg = 0;
        float volume = 1.f;
        std::vector<std::pair<int, int>> chord_notes;  // midi, flags
        bool done = false;
    };

    mutable int last_playhead_row_ = -1;

    void prime_track(TrackState& tr);
    void handle_control(TrackState& tr, const SEvent& ev);
    void start_voice(int ch, int midi, int flags, TrackState& tr, bool tied);
    void consume_event(TrackState& tr, int ch);
    void advance_tracks(float beats);
    void step_envelope(Voice& v, const Instrument& inst, float* env_out, float* bank_out, int n);
    void render_voice(Voice& v, float* mono, int n);
    const Instrument& inst_for_reg(int reg) const;

    Score score_;
    DisplayPattern pattern_;
    std::unordered_map<int, Instrument> instruments_;
    int sr_ = 44100;
    float master_ = 0.28f;
    float bpm_ = 120.f;
    float beat_samples_ = 0.f;
    float score_volume_ = 1.f;
    float beats_played_ = 0.f;
    std::vector<TrackState> tracks_;
    std::array<Voice, 4> voices_{};
    bool playing_ = true;
    Instrument fallback_;
};

bool is_smus_file(const std::filesystem::path& path);

}  // namespace smus
