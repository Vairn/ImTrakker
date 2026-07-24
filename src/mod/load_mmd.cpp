#include "mod/module.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace mod {
namespace {

uint16_t be16(const uint8_t* p) {
    return uint16_t((p[0] << 8) | p[1]);
}
uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

bool in_range(size_t off, size_t need, size_t size) {
    return off <= size && size - off >= need;
}

std::vector<float> pcm_from_bytes(const uint8_t* p, size_t nbytes) {
    std::vector<float> out(nbytes ? nbytes : 2, 0.f);
    for (size_t i = 0; i < nbytes; ++i) {
        out[i] = float(int8_t(p[i])) / 128.f;
    }
    if (nbytes >= 2) {
        out[0] = 0.f;
        out[1] = 0.f;
    }
    return out;
}

}  // namespace

Module load_mmd(std::vector<uint8_t> data, std::filesystem::path path) {
    if (data.size() < 52) {
        throw std::runtime_error("truncated MMD header");
    }

    const bool mmd0 = std::memcmp(data.data(), "MMD0", 4) == 0;
    const bool mmd1 = std::memcmp(data.data(), "MMD1", 4) == 0;
    if (!mmd0 && !mmd1 && std::memcmp(data.data(), "MMD2", 4) != 0 &&
        std::memcmp(data.data(), "MMD3", 4) != 0) {
        throw std::runtime_error("not an MMD module");
    }

    // MMD2/3 song structs differ; support MMD0/1 fully, best-effort MMD2/3 as MMD1-like.
    const bool use_mmd1_blocks = !mmd0;

    const uint32_t song_off = be32(data.data() + 8);
    const uint32_t blockarr_off = be32(data.data() + 16);
    const uint32_t smplarr_off = be32(data.data() + 24);

    if (!in_range(song_off, 788, data.size())) {
        throw std::runtime_error("bad MMD song pointer");
    }

    const uint8_t* song = data.data() + song_off;
    const uint16_t numblocks = be16(song + 504);
    const uint16_t songlen = be16(song + 506);
    const uint16_t deftempo = be16(song + 764);
    const int8_t playtransp = int8_t(song[766]);
    const uint8_t flags = song[767];
    const uint8_t flags2 = song[768];
    const uint8_t tempo2 = song[769];
    const uint8_t numsamples = song[787];

    Module mod;
    mod.path = std::move(path);
    mod.magic.assign(reinterpret_cast<const char*>(data.data()), 4);
    mod.title = mod.path.stem().string();
    mod.song_length = songlen;
    mod.restart = 0;
    mod.initial_speed = std::max(1, int(tempo2 ? tempo2 : 6));
    if (flags2 & 0x20) {
        mod.initial_tempo = std::max(32, int(deftempo));
    } else {
        mod.initial_tempo = 125;
    }

    mod.orders.assign(song + 508, song + 508 + std::min<int>(songlen, 256));
    if (mod.orders.size() < size_t(songlen)) {
        mod.orders.resize(size_t(songlen), 0);
    }

    // Samples
    mod.samples.resize(std::max(31, int(numsamples)));
    for (int i = 0; i < 63 && i < int(mod.samples.size()); ++i) {
        const uint8_t* s = song + size_t(i) * 8;
        Sample& smp = mod.samples[size_t(i)];
        smp.repstart_words = be16(s + 0);
        smp.replen_words = be16(s + 2);
        smp.volume = s[6];
        smp.name = "smp" + std::to_string(i + 1);
        // transpose applied when converting notes
        (void)s[7];
    }

    if (smplarr_off && in_range(smplarr_off, size_t(numsamples) * 4, data.size())) {
        for (int i = 0; i < int(numsamples); ++i) {
            const uint32_t off = be32(data.data() + smplarr_off + size_t(i) * 4);
            if (!off || !in_range(off, 6, data.size())) {
                continue;
            }
            const uint32_t length = be32(data.data() + off);
            const int16_t type = int16_t(be16(data.data() + off + 4));
            Sample& smp = mod.samples[size_t(i)];
            if (type >= 0) {
                // sample / multi-octave IFF: use first octave length bytes
                size_t nbytes = length;
                if (!in_range(off + 6, nbytes, data.size())) {
                    nbytes = data.size() - (off + 6);
                }
                // Multi-octave: length is total; use first portion as one octave
                if (type > 0) {
                    // approximate: length is all octaves concatenated; take 1/N
                    const int octs = (type == 1) ? 5 : (type == 2) ? 3 : (type + 1);
                    nbytes = nbytes / size_t(std::max(1, octs));
                }
                smp.length_words = int(nbytes / 2);
                smp.wave = pcm_from_bytes(data.data() + off + 6, nbytes);
                if (smp.replen_words <= 1) {
                    smp.replen_words = 0;
                }
            } else {
                // synth/hybrid — generate a short tone so notes still speak
                smp.wave.assign(256, 0.f);
                for (size_t n = 0; n < smp.wave.size(); ++n) {
                    smp.wave[n] = (n & 32) ? 0.25f : -0.25f;
                }
                smp.length_words = int(smp.wave.size() / 2);
                smp.replen_words = smp.length_words;
            }
        }
    }

    // Blocks
    if (!blockarr_off || !in_range(blockarr_off, size_t(numblocks) * 4, data.size())) {
        throw std::runtime_error("bad MMD blockarr pointer");
    }

    int max_ch = 4;
    mod.patterns.resize(numblocks);
    for (int b = 0; b < int(numblocks); ++b) {
        const uint32_t boff = be32(data.data() + blockarr_off + size_t(b) * 4);
        if (!boff || !in_range(boff, 2, data.size())) {
            mod.patterns[size_t(b)].assign(1, std::vector<Note>(4));
            continue;
        }

        int numtracks = 0;
        int lines = 0;
        size_t note_off = 0;
        size_t note_stride = 0;

        if (!use_mmd1_blocks) {
            numtracks = data[boff];
            lines = int(data[boff + 1]) + 1;
            note_off = boff + 2;
            note_stride = 3;
        } else {
            if (!in_range(boff, 8, data.size())) {
                mod.patterns[size_t(b)].assign(1, std::vector<Note>(4));
                continue;
            }
            numtracks = be16(data.data() + boff);
            lines = int(be16(data.data() + boff + 2)) + 1;
            note_off = boff + 8;
            note_stride = 4;
        }

        numtracks = std::clamp(numtracks, 1, kMaxChannels);
        lines = std::clamp(lines, 1, 3200);
        max_ch = std::max(max_ch, numtracks);

        auto& pat = mod.patterns[size_t(b)];
        pat.resize(size_t(lines));
        for (int row = 0; row < lines; ++row) {
            pat[size_t(row)].assign(size_t(numtracks), Note{});
            for (int tr = 0; tr < numtracks; ++tr) {
                const size_t o = note_off + (size_t(row) * size_t(numtracks) + size_t(tr)) * note_stride;
                if (!in_range(o, note_stride, data.size())) {
                    continue;
                }
                Note& n = pat[size_t(row)][size_t(tr)];
                if (!use_mmd1_blocks) {
                    const uint8_t a = data[o];
                    const uint8_t b1 = data[o + 1];
                    const uint8_t c = data[o + 2];
                    const int note = a & 0x3F;
                    int ins = ((a & 0x80) ? 0x10 : 0) | ((a & 0x40) ? 0x20 : 0) | ((b1 >> 4) & 0x0F);
                    n.instrument = ins;
                    n.effect = b1 & 0x0F;
                    n.param = c;
                    if (note) {
                        n.period = med_note_to_period(note + playtransp);
                    }
                } else {
                    const uint8_t a = data[o];
                    const uint8_t b1 = data[o + 1];
                    const uint8_t c = data[o + 2];
                    const uint8_t d = data[o + 3];
                    const int note = a & 0x7F;
                    const int ins = b1 & 0x3F;
                    n.instrument = ins;
                    n.effect = c & 0x0F;  // map low nibble-ish MED cmds loosely
                    n.param = d;
                    // MMD1 command is full byte — keep common PT-like subset in low nibble
                    if (c <= 0x0F) {
                        n.effect = c;
                    } else if (c == 0x0C) {
                        n.effect = 0xC;
                    } else if (c == 0x0F) {
                        n.effect = 0xF;
                    } else {
                        n.effect = 0;
                        n.param = 0;
                    }
                    if (note) {
                        n.period = med_note_to_period(note + playtransp);
                    }
                }
            }
        }
    }

    if (flags & 0x40) {
        max_ch = std::max(max_ch, 8);
    }
    mod.channels = max_ch;

    // Normalize pattern channel counts
    for (auto& pat : mod.patterns) {
        for (auto& row : pat) {
            row.resize(size_t(mod.channels));
        }
    }

    return mod;
}

}  // namespace mod
