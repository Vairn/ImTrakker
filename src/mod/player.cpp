#include "mod/player.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mod {

Player::Player(Module module) {
    load(std::move(module));
}

void Player::load(Module module) {
    std::lock_guard lock(mutex_);
    module_ = std::move(module);
    order_pos_ = 0;
    row_ = 0;
    tick_ = 0;
    speed_ = 6;
    tempo_ = 125;
    pattern_break_ = -1;
    pattern_jump_ = -1;
    channels_.assign(size_t(module_.channels), ChannelState{});
    playing_ = true;
    finished_ = false;
    row_event_ = true;
    tick_left_ = 0;
}

void Player::restart() {
    std::lock_guard lock(mutex_);
    order_pos_ = 0;
    row_ = 0;
    tick_ = 0;
    pattern_break_ = -1;
    pattern_jump_ = -1;
    tick_left_ = 0;
    for (auto& ch : channels_) {
        ch.sample = nullptr;
        ch.sample_pos = 0.0;
        ch.volume = 0;
        ch.period = 0;
    }
    playing_ = true;
    finished_ = false;
    row_event_ = true;
}

void Player::seek_order(int delta) {
    std::lock_guard lock(mutex_);
    if (module_.song_length <= 0) {
        return;
    }
    order_pos_ = std::clamp(order_pos_ + delta, 0, module_.song_length - 1);
    row_ = 0;
    tick_ = 0;
    pattern_break_ = -1;
    pattern_jump_ = -1;
    tick_left_ = 0;
    row_event_ = true;
}

void Player::toggle_mute(int ch) {
    std::lock_guard lock(mutex_);
    if (ch >= 0 && ch < int(channels_.size())) {
        channels_[size_t(ch)].muted = !channels_[size_t(ch)].muted;
    }
}

void Player::unmute_all() {
    std::lock_guard lock(mutex_);
    for (auto& ch : channels_) {
        ch.muted = false;
    }
}

int Player::samples_per_tick() const {
    return std::max(1, int(std::lround(kSampleRate * 2.5 / double(tempo_))));
}

int Player::pattern_index_unlocked() const {
    if (order_pos_ >= module_.song_length) {
        return 0;
    }
    return module_.orders[size_t(order_pos_)];
}

void Player::trigger(ChannelState& ch, const Note& note) {
    if (note.instrument) {
        const int ins = note.instrument;
        if (ins >= 1 && ins <= 31) {
            ch.instrument = ins;
            ch.sample = &module_.samples[size_t(ins - 1)];
            ch.volume = ch.sample->volume;
            ch.sample_pos = 0.0;
        }
    }
    if (note.period) {
        ch.period = note.period;
        ch.arp_period = note.period;
        ch.last_note = period_to_note(note.period);
        if (note.effect != 0x3) {
            ch.sample_pos = 0.0;
            if (ch.instrument && !ch.sample) {
                ch.sample = &module_.samples[size_t(ch.instrument - 1)];
            }
        }
    }
    ch.effect = note.effect;
    ch.param = note.param;
    if (note.effect || note.param) {
        std::snprintf(ch.last_fx, sizeof(ch.last_fx), "%X%02X", note.effect, note.param);
    } else {
        std::memcpy(ch.last_fx, "...", 4);
    }

    const int fx = note.effect;
    const int p = note.param;
    if (fx == 0xC) {
        ch.volume = std::min(64, p);
    } else if (fx == 0xF) {
        if (p == 0) {
        } else if (p < 32) {
            speed_ = p;
        } else {
            tempo_ = p;
        }
    } else if (fx == 0xD) {
        pattern_break_ = (p >> 4) * 10 + (p & 0x0F);
    } else if (fx == 0xB) {
        pattern_jump_ = p;
    } else if (fx == 0x1 || fx == 0x2) {
        ch.porta_speed = p;
    }
}

void Player::tick_fx(ChannelState& ch) {
    const int fx = ch.effect;
    const int p = ch.param;
    if (fx == 0x0 && p) {
        const int step = tick_ % 3;
        const int base = ch.arp_period ? ch.arp_period : ch.period;
        if (step == 0) {
            ch.period = base;
        } else {
            const int idx = nearest_period_index(base);
            const int semi = (step == 1) ? (p >> 4) : (p & 0x0F);
            const int nidx = std::min(int(kPeriodTable.size()) - 1, idx + semi);
            ch.period = kPeriodTable[size_t(nidx)];
        }
    } else if (fx == 0x1) {
        const int speed = p ? p : ch.porta_speed;
        ch.period = std::max(113, ch.period - speed);
        ch.arp_period = ch.period;
    } else if (fx == 0x2) {
        const int speed = p ? p : ch.porta_speed;
        ch.period = std::min(856, ch.period + speed);
        ch.arp_period = ch.period;
    }
}

void Player::process_tick() {
    if (module_.song_length <= 0) {
        finished_ = true;
        playing_ = false;
        return;
    }
    if (order_pos_ >= module_.song_length) {
        order_pos_ = module_.restart;
        if (order_pos_ >= module_.song_length) {
            finished_ = true;
            playing_ = false;
            return;
        }
    }

    const int pat = module_.orders[size_t(order_pos_)];
    if (tick_ == 0) {
        if (pat < 0 || pat >= module_.pattern_count()) {
            return;
        }
        const auto& row_notes = module_.patterns[size_t(pat)][size_t(row_)];
        pattern_break_ = -1;
        pattern_jump_ = -1;
        for (size_t i = 0; i < channels_.size() && i < row_notes.size(); ++i) {
            trigger(channels_[i], row_notes[i]);
        }
        row_event_ = true;
    } else {
        for (auto& ch : channels_) {
            tick_fx(ch);
        }
    }

    ++tick_;
    if (tick_ >= speed_) {
        tick_ = 0;
        if (pattern_jump_ >= 0) {
            order_pos_ = pattern_jump_ % module_.song_length;
            row_ = 0;
        } else if (pattern_break_ >= 0) {
            ++order_pos_;
            if (order_pos_ >= module_.song_length) {
                order_pos_ = module_.restart;
            }
            row_ = std::min(63, pattern_break_);
        } else {
            ++row_;
            if (row_ >= kRows) {
                row_ = 0;
                ++order_pos_;
                if (order_pos_ >= module_.song_length) {
                    order_pos_ = module_.restart;
                }
            }
        }
    }
}

void Player::mix(float* left, float* right, int n) {
    std::fill(left, left + n, 0.f);
    std::fill(right, right + n, 0.f);

    // Amiga-style alternating hard pan, extended for 6/8 ch.
    static const float pans4[4][2] = {{1, 0}, {0, 1}, {0, 1}, {1, 0}};
    static const float pans8[8][2] = {{1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}};

    const float dt = float(n) / float(kSampleRate);
    for (size_t ci = 0; ci < channels_.size(); ++ci) {
        ChannelState& ch = channels_[ci];
        float pl = 1.f, pr = 0.f;
        if (channels_.size() <= 4) {
            pl = pans4[ci % 4][0];
            pr = pans4[ci % 4][1];
        } else {
            pl = pans8[ci % 8][0];
            pr = pans8[ci % 8][1];
        }

        if (ch.muted || !ch.sample || ch.period <= 0 || ch.volume <= 0) {
            ch.peak *= 0.85f;
            ch.peak_hold_age += dt;
            if (ch.peak_hold_age > 0.7f) {
                ch.peak_hold *= 0.96f;
            }
            for (float& s : ch.scope) {
                s *= 0.92f;
            }
            continue;
        }

        const Sample& samp = *ch.sample;
        const auto& data = samp.data;
        const int length = int(data.size());
        if (length < 2) {
            continue;
        }

        const double step = (kPaulaClock / double(ch.period)) / double(kSampleRate);
        const float vol = ch.volume / 64.f;
        const double rep_start = double(samp.repstart_words * 2);
        const double rep_len = double(samp.replen_words * 2);
        const bool looping = rep_len > 2.0;
        const double loop_end = rep_start + rep_len;

        float peak = 0.f;
        double pos = ch.sample_pos;
        for (int i = 0; i < n; ++i) {
            float v = 0.f;
            if (looping) {
                if (pos >= loop_end) {
                    pos = rep_start + std::fmod(pos - rep_start, rep_len);
                }
                const int idx = int(pos);
                if (idx >= 0 && idx < length) {
                    v = data[size_t(idx)] * vol;
                }
            } else if (pos < length) {
                v = data[size_t(int(pos))] * vol;
            }
            left[i] += v * pl;
            right[i] += v * pr;
            peak = std::max(peak, std::fabs(v));
            pos += step;
        }
        if (looping) {
            if (pos >= loop_end) {
                pos = rep_start + std::fmod(pos - rep_start, rep_len);
            }
            ch.sample_pos = pos;
        } else {
            ch.sample_pos = pos >= length ? double(length) : pos;
        }

        ch.peak = std::max(peak, ch.peak * 0.92f);
        if (peak >= ch.peak_hold) {
            ch.peak_hold = peak;
            ch.peak_hold_age = 0.f;
        } else {
            ch.peak_hold_age += dt;
            if (ch.peak_hold_age > 0.7f) {
                ch.peak_hold *= 0.96f;
            }
        }

        // Scope downsample from this block's contribution is approximate: reuse peaks path
        // by sampling the channel's current position region.
        for (int s = 0; s < kScopeSamples; ++s) {
            const double t = ch.sample_pos - step * (kScopeSamples - s);
            double tp = t;
            float v = 0.f;
            if (looping && length > 0) {
                if (tp >= loop_end) {
                    tp = rep_start + std::fmod(std::max(0.0, tp - rep_start), rep_len);
                }
                if (tp < 0) {
                    tp = 0;
                }
                const int idx = std::clamp(int(tp), 0, length - 1);
                v = data[size_t(idx)] * vol;
            } else if (tp >= 0 && tp < length) {
                v = data[size_t(int(tp))] * vol;
            }
            ch.scope[size_t(s)] = v;
        }
    }

    const float gain = 0.35f * (4.f / float(std::max(4, int(channels_.size()))));
    for (int i = 0; i < n; ++i) {
        left[i] *= gain;
        right[i] *= gain;
    }
}

void Player::render(float* interleaved_stereo, int n_frames) {
    std::fill(interleaved_stereo, interleaved_stereo + n_frames * 2, 0.f);
    if (!playing_) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (!playing_) {
        return;
    }

    std::vector<float> left(static_cast<size_t>(n_frames), 0.f);
    std::vector<float> right(static_cast<size_t>(n_frames), 0.f);
    int write = 0;
    int remaining = n_frames;
    while (remaining > 0 && playing_) {
        if (tick_left_ <= 0) {
            process_tick();
            tick_left_ = samples_per_tick();
        }
        const int n = std::min(remaining, tick_left_);
        mix(left.data() + write, right.data() + write, n);
        write += n;
        remaining -= n;
        tick_left_ -= n;
    }
    for (int i = 0; i < n_frames; ++i) {
        interleaved_stereo[i * 2] = left[size_t(i)];
        interleaved_stereo[i * 2 + 1] = right[size_t(i)];
    }
}

Player::Snapshot Player::snapshot() {
    std::lock_guard lock(mutex_);
    Snapshot s;
    s.order_pos = order_pos_;
    s.pattern_index = pattern_index_unlocked();
    s.row = row_;
    s.tick = tick_;
    s.speed = speed_;
    s.tempo = tempo_;
    s.song_length = module_.song_length;
    s.channels = module_.channels;
    s.playing = playing_;
    s.row_event = row_event_;
    row_event_ = false;
    s.title = module_.title;
    s.magic = module_.magic;
    s.channels_state = channels_;
    s.orders = module_.orders;
    if (s.pattern_index >= 0 && s.pattern_index < module_.pattern_count()) {
        s.pattern = module_.patterns[size_t(s.pattern_index)];
    }
    return s;
}

}  // namespace mod
