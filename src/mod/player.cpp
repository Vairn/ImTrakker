#include "mod/player.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mod {
namespace {

constexpr int kMinPeriod = 113;
constexpr int kMaxPeriod = 856;

int clamp_period(int p) {
    return std::clamp(p, kMinPeriod, kMaxPeriod);
}

int clamp_vol(int v) {
    return std::clamp(v, 0, 64);
}

// Linear-interpolated sample read with optional Amiga-style loop.
float fetch_sample(const std::vector<float>& data, int length, double pos, bool looping,
                   double rep_start, double rep_len, double loop_end) {
    if (length < 2) {
        return 0.f;
    }
    if (looping) {
        if (pos >= loop_end) {
            pos = rep_start + std::fmod(std::max(0.0, pos - rep_start), rep_len);
        }
        if (pos < 0.0) {
            pos = 0.0;
        }
        int i0 = int(pos);
        const double frac = pos - double(i0);
        int i1 = i0 + 1;
        if (i0 >= length) {
            return 0.f;
        }
        if (double(i1) >= loop_end) {
            i1 = int(rep_start);
        }
        i0 = std::clamp(i0, 0, length - 1);
        i1 = std::clamp(i1, 0, length - 1);
        return data[size_t(i0)] * float(1.0 - frac) + data[size_t(i1)] * float(frac);
    }
    if (pos < 0.0 || pos >= double(length)) {
        return 0.f;
    }
    const int i0 = int(pos);
    const double frac = pos - double(i0);
    const int i1 = std::min(i0 + 1, length - 1);
    return data[size_t(i0)] * float(1.0 - frac) + data[size_t(i1)] * float(frac);
}

}  // namespace

Player::Player(Module module) {
    load(std::move(module));
}

void Player::load(Module module) {
    std::lock_guard lock(mutex_);
    module_ = std::move(module);
    order_pos_ = 0;
    row_ = 0;
    tick_ = 0;
    speed_ = std::max(1, module_.initial_speed);
    tempo_ = std::max(32, module_.initial_tempo);
    pattern_break_ = -1;
    pattern_jump_ = -1;
    pattern_delay_ = 0;
    pattern_loop_ = false;
    pattern_loop_to_ = 0;
    filter_on_ = true;
    filter_l_ = filter_r_ = 0.f;
    channels_.assign(size_t(std::max(1, module_.channels)), ChannelState{});
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
    pattern_delay_ = 0;
    pattern_loop_ = false;
    pattern_loop_to_ = 0;
    tick_left_ = 0;
    filter_l_ = filter_r_ = 0.f;
    for (auto& ch : channels_) {
        const bool muted = ch.muted;
        ch = ChannelState{};
        ch.muted = muted;
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
    pattern_delay_ = 0;
    pattern_loop_ = false;
    tick_left_ = 0;
    row_event_ = true;
    for (auto& ch : channels_) {
        ch.loop_count = 0;
        ch.loop_row = 0;
    }
}

void Player::seek_row(int order, int row) {
    std::lock_guard lock(mutex_);
    if (module_.song_length <= 0) {
        return;
    }
    order_pos_ = std::clamp(order, 0, module_.song_length - 1);
    const int pat = module_.orders[size_t(order_pos_)];
    const int pat_rows =
        (pat >= 0 && pat < module_.pattern_count()) ? int(module_.patterns[size_t(pat)].size()) : kRows;
    row_ = std::clamp(row, 0, std::max(0, pat_rows - 1));
    tick_ = 0;
    pattern_break_ = -1;
    pattern_jump_ = -1;
    pattern_delay_ = 0;
    pattern_loop_ = false;
    tick_left_ = 0;
    row_event_ = true;
}

void Player::audition(int instrument_1based, int period) {
    std::lock_guard lock(mutex_);
    if (instrument_1based < 1 || instrument_1based > int(module_.samples.size()) || period <= 0) {
        audition_active_ = false;
        return;
    }
    audition_sample_ = &module_.samples[size_t(instrument_1based - 1)];
    audition_pos_ = 0.0;
    audition_period_ = period;
    audition_volume_ = std::clamp(audition_sample_->volume, 0, 64);
    audition_active_ = true;
}

void Player::stop_audition() {
    std::lock_guard lock(mutex_);
    audition_active_ = false;
    audition_sample_ = nullptr;
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

void Player::set_channel_count(int channels) {
    std::lock_guard lock(mutex_);
    channels = std::clamp(channels, 2, 8);
    if (channels == module_.channels) {
        module_.magic = magic_for_channels(channels);
        return;
    }
    for (auto& pat : module_.patterns) {
        for (auto& row : pat) {
            row.resize(size_t(channels));
        }
    }
    module_.channels = channels;
    module_.magic = magic_for_channels(channels);

    std::vector<ChannelState> next;
    next.resize(size_t(channels));
    for (size_t i = 0; i < next.size() && i < channels_.size(); ++i) {
        next[i] = channels_[i];
    }
    channels_ = std::move(next);
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

int Player::vib_wave(ChannelState& /*ch*/, int pos, int wave) const {
    const int p = (pos >> 2) & 0x1F;
    switch (wave & 3) {
        case 0:  // sine
            return kVibTable[size_t(p)];
        case 1:  // ramp down
            return (pos & 0x80) ? (255 - (p << 3)) : (p << 3);
        case 2:  // square
        case 3:
            return 255;
        default:
            return kVibTable[size_t(p)];
    }
}

void Player::do_tone_porta(ChannelState& ch) {
    if (!ch.wanted_period || !ch.porta_speed) {
        ch.out_period = ch.glissando ? period_for_note(nearest_period_index_ft(ch.period, ch.finetune), ch.finetune)
                                     : ch.period;
        return;
    }
    if (ch.tone_porta_dir) {
        ch.period -= ch.porta_speed;
        if (ch.period <= ch.wanted_period) {
            ch.period = ch.wanted_period;
            ch.wanted_period = 0;
        }
    } else {
        ch.period += ch.porta_speed;
        if (ch.period >= ch.wanted_period) {
            ch.period = ch.wanted_period;
            ch.wanted_period = 0;
        }
    }
    if (ch.glissando) {
        ch.out_period = period_for_note(nearest_period_index_ft(ch.period, ch.finetune), ch.finetune);
    } else {
        ch.out_period = ch.period;
    }
}

void Player::do_vibrato(ChannelState& ch) {
    const int wave = ch.wave_control & 3;
    int delta = vib_wave(ch, ch.vib_pos, wave);
    delta = (delta * ch.vib_depth) >> 7;
    if (ch.vib_pos & 0x80) {
        ch.out_period = ch.period - delta;
    } else {
        ch.out_period = ch.period + delta;
    }
    ch.vib_pos = (ch.vib_pos + (ch.vib_speed << 2)) & 0xFF;
}

void Player::do_tremolo(ChannelState& ch) {
    const int wave = (ch.wave_control >> 4) & 3;
    int delta = vib_wave(ch, ch.trem_pos, wave);
    delta = (delta * ch.trem_depth) >> 6;
    int vol = ch.volume;
    if (ch.trem_pos & 0x80) {
        vol -= delta;
    } else {
        vol += delta;
    }
    ch.out_volume = clamp_vol(vol);
    ch.trem_pos = (ch.trem_pos + (ch.trem_speed << 2)) & 0xFF;
}

void Player::do_vol_slide(ChannelState& ch, int param) {
    const int up = (param >> 4) & 0x0F;
    const int down = param & 0x0F;
    if (up) {
        ch.volume = clamp_vol(ch.volume + up);
    } else {
        ch.volume = clamp_vol(ch.volume - down);
    }
    ch.out_volume = ch.volume;
}

void Player::do_arpeggio(ChannelState& ch) {
    const int step = tick_ % 3;
    const int p = ch.param;
    const int base_idx = nearest_period_index_ft(ch.period, ch.finetune);
    if (step == 0) {
        ch.out_period = ch.period;
    } else {
        const int semi = (step == 1) ? ((p >> 4) & 0x0F) : (p & 0x0F);
        const int nidx = std::min(35, base_idx + semi);
        ch.out_period = period_for_note(nidx, ch.finetune);
    }
}

void Player::do_retrig(ChannelState& ch) {
    if (!ch.sample) {
        return;
    }
    ch.sample_pos = 0.0;
}

void Player::update_funk(ChannelState& ch) {
    if (!ch.funk_speed || !ch.sample) {
        return;
    }
    ch.funk_offset += kFunkTable[size_t(ch.funk_speed & 0x0F)];
    if (!(ch.funk_offset & 0x80)) {
        return;
    }
    ch.funk_offset = 0;
    Sample* samp = const_cast<Sample*>(ch.sample);
    const int length = int(samp->wave.size());
    if (length <= 0) {
        return;
    }
    const int rep_start = samp->repstart_words * 2;
    const int rep_len = samp->replen_words * 2;
    if (rep_len <= 2) {
        return;
    }
    const int loop_end = rep_start + rep_len;
    ++ch.funk_pos;
    if (ch.funk_pos < rep_start || ch.funk_pos >= loop_end) {
        ch.funk_pos = rep_start;
    }
    if (ch.funk_pos >= 0 && ch.funk_pos < length) {
        // Approximate Amiga `not.b` on signed 8-bit PCM stored as float.
        samp->wave[size_t(ch.funk_pos)] = -samp->wave[size_t(ch.funk_pos)];
    }
}

void Player::trigger(ChannelState& ch, const Note& note, bool force_retrig) {
    const int fx = note.effect;
    const int p = note.param;
    const bool is_porta = (fx == 0x3 || fx == 0x5);
    const bool is_delay = (fx == 0xE && ((p >> 4) & 0x0F) == 0xD && (p & 0x0F) != 0);

    if (is_delay && !force_retrig) {
        ch.delay_note = true;
        ch.delayed = note;
        // Still remember instrument / effect for display; note starts later.
        ch.effect = fx;
        ch.param = p;
        if (fx || p) {
            std::snprintf(ch.last_fx, sizeof(ch.last_fx), "%X%02X", fx, p);
        }
        return;
    }
    ch.delay_note = false;

    if (note.instrument) {
        const int ins = note.instrument;
        if (ins >= 1 && ins <= int(module_.samples.size())) {
            ch.instrument = ins;
            ch.sample = &module_.samples[size_t(ins - 1)];
            ch.volume = clamp_vol(ch.sample->volume);
            ch.out_volume = ch.volume;
            ch.finetune = ch.sample->finetune;
            ch.funk_pos = ch.sample->repstart_words * 2;
        }
    }

    if (note.period) {
        const int note_idx = nearest_period_index(note.period);
        const int tuned = period_for_note(note_idx, ch.finetune);
        ch.last_note = period_to_note(note.period);

        if (is_porta) {
            ch.wanted_period = tuned;
            if (ch.wanted_period == ch.period) {
                ch.wanted_period = 0;
            } else {
                ch.tone_porta_dir = (ch.wanted_period < ch.period) ? 1 : 0;
            }
        } else {
            ch.period = tuned;
            ch.out_period = tuned;
            ch.sample_pos = 0.0;
            if (!(ch.wave_control & 4)) {
                ch.vib_pos = 0;
            }
            if (!(ch.wave_control & 0x40)) {
                ch.trem_pos = 0;
            }
            if (ch.instrument && !ch.sample) {
                ch.sample = &module_.samples[size_t(ch.instrument - 1)];
            }
        }
    }

    if (force_retrig && note.period && !is_porta) {
        ch.sample_pos = 0.0;
    }

    ch.effect = fx;
    ch.param = p;
    if (fx || p) {
        std::snprintf(ch.last_fx, sizeof(ch.last_fx), "%X%02X", fx, p);
    } else {
        std::memcpy(ch.last_fx, "...", 4);
    }

    apply_row_fx(ch, note);
}

void Player::apply_row_fx(ChannelState& ch, const Note& note) {
    const int fx = note.effect;
    const int p = note.param;
    const int ex = (p >> 4) & 0x0F;
    const int ey = p & 0x0F;

    ch.out_period = ch.period;
    ch.out_volume = ch.volume;

    switch (fx) {
        case 0x3:
            if (p) {
                ch.porta_speed = p;
            }
            break;
        case 0x4:
            if (ex) {
                ch.vib_speed = ex;
            }
            if (ey) {
                ch.vib_depth = ey;
            }
            do_vibrato(ch);
            break;
        case 0x5:
            // tone porta + vol slide: porta uses prior speed; vol slide on ticks > 0
            break;
        case 0x6:
            do_vibrato(ch);
            break;
        case 0x7:
            if (ex) {
                ch.trem_speed = ex;
            }
            if (ey) {
                ch.trem_depth = ey;
            }
            do_tremolo(ch);
            break;
        case 0x9: {
            if (p) {
                ch.sample_offset = p;
            }
            if (ch.sample) {
                const double off = double(ch.sample_offset) * 256.0;
                if (off >= double(ch.sample->wave.size())) {
                    // PT quirk: offset past end → silence
                    ch.sample_pos = double(ch.sample->wave.size());
                } else {
                    ch.sample_pos = off;
                }
            }
            break;
        }
        case 0xB:
            pattern_jump_ = p;
            break;
        case 0xC:
            ch.volume = clamp_vol(p);
            ch.out_volume = ch.volume;
            break;
        case 0xD:
            pattern_break_ = ex * 10 + ey;
            break;
        case 0xE:
            switch (ex) {
                case 0x0:  // filter
                    filter_on_ = (ey & 1) == 0;
                    break;
                case 0x1:  // fine porta up
                    ch.period = clamp_period(ch.period - ey);
                    ch.out_period = ch.period;
                    break;
                case 0x2:  // fine porta down
                    ch.period = clamp_period(ch.period + ey);
                    ch.out_period = ch.period;
                    break;
                case 0x3:  // glissando
                    ch.glissando = ey != 0;
                    break;
                case 0x4:  // vibrato waveform
                    ch.wave_control = (ch.wave_control & 0xF0) | (ey & 0x0F);
                    break;
                case 0x5:  // set finetune
                    ch.finetune = (ey >= 8) ? ey - 16 : ey;
                    if (note.period) {
                        const int idx = nearest_period_index(note.period);
                        ch.period = period_for_note(idx, ch.finetune);
                        ch.out_period = ch.period;
                    }
                    break;
                case 0x6:  // pattern loop
                    if (ey == 0) {
                        ch.loop_row = row_;
                    } else if (ch.loop_count == 0) {
                        ch.loop_count = ey;
                        pattern_loop_ = true;
                        pattern_loop_to_ = ch.loop_row;
                    } else {
                        --ch.loop_count;
                        if (ch.loop_count != 0) {
                            pattern_loop_ = true;
                            pattern_loop_to_ = ch.loop_row;
                        }
                    }
                    break;
                case 0x7:  // tremolo waveform
                    ch.wave_control = (ch.wave_control & 0x0F) | ((ey & 0x0F) << 4);
                    break;
                case 0x9:  // retrig — handled on later ticks; also on tick0 if no note
                    if (ey && tick_ == 0 && !note.period) {
                        do_retrig(ch);
                    }
                    break;
                case 0xA:  // fine vol up
                    ch.volume = clamp_vol(ch.volume + ey);
                    ch.out_volume = ch.volume;
                    break;
                case 0xB:  // fine vol down
                    ch.volume = clamp_vol(ch.volume - ey);
                    ch.out_volume = ch.volume;
                    break;
                case 0xC:  // note cut
                    if (ey == 0) {
                        ch.volume = 0;
                        ch.out_volume = 0;
                    }
                    break;
                case 0xD:  // note delay — trigger already deferred
                    break;
                case 0xE:  // pattern delay
                    if (pattern_delay_ == 0) {
                        pattern_delay_ = ey;
                    }
                    break;
                case 0xF:  // invert loop speed
                    ch.funk_speed = ey;
                    break;
                default:
                    break;
            }
            break;
        case 0xF:
            if (p == 0) {
                // F00: stop (ProTracker quirk varies; treat as stop)
                playing_ = false;
                finished_ = true;
            } else if (p < 32) {
                speed_ = p;
            } else {
                tempo_ = p;
            }
            break;
        default:
            break;
    }
}

void Player::tick_fx(ChannelState& ch) {
    update_funk(ch);

    const int fx = ch.effect;
    const int p = ch.param;
    const int ex = (p >> 4) & 0x0F;
    const int ey = p & 0x0F;

    ch.out_period = ch.period;
    ch.out_volume = ch.volume;

    // Delayed note trigger (EDx)
    if (ch.delay_note && fx == 0xE && ex == 0xD && tick_ == ey) {
        const Note n = ch.delayed;
        ch.delay_note = false;
        // Clear delay so trigger plays immediately
        Note play = n;
        play.effect = 0;
        play.param = 0;
        trigger(ch, play, true);
        ch.effect = n.effect;
        ch.param = n.param;
        return;
    }

    switch (fx) {
        case 0x0:
            if (p) {
                do_arpeggio(ch);
            }
            break;
        case 0x1:
            if (tick_ > 0) {
                ch.period = clamp_period(ch.period - p);
                ch.out_period = ch.period;
            }
            break;
        case 0x2:
            if (tick_ > 0) {
                ch.period = clamp_period(ch.period + p);
                ch.out_period = ch.period;
            }
            break;
        case 0x3:
            if (p) {
                ch.porta_speed = p;
            }
            if (tick_ > 0) {
                do_tone_porta(ch);
            }
            break;
        case 0x4:
            if (ex) {
                ch.vib_speed = ex;
            }
            if (ey) {
                ch.vib_depth = ey;
            }
            do_vibrato(ch);
            break;
        case 0x5:
            if (tick_ > 0) {
                do_tone_porta(ch);
                do_vol_slide(ch, p);
            }
            break;
        case 0x6:
            do_vibrato(ch);
            if (tick_ > 0) {
                do_vol_slide(ch, p);
            }
            break;
        case 0x7:
            if (ex) {
                ch.trem_speed = ex;
            }
            if (ey) {
                ch.trem_depth = ey;
            }
            do_tremolo(ch);
            break;
        case 0xA:
            if (tick_ > 0) {
                do_vol_slide(ch, p);
            }
            break;
        case 0xE:
            switch (ex) {
                case 0x9:  // retrig
                    if (ey && (tick_ % ey) == 0) {
                        do_retrig(ch);
                    }
                    break;
                case 0xC:  // note cut
                    if (tick_ == ey) {
                        ch.volume = 0;
                        ch.out_volume = 0;
                    }
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

void Player::advance_row() {
    // Pattern loop (E6) takes priority over normal advance; Bxx/Dxx still win if set.
    if (pattern_loop_ && pattern_jump_ < 0 && pattern_break_ < 0) {
        row_ = std::max(0, pattern_loop_to_);
        pattern_loop_ = false;
        return;
    }
    pattern_loop_ = false;

    // Pattern delay: re-play same row (re-triggers notes, like PT).
    if (pattern_delay_ > 0) {
        --pattern_delay_;
        return;
    }

    if (pattern_jump_ >= 0) {
        order_pos_ = pattern_jump_ % std::max(1, module_.song_length);
        // Bxx + Dxx on same row: jump to position, then start at break row.
        if (pattern_break_ >= 0) {
            const int pat = module_.orders[size_t(order_pos_)];
            const int pat_rows =
                (pat >= 0 && pat < module_.pattern_count()) ? int(module_.patterns[size_t(pat)].size())
                                                            : kRows;
            row_ = std::min(std::max(0, pat_rows - 1), pattern_break_);
        } else {
            row_ = 0;
        }
        for (auto& ch : channels_) {
            ch.loop_count = 0;
            ch.loop_row = 0;
        }
    } else if (pattern_break_ >= 0) {
        ++order_pos_;
        if (order_pos_ >= module_.song_length) {
            order_pos_ = module_.restart;
        }
        const int pat = (order_pos_ < module_.song_length) ? module_.orders[size_t(order_pos_)] : -1;
        const int pat_rows =
            (pat >= 0 && pat < module_.pattern_count()) ? int(module_.patterns[size_t(pat)].size())
                                                        : kRows;
        row_ = std::min(std::max(0, pat_rows - 1), pattern_break_);
        for (auto& ch : channels_) {
            ch.loop_count = 0;
            ch.loop_row = 0;
        }
    } else {
        const int pat = pattern_index_unlocked();
        const int pat_rows =
            (pat >= 0 && pat < module_.pattern_count()) ? int(module_.patterns[size_t(pat)].size()) : 0;
        ++row_;
        if (row_ >= pat_rows) {
            row_ = 0;
            ++order_pos_;
            if (order_pos_ >= module_.song_length) {
                order_pos_ = module_.restart;
            }
            for (auto& ch : channels_) {
                ch.loop_count = 0;
                ch.loop_row = 0;
            }
        }
    }
    pattern_break_ = -1;
    pattern_jump_ = -1;
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
    const int pat_rows =
        (pat >= 0 && pat < module_.pattern_count()) ? int(module_.patterns[size_t(pat)].size()) : 0;

    if (tick_ == 0) {
        if (pat < 0 || pat >= module_.pattern_count() || pat_rows <= 0 || row_ >= pat_rows) {
            return;
        }
        const auto& row_notes = module_.patterns[size_t(pat)][size_t(row_)];
        pattern_break_ = -1;
        pattern_jump_ = -1;
        pattern_loop_ = false;
        for (size_t i = 0; i < channels_.size() && i < row_notes.size(); ++i) {
            auto& ch = channels_[i];
            update_funk(ch);
            trigger(ch, row_notes[i], false);
            // Tick-0 arpeggio / porta-up etc. handled inside trigger / apply_row_fx.
            // Also run tick-0 note-cut EC0 already done; arpeggio on tick 0:
            if (ch.effect == 0x0 && ch.param) {
                do_arpeggio(ch);
            } else if (ch.effect == 0x1 || ch.effect == 0x2) {
                ch.out_period = ch.period;
            }
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
        advance_row();
    }
}

void Player::mix(float* left, float* right, int n) {
    std::fill(left, left + n, 0.f);
    std::fill(right, right + n, 0.f);

    // Amiga-style alternating hard pan, extended for up to 16 ch.
    static const float pans8[8][2] = {{1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}};

    const float dt = float(n) / float(kSampleRate);
    for (size_t ci = 0; ci < channels_.size(); ++ci) {
        ChannelState& ch = channels_[ci];
        float pl = pans8[ci % 8][0];
        float pr = pans8[ci % 8][1];

        const int use_period = ch.out_period > 0 ? ch.out_period : ch.period;
        const int use_vol = ch.out_volume;

        if (ch.muted || !ch.sample || use_period <= 0 || use_vol <= 0) {
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
        const auto& data = samp.wave;
        const int length = int(data.size());
        if (length < 2) {
            continue;
        }

        const double step = (kPaulaClock / double(use_period)) / double(kSampleRate);
        const float vol = use_vol / 64.f;
        const double rep_start = double(samp.repstart_words * 2);
        const double rep_len = double(samp.replen_words * 2);
        const bool looping = rep_len > 2.0;
        const double loop_end = rep_start + rep_len;

        float peak = 0.f;
        double pos = ch.sample_pos;
        for (int i = 0; i < n; ++i) {
            const float v =
                fetch_sample(data, length, pos, looping, rep_start, rep_len, loop_end) * vol;
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

        for (int s = 0; s < kScopeSamples; ++s) {
            const double t = ch.sample_pos - step * (kScopeSamples - s);
            ch.scope[size_t(s)] =
                fetch_sample(data, length, t, looping, rep_start, rep_len, loop_end) * vol;
        }
    }

    const float gain = 0.35f * (4.f / float(std::max(4, int(channels_.size()))));
    // Amiga LED filter ≈ one-pole LPF ~3.3 kHz when on (default).
    constexpr float kFilterA = 0.37f;
    for (int i = 0; i < n; ++i) {
        float l = left[i] * gain;
        float r = right[i] * gain;
        if (filter_on_) {
            filter_l_ += (l - filter_l_) * kFilterA;
            filter_r_ += (r - filter_r_) * kFilterA;
            l = filter_l_;
            r = filter_r_;
        }
        left[i] = l;
        right[i] = r;
    }
}

void Player::mix_audition(float* left, float* right, int n) {
    if (!audition_active_ || !audition_sample_ || audition_period_ <= 0) {
        return;
    }
    const Sample& samp = *audition_sample_;
    const auto& data = samp.wave;
    const int length = int(data.size());
    if (length < 2) {
        audition_active_ = false;
        return;
    }
    const double step = (kPaulaClock / double(audition_period_)) / double(kSampleRate);
    const float vol = audition_volume_ / 64.f * 0.45f;
    const double rep_start = double(samp.repstart_words * 2);
    const double rep_len = double(samp.replen_words * 2);
    const bool looping = rep_len > 2.0;
    const double loop_end = rep_start + rep_len;
    double pos = audition_pos_;
    bool alive = false;
    for (int i = 0; i < n; ++i) {
        const float v =
            fetch_sample(data, length, pos, looping, rep_start, rep_len, loop_end) * vol;
        if (looping || pos < length) {
            alive = true;
        }
        left[i] += v;
        right[i] += v;
        pos += step;
    }
    if (looping) {
        if (pos >= loop_end) {
            pos = rep_start + std::fmod(pos - rep_start, rep_len);
        }
        audition_pos_ = pos;
    } else {
        audition_pos_ = pos;
        if (!alive || pos >= length) {
            audition_active_ = false;
        }
    }
}

void Player::render(float* interleaved_stereo, int n_frames) {
    std::fill(interleaved_stereo, interleaved_stereo + n_frames * 2, 0.f);
    std::lock_guard lock(mutex_);

    std::vector<float> left(static_cast<size_t>(n_frames), 0.f);
    std::vector<float> right(static_cast<size_t>(n_frames), 0.f);

    if (playing_) {
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
    }

    mix_audition(left.data(), right.data(), n_frames);

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
