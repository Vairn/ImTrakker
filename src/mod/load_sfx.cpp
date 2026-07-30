#include "mod/module.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace mod {

static uint16_t be16(const uint8_t* p) {
    return uint16_t((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static std::string asciiz(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len] != 0) {
        ++len;
    }
    while (len > 0 && p[len - 1] == ' ') {
        --len;
    }
    return std::string(reinterpret_cast<const char*>(p), len);
}

// SoundFX effect set → closest ProTracker commands (libxmp / Flod mapping).
static void translate_sfx_effect(Note& n) {
    const int fxt = n.effect;
    const int fxp = n.param;
    switch (fxt) {
        case 0x1:  // arpeggio
            n.effect = 0x0;
            n.param = fxp;
            break;
        case 0x2:  // pitch bend: high nibble = down, low = up
            if (fxp >> 4) {
                n.effect = 0x2;
                n.param = fxp >> 4;
            } else if (fxp & 0x0F) {
                n.effect = 0x1;
                n.param = fxp & 0x0F;
            } else {
                n.effect = 0;
                n.param = 0;
            }
            break;
        case 0x3:  // LED / filter on
            n.effect = 0xE;
            n.param = 0x01;
            break;
        case 0x4:  // LED / filter off
            n.effect = 0xE;
            n.param = 0x00;
            break;
        case 0x5:  // add to volume (applied once in SFX) → fine vol up
            n.effect = 0xE;
            n.param = 0xA0 | std::min(0x0F, fxp);
            break;
        case 0x6:  // subtract from volume → fine vol down
            n.effect = 0xE;
            n.param = 0xB0 | std::min(0x0F, fxp);
            break;
        case 0x7:  // add semitones (SFX-specific) — no PT equivalent
        case 0x8:  // subtract semitones
        default:
            if (fxt != 0) {
                n.effect = 0;
                n.param = 0;
            }
            break;
    }
}

static Module load_sfx_nins(std::vector<uint8_t> data, std::filesystem::path path, int nins) {
    if (nins != 15 && nins != 31) {
        throw std::runtime_error("SoundFX: invalid instrument count");
    }

    const size_t size_tbl_bytes = size_t(nins) * 4;
    const size_t magic_off = size_tbl_bytes;
    const size_t hdr_off = magic_off + 4 + 2 + 14;          // after SONG/SO31 + delay + pad
    const size_t song_off = hdr_off + size_t(nins) * 30;    // song length
    const size_t orders_off = song_off + 2;
    const size_t pat_base = orders_off + 128;

    if (data.size() < pat_base + 1024) {
        throw std::runtime_error("too small for SoundFX module");
    }

    const char* expect = (nins == 15) ? "SONG" : "SO31";
    if (std::memcmp(data.data() + magic_off, expect, 4) != 0) {
        throw std::runtime_error("not a SoundFX module");
    }

    const int delay = int(be16(data.data() + magic_off + 4));
    if (delay < 178) {
        throw std::runtime_error("SoundFX: invalid delay/tempo");
    }

    Module mod;
    mod.path = std::move(path);
    mod.magic = (nins == 15) ? "SONG" : "SO31";
    mod.channels = 4;
    mod.initial_speed = 6;
    // Default delay 14565 ≈ 122 BPM (libxmp).
    mod.initial_tempo = std::max(32, (14565 * 122) / delay);

    std::vector<uint32_t> sizes(static_cast<size_t>(nins), 0u);
    for (int i = 0; i < nins; ++i) {
        sizes[size_t(i)] = be32(data.data() + size_t(i) * 4);
    }

    mod.samples.assign(31, Sample{});
    for (int i = 0; i < nins; ++i) {
        const size_t off = hdr_off + size_t(i) * 30;
        Sample& s = mod.samples[size_t(i)];
        s.name = asciiz(data.data() + off, 22);
        const int hdr_len_words = int(be16(data.data() + off + 22));
        int ft = data[off + 24] & 0x0F;
        if (ft >= 8) {
            ft -= 16;
        }
        s.finetune = ft;
        s.volume = std::clamp(int(data[off + 25]), 0, 64);
        const int loop_start_bytes = int(be16(data.data() + off + 26));
        s.replen_words = int(be16(data.data() + off + 28));
        s.repstart_words = loop_start_bytes / 2;

        // PCM length comes from the size table (header length can disagree).
        const uint32_t nbytes = sizes[size_t(i)];
        if (nbytes > 2) {
            s.length_words = int(nbytes / 2);
        } else if (hdr_len_words > 1) {
            s.length_words = hdr_len_words;
        } else {
            s.length_words = 0;
        }
    }
    for (int i = nins; i < 31; ++i) {
        mod.samples[size_t(i)].volume = 64;
        mod.samples[size_t(i)].replen_words = 1;
        mod.samples[size_t(i)].wave = {0.f, 0.f};
    }

    mod.song_length = data[song_off];
    mod.restart = data[song_off + 1];
    if (mod.song_length < 1 || mod.song_length > 128) {
        throw std::runtime_error("SoundFX: bad song length");
    }
    if (mod.restart >= mod.song_length) {
        mod.restart = 0;
    }

    mod.orders.assign(128, 0);
    for (int i = 0; i < 128; ++i) {
        mod.orders[size_t(i)] = data[orders_off + size_t(i)];
    }

    int npat = 0;
    for (int i = 0; i < mod.song_length; ++i) {
        npat = std::max(npat, mod.orders[size_t(i)]);
    }
    ++npat;

    constexpr int ch = 4;
    constexpr int row_bytes = ch * 4;
    constexpr int pat_bytes = kRows * row_bytes;
    if (pat_base + size_t(npat) * size_t(pat_bytes) > data.size()) {
        throw std::runtime_error("truncated SoundFX pattern data");
    }

    mod.patterns.resize(size_t(npat));
    for (int p = 0; p < npat; ++p) {
        mod.patterns[size_t(p)].resize(kRows);
        for (int row = 0; row < kRows; ++row) {
            mod.patterns[size_t(p)][size_t(row)].resize(ch);
            for (int c = 0; c < ch; ++c) {
                const size_t i = pat_base + size_t(p) * size_t(pat_bytes) +
                                 size_t(row) * size_t(row_bytes) + size_t(c) * 4;
                const uint8_t b0 = data[i];
                const uint8_t b1 = data[i + 1];
                const uint8_t b2 = data[i + 2];
                const uint8_t b3 = data[i + 3];
                Note& n = mod.patterns[size_t(p)][size_t(row)][size_t(c)];
                n.period = ((b0 & 0x0F) << 8) | b1;
                n.instrument = (b0 & 0xF0) | ((b2 & 0xF0) >> 4);
                n.effect = b2 & 0x0F;
                n.param = b3;
                if (n.effect || n.param) {
                    translate_sfx_effect(n);
                }
            }
        }
    }

    size_t sample_pos = pat_base + size_t(npat) * size_t(pat_bytes);
    for (int i = 0; i < nins; ++i) {
        Sample& s = mod.samples[size_t(i)];
        const size_t nbytes = sizes[size_t(i)];
        if (nbytes <= 2) {
            s.wave = {0.f, 0.f};
            s.length_words = 0;
            continue;
        }
        if (sample_pos + nbytes > data.size()) {
            s.wave.assign(2, 0.f);
            s.length_words = 0;
            break;
        }
        s.wave.resize(nbytes);
        for (size_t b = 0; b < nbytes; ++b) {
            s.wave[b] = float(int8_t(data[sample_pos + b])) / 128.f;
        }
        if (nbytes >= 2) {
            s.wave[0] = 0.f;
            s.wave[1] = 0.f;
        }
        s.length_words = int(nbytes / 2);
        // Clamp loop to sample
        const int len_bytes = int(nbytes);
        if (s.replen_words <= 1) {
            s.repstart_words = 0;
            s.replen_words = 1;
        } else if (s.repstart_words * 2 + s.replen_words * 2 > len_bytes) {
            if (s.repstart_words * 2 >= len_bytes) {
                s.repstart_words = 0;
                s.replen_words = 1;
            } else {
                s.replen_words = (len_bytes - s.repstart_words * 2) / 2;
            }
        }
        sample_pos += nbytes;
    }

    if (mod.title.empty() && !mod.path.empty()) {
        mod.title = mod.path.stem().string();
    }
    return mod;
}

Module load_sfx(std::vector<uint8_t> data, std::filesystem::path path) {
    if (data.size() >= 64 && std::memcmp(data.data() + 60, "SONG", 4) == 0) {
        return load_sfx_nins(std::move(data), std::move(path), 15);
    }
    if (data.size() >= 128 && std::memcmp(data.data() + 124, "SO31", 4) == 0) {
        return load_sfx_nins(std::move(data), std::move(path), 31);
    }
    throw std::runtime_error("not a SoundFX module");
}

}  // namespace mod
