#include "smus/smus.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace smus {
namespace {

constexpr uint8_t kSidRest = 0x80;
constexpr uint8_t kSidInstrument = 0x81;
constexpr uint8_t kSidDynamic = 0x84;
constexpr uint8_t kSidTempo = 0x88;

const int kChannelPan[4] = {0, 1, 1, 0};  // L R R L

float note_duration_beats(int flags) {
    const int division = flags & 0x07;
    const bool dotted = (flags & 0x08) != 0;
    const int ntuplet = (flags >> 4) & 0x03;
    float beats = 4.f / float(1 << division);
    if (dotted) {
        beats *= 1.5f;
    }
    if (ntuplet == 1) {
        beats *= 2.f / 3.f;
    } else if (ntuplet == 2) {
        beats *= 4.f / 5.f;
    } else if (ntuplet == 3) {
        beats *= 4.f / 7.f;
    }
    return beats;
}

float sonix_rate_units(int r) {
    r &= 0xFFFF;
    if (r == 0) {
        return 4000.f;
    }
    const int exp = 7 ^ ((r >> 5) & 7);
    const int mant = (r & 0x1F) + 0x21;
    return float(mant << exp);
}

int sample_octave_for_midi(int midi, int lo, int hi) {
    return std::clamp(10 - (midi / 12), lo, hi);
}

const float kNotePeriod[12] = {
    float(0x8000), float(0x78D1), float(0x7209), float(0x6BA2), float(0x6598), float(0x5FE4),
    float(0x5A82), float(0x556E), float(0x50A3), float(0x4C1C), float(0x47D6), float(0x43CE),
};

}  // namespace

Engine::Engine(Score score, std::unordered_map<int, Instrument> instruments, int sample_rate,
               float master)
    : score_(std::move(score)),
      instruments_(std::move(instruments)),
      sr_(sample_rate),
      master_(master),
      fallback_(default_instrument()) {
    bpm_ = std::max(score_.tempo / 128.f, 1.f);
    beat_samples_ = (60.f / bpm_) * float(sr_);
    score_volume_ = float(score_.volume) / 127.f;
    for (size_t i = 0; i < score_.tracks.size() && i < 4; ++i) {
        TrackState tr;
        tr.events = score_.tracks[i];
        tracks_.push_back(std::move(tr));
    }
    for (size_t i = 0; i < voices_.size(); ++i) {
        voices_[i].channel = int(i);
    }
    for (auto& tr : tracks_) {
        prime_track(tr);
    }
}

std::unique_ptr<Engine> Engine::load(const std::filesystem::path& path) {
    auto score = parse_file(path);
    std::unordered_map<int, Instrument> instruments;
    const auto folder = path.parent_path();
    for (const auto& [reg, name] : score.instruments) {
        try {
            instruments[reg] = load_instrument(folder, name);
        } catch (...) {
            instruments[reg] = default_instrument(name);
        }
    }
    if (!instruments.count(0)) {
        instruments[0] = default_instrument();
    }
    return std::make_unique<Engine>(std::move(score), std::move(instruments));
}

void Engine::restart() {
    for (auto& v : voices_) {
        v = Voice{};
    }
    for (size_t i = 0; i < voices_.size(); ++i) {
        voices_[i].channel = int(i);
    }
    tracks_.clear();
    for (size_t i = 0; i < score_.tracks.size() && i < 4; ++i) {
        TrackState tr;
        tr.events = score_.tracks[i];
        tracks_.push_back(std::move(tr));
    }
    bpm_ = std::max(score_.tempo / 128.f, 1.f);
    beat_samples_ = (60.f / bpm_) * float(sr_);
    for (auto& tr : tracks_) {
        prime_track(tr);
    }
    playing_ = true;
}

bool Engine::finished() const {
    bool tracks_done = true;
    for (const auto& t : tracks_) {
        if (!t.done && t.index < int(t.events.size())) {
            tracks_done = false;
            break;
        }
    }
    bool voices_idle = true;
    for (const auto& v : voices_) {
        if (v.active) {
            voices_idle = false;
            break;
        }
    }
    return tracks_done && voices_idle;
}

const Instrument& Engine::inst_for_reg(int reg) const {
    auto it = instruments_.find(reg);
    if (it != instruments_.end()) {
        return it->second;
    }
    return fallback_;
}

void Engine::prime_track(TrackState& tr) {
    while (tr.index < int(tr.events.size())) {
        const auto& ev = tr.events[size_t(tr.index)];
        if (ev.sid < 0x80 || ev.sid == kSidRest) {
            break;
        }
        handle_control(tr, ev);
        ++tr.index;
    }
}

void Engine::handle_control(TrackState& tr, const SEvent& ev) {
    if (ev.sid == kSidInstrument) {
        tr.instrument_reg = ev.data;
    } else if (ev.sid == kSidDynamic) {
        tr.volume = float(std::max(int(ev.data), 1)) / 127.f;
    } else if (ev.sid == kSidTempo && ev.data > 0) {
        bpm_ = float(ev.data);
        beat_samples_ = (60.f / bpm_) * float(sr_);
    }
}

void Engine::start_voice(int ch, int midi, int flags, TrackState& tr, bool tied) {
    const Instrument& inst = inst_for_reg(tr.instrument_reg);
    const float dur_beats = note_duration_beats(flags);
    int note_samples = std::max(1, int(dur_beats * beat_samples_));
    int gate_samples = tied ? note_samples : std::max(1, (note_samples * 0xC000) >> 16);
    const float freq = 440.f * std::pow(2.f, float(midi - 69) / 12.f);

    std::vector<float> sample_wave;
    int sample_loop_start = 0, sample_loop_end = 0;
    double step = 0.0;

    if (inst.kind == InstrKind::Synth) {
        step = double(freq * 128.f) / double(sr_);
    } else if (inst.kind == InstrKind::Sample && !inst.ss_data.empty()) {
        const int octv = sample_octave_for_midi(midi, inst.ss_lo, inst.ss_hi);
        const int oneshot = inst.ss_oneshot;
        const int repeat = inst.ss_repeat;
        const int lo = inst.ss_lo;
        const int offset = oneshot * ((1 << octv) - (1 << lo));
        const int length = oneshot << octv;
        if (offset >= 0 && offset + length <= int(inst.ss_data.size()) && length > 0) {
            sample_wave.assign(inst.ss_data.begin() + offset, inst.ss_data.begin() + offset + length);
        } else {
            sample_wave = inst.wave;
        }
        const int wlen = int(sample_wave.size());
        if (repeat > 0 && repeat < oneshot && wlen > 0) {
            sample_loop_start = std::min(wlen - 1, repeat << octv);
            sample_loop_end = std::min(wlen, oneshot << octv);
            if (sample_loop_end - sample_loop_start < 2) {
                sample_loop_start = sample_loop_end = 0;
            } else {
                // Crossfade hold region join
                const int ls = sample_loop_start, le = sample_loop_end;
                const int fade = std::min(std::max(le - ls, 2) / 4, 32);
                if (fade >= 2) {
                    for (int i = 0; i < fade; ++i) {
                        const float t = float(i + 1) / float(fade);
                        const float a = sample_wave[size_t(ls + i)];
                        const float b = sample_wave[size_t(le - fade + i)];
                        sample_wave[size_t(ls + i)] = a * t + b * (1.f - t);
                    }
                }
            }
        }
        const int note_in_oct = midi % 12;
        const float rate = inst.base_rate * (kNotePeriod[0] / kNotePeriod[note_in_oct]);
        step = double(rate) / double(sr_);
        if (sample_loop_end == 0 && wlen > 0) {
            const int max_play = int(double(wlen) / std::max(step, 1e-6)) + sr_ / 20;
            gate_samples = std::min(gate_samples, max_play);
        }
    } else {
        const float base_freq = 440.f * std::pow(2.f, float(inst.base_midi - 69) / 12.f);
        step = double(inst.base_rate / float(sr_)) * double(freq / std::max(base_freq, 1e-6f));
        sample_wave = inst.wave;
        sample_loop_start = inst.loop_start;
        sample_loop_end = inst.loop_end;
    }

    const float vol = tr.volume * inst.volume * score_volume_;
    int vib_delay = 0;
    if (inst.vib_delay > 0) {
        vib_delay = int((sonix_rate_units(inst.vib_delay) / 8000.f) * float(sr_));
    }

    Voice& v = voices_[size_t(ch)];
    v.active = true;
    v.channel = ch;
    v.instrument = &inst_for_reg(tr.instrument_reg);
    v.pos = 0.0;
    v.step = step;
    v.vol = vol;
    v.samples_left = gate_samples;
    v.release = false;
    v.env_fixed = 0.f;
    v.env_stage = 0;
    v.lfo_phase = 0.f;
    v.lfo_frozen = false;
    v.lfo_mod = 0.f;
    v.vib_phase = 0.f;
    v.vib_delay_left = vib_delay;
    v.sample_wave = std::move(sample_wave);
    v.sample_loop_start = sample_loop_start;
    v.sample_loop_end = sample_loop_end;
    v.note_freq = freq;
    v.in_hold = false;
    v.peak = 0.f;
}

void Engine::consume_event(TrackState& tr, int ch) {
    if (tr.index >= int(tr.events.size())) {
        tr.done = true;
        return;
    }
    const SEvent ev = tr.events[size_t(tr.index++)];

    if (ev.sid < 0x80) {
        const bool chord = (ev.data & 0x80) != 0;
        const bool tie = (ev.data & 0x40) != 0;
        const int flags = ev.data & 0x3F;
        const int midi = ev.sid;
        if (chord) {
            tr.chord_notes.push_back({midi, flags});
            consume_event(tr, ch);
            return;
        }
        auto notes = tr.chord_notes;
        notes.push_back({midi, flags});
        tr.chord_notes.clear();
        start_voice(ch, notes[0].first, notes[0].second, tr, tie);
        for (size_t n = 1; n < notes.size(); ++n) {
            int free = -1;
            for (int i = 0; i < 4; ++i) {
                if (!voices_[size_t(i)].active) {
                    free = i;
                    break;
                }
            }
            if (free >= 0) {
                start_voice(free, notes[n].first, notes[n].second, tr, tie);
            }
        }
        tr.wait = note_duration_beats(flags);
        return;
    }

    if (ev.sid == kSidRest) {
        tr.wait = note_duration_beats(ev.data & 0x3F);
        return;
    }

    handle_control(tr, ev);
    consume_event(tr, ch);
}

void Engine::advance_tracks(float beats) {
    for (size_t ch = 0; ch < tracks_.size(); ++ch) {
        auto& tr = tracks_[ch];
        if (tr.done) {
            continue;
        }
        tr.wait -= beats;
        while (tr.wait <= 1e-9f && !tr.done) {
            consume_event(tr, int(ch));
            if (tr.wait <= 1e-9f && !tr.done && tr.index >= int(tr.events.size())) {
                tr.done = true;
            }
        }
    }
}

void Engine::render_voice(Voice& v, float* mono, int n) {
    std::fill(mono, mono + n, 0.f);
    if (!v.active || !v.instrument) {
        return;
    }
    const Instrument& inst = *v.instrument;
    const float sr = float(sr_);

    auto env_step = [&](float* env_out, float* bank_out) {
        float levels[4];
        for (int i = 0; i < 4; ++i) {
            levels[i] = float(inst.env_levels[size_t(i)]);
        }
        auto step_per = [&](int rate_word) {
            const float units = sonix_rate_units(rate_word);
            const float secs = std::max(0.008f, units / 2500.f);
            return 255.f / (secs * sr);
        };
        float rates[4];
        for (int i = 0; i < 4; ++i) {
            rates[i] = step_per(inst.env_rates[size_t(i)]);
        }

        float env = v.env_fixed;
        int stage = v.env_stage;
        float lfo = v.lfo_phase;
        bool frozen = v.lfo_frozen;
        float mod_held = v.lfo_mod;
        int gate_left = v.samples_left;

        const int lfo_speed = inst.lfo_rate ? inst.lfo_rate : inst.lfo_inc;
        const bool use_lfo =
            (inst.lfo_enable || inst.f_mod || inst.vol_mod || inst.pitch_mod) && lfo_speed > 0 &&
            !inst.mod_table.empty();
        float lfo_step = 0.f;
        if (use_lfo) {
            const float lfo_hz =
                inst.lfo_oneshot ? (0.35f + (lfo_speed / 255.f) * 5.f) : (0.15f + (lfo_speed / 255.f) * 1.2f);
            lfo_step = (lfo_hz * 256.f) / sr;
        }

        for (int i = 0; i < n; ++i) {
            if (gate_left <= 0 && stage < 3) {
                stage = 3;
                v.release = true;
            }
            float target = levels[std::min(stage, 3)];
            if (stage >= 3) {
                target = 0.f;
            }
            float spd = rates[std::min(stage, 3)];
            if (stage >= 3 && spd < 1e-6f) {
                spd = 255.f / (0.05f * sr);
            }
            const float dist = std::fabs(env - target);
            if (dist <= spd) {
                env = target;
                if (stage < 2) {
                    ++stage;
                }
            } else if (env < target) {
                env += spd;
            } else {
                env -= spd;
            }

            float mod = mod_held;
            if (use_lfo && !frozen) {
                mod = inst.mod_table[size_t(int(lfo) & 255)] * 128.f;
                lfo += lfo_step;
                if (inst.lfo_oneshot && lfo >= 254.f) {
                    lfo = 254.f;
                    frozen = true;
                    mod = inst.mod_table[254] * 128.f;
                } else if (!inst.lfo_oneshot && lfo >= 256.f) {
                    lfo -= 256.f;
                }
                mod_held = mod;
            }

            const int env_i = std::clamp(int(env), 0, 255);
            env_out[i] = float(env_i) / 255.f;
            if (bank_out) {
                int filt = (255 - inst.f_base) - ((env_i * inst.f_env) >> 8) + int((mod * inst.f_mod) / 256.f);
                filt = std::clamp(filt, 0, 255);
                bank_out[i] = float(filt >> 2);
            }
            if (gate_left > 0) {
                --gate_left;
            }
        }
        v.env_fixed = env;
        v.env_stage = stage;
        v.lfo_phase = lfo;
        v.lfo_frozen = frozen;
        v.lfo_mod = mod_held;
        v.samples_left = gate_left;
    };

    if (inst.kind == InstrKind::Synth && inst.filter_banks.size() >= 64 * 128) {
        std::vector<float> env(size_t(n)), bank(size_t(n));
        env_step(env.data(), bank.data());
        float peak = 0.f;
        for (int i = 0; i < n; ++i) {
            const double pos = v.pos + v.step * i;
            const int idx = int(std::floor(pos)) & 127;
            const int b0 = std::clamp(int(bank[size_t(i)]), 0, 63);
            const int b1 = std::min(63, b0 + 1);
            const float frac = bank[size_t(i)] - float(b0);
            const float s0 = inst.filter_banks[size_t(b0 * 128 + idx)];
            const float s1 = inst.filter_banks[size_t(b1 * 128 + idx)];
            const float sample = s0 * (1.f - frac) + s1 * frac;
            const float amp = inst.vol_env ? env[size_t(i)] : 1.f;
            mono[i] = sample * amp * v.vol * 1.4f;
            peak = std::max(peak, std::fabs(mono[i]));
        }
        v.pos += v.step * n;
        v.peak = std::max(peak, v.peak * 0.9f);
        if (v.env_fixed <= 1.f &&
            (v.env_stage >= 3 || inst.env_levels[size_t(std::min(v.env_stage, 3))] == 0)) {
            v.active = false;
        }
        return;
    }

    // Sample / 8SVX
    const std::vector<float>& wave = v.sample_wave.empty() ? inst.wave : v.sample_wave;
    const int ls = v.sample_loop_start;
    const int le = v.sample_loop_end;
    const int wlen = int(wave.size());
    if (wlen <= 0) {
        v.active = false;
        return;
    }
    std::vector<float> env(size_t(n));
    env_step(env.data(), nullptr);

    float peak = 0.f;
    double pos = v.pos;
    for (int i = 0; i < n; ++i) {
        double step = v.step;
        if (inst.vib_depth > 0 && inst.vib_rate > 0) {
            if (v.vib_delay_left > 0) {
                --v.vib_delay_left;
            } else {
                const float vib_hz = 0.8f + (inst.vib_rate / 255.f) * 6.f;
                const float depth = (inst.vib_depth / 128.f) * 0.015f;
                step = v.step * double(1.f + depth * std::sin(v.vib_phase));
                v.vib_phase += (6.2831853f * vib_hz) / sr;
            }
        }

        float sample = 0.f;
        if (le > ls) {
            double idx_f;
            if (pos < le) {
                idx_f = std::min(pos, double(wlen) - 1.001);
            } else {
                const double ll = double(le - ls);
                idx_f = ls + std::fmod(pos - ls, ll);
                v.in_hold = true;
            }
            int i0 = int(std::floor(idx_f));
            float frac = float(idx_f - i0);
            int i1 = i0 + 1;
            if (pos >= le) {
                if (i1 >= le) {
                    i1 = ls;
                }
                i0 = std::clamp(i0, ls, le - 1);
            } else {
                i0 = std::clamp(i0, 0, wlen - 1);
                i1 = std::min(i1, wlen - 1);
            }
            sample = wave[size_t(i0)] * (1.f - frac) + wave[size_t(i1)] * frac;
        } else {
            if (pos >= wlen) {
                v.active = false;
                break;
            }
            const int idx = std::clamp(int(pos), 0, wlen - 1);
            sample = wave[size_t(idx)];
        }
        mono[i] = sample * env[size_t(i)] * v.vol;
        peak = std::max(peak, std::fabs(mono[i]));
        pos += step;
    }
    if (le > ls && pos >= le) {
        v.pos = ls + std::fmod(pos - ls, double(le - ls));
    } else {
        v.pos = pos;
    }
    v.peak = std::max(peak, v.peak * 0.9f);
    if (le <= ls && v.pos >= wlen) {
        v.active = false;
    } else if (v.env_fixed <= 1.f &&
               (v.env_stage >= 3 || (v.env_stage >= 2 && inst.env_levels[2] == 0))) {
        v.active = false;
    }
}

void Engine::render(float* interleaved_stereo, int n_frames) {
    std::fill(interleaved_stereo, interleaved_stereo + n_frames * 2, 0.f);
    if (!playing_) {
        return;
    }
    const int grain = 128;
    int pos = 0;
    std::vector<float> mono(size_t(grain));
    while (pos < n_frames) {
        const int g = std::min(grain, n_frames - pos);
        advance_tracks(float(g) / beat_samples_);
        for (auto& v : voices_) {
            if (v.active && v.instrument) {
                render_voice(v, mono.data(), g);
                const int side = kChannelPan[v.channel & 3];
                for (int i = 0; i < g; ++i) {
                    interleaved_stereo[(pos + i) * 2 + side] += mono[size_t(i)];
                }
            }
        }
        pos += g;
    }
    for (int i = 0; i < n_frames * 2; ++i) {
        interleaved_stereo[i] = std::clamp(interleaved_stereo[i] * master_, -1.f, 1.f);
    }
}

Engine::Snapshot Engine::snapshot() const {
    Snapshot s;
    s.title = score_.name;
    s.bpm = bpm_;
    s.playing = playing_;
    s.finished = finished();
    s.tracks = int(tracks_.size());
    for (size_t i = 0; i < 4; ++i) {
        if (i < tracks_.size()) {
            s.track_index[i] = tracks_[i].index;
            s.track_done[i] = tracks_[i].done;
        }
        s.voice_peak[i] = voices_[i].peak;
    }
    return s;
}

}  // namespace smus
