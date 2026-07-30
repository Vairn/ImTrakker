#include "mod/module.hpp"
#include "mod/tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace mod {
namespace {

uint16_t le16(const uint8_t* p) {
    return uint16_t(p[0] | (p[1] << 8));
}
uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

std::string asciiz(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len] != 0) {
        ++len;
    }
    std::string s(reinterpret_cast<const char*>(p), len);
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    return s;
}

bool in_range(size_t off, size_t need, size_t size) {
    return off <= size && size - off >= need;
}

std::vector<float> decode_delta8(const uint8_t* p, size_t nbytes) {
    std::vector<float> out(nbytes ? nbytes : 2, 0.f);
    int old = 0;
    for (size_t i = 0; i < nbytes; ++i) {
        old = int8_t(uint8_t(old + p[i]));
        out[i] = float(old) / 128.f;
    }
    return out;
}

std::vector<float> decode_delta16(const uint8_t* p, size_t nbytes) {
    const size_t n = nbytes / 2;
    std::vector<float> out(n ? n : 2, 0.f);
    int old = 0;
    for (size_t i = 0; i < n; ++i) {
        const int16_t d = int16_t(le16(p + i * 2));
        old = int16_t(old + d);
        out[i] = float(old) / 32768.f;
    }
    return out;
}

int xm_note_to_period(int note) {
    // XM note 1 = C-0 … ; Amiga period approx via midi
    if (note <= 0 || note >= 97) {
        return 0;  // 97 = key off
    }
    return midi_to_period(note + 11);  // XM C-0 ≈ MIDI 12
}

int map_xm_effect(int fx, int param, Note& n) {
    // Keep common PT-compatible subset; volume column handled separately.
    n.effect = 0;
    n.param = 0;
    switch (fx) {
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x3:
    case 0x4:
    case 0x5:
    case 0x6:
    case 0x7:
    case 0x9:
    case 0xA:
    case 0xB:
    case 0xC:
    case 0xD:
    case 0xE:
    case 0xF:
        n.effect = fx;
        n.param = param;
        break;
    case 0x10:  // G — set global volume → ignore
        break;
    default:
        break;
    }
    return 0;
}

Module load_xm(std::vector<uint8_t> data, std::filesystem::path path) {
    if (data.size() < 60 || std::memcmp(data.data(), "Extended Module: ", 17) != 0) {
        throw std::runtime_error("not an XM module");
    }
    Module mod;
    mod.path = std::move(path);
    mod.magic = "XM  ";
    mod.title = asciiz(data.data() + 17, 20);

    const uint32_t hdr_size = le32(data.data() + 60);
    if (hdr_size < 20 || !in_range(60, hdr_size, data.size())) {
        throw std::runtime_error("bad XM header size");
    }
    const uint16_t songlen = le16(data.data() + 64);
    const uint16_t restart = le16(data.data() + 66);
    const uint16_t channels = le16(data.data() + 68);
    const uint16_t npat = le16(data.data() + 70);
    const uint16_t nins = le16(data.data() + 72);
    const uint16_t flags = le16(data.data() + 74);
    const uint16_t speed = le16(data.data() + 76);
    const uint16_t tempo = le16(data.data() + 78);
    (void)flags;

    mod.song_length = std::clamp(int(songlen), 1, 256);
    mod.restart = std::clamp(int(restart), 0, mod.song_length - 1);
    mod.channels = std::clamp(int(channels), 1, kMaxChannels);
    mod.initial_speed = std::max(1, int(speed));
    mod.initial_tempo = std::max(32, int(tempo));
    mod.orders.assign(256, 0);
    for (int i = 0; i < 256; ++i) {
        mod.orders[size_t(i)] = data[80 + i];
    }

    size_t pos = 60 + hdr_size;
    mod.patterns.resize(npat);
    for (int p = 0; p < int(npat); ++p) {
        if (!in_range(pos, 9, data.size())) {
            break;
        }
        const uint32_t ph_size = le32(data.data() + pos);
        const uint16_t rows = le16(data.data() + pos + 5);
        const uint16_t pdata_size = le16(data.data() + pos + 7);
        size_t data_off = pos + ph_size;
        if (!in_range(data_off, pdata_size, data.size())) {
            throw std::runtime_error("truncated XM pattern");
        }
        auto& pat = mod.patterns[size_t(p)];
        pat.assign(size_t(std::max(1, int(rows))), std::vector<Note>(size_t(mod.channels)));
        size_t cur = data_off;
        for (int row = 0; row < int(rows); ++row) {
            for (int ch = 0; ch < mod.channels; ++ch) {
                if (cur >= data_off + pdata_size) {
                    break;
                }
                Note& n = pat[size_t(row)][size_t(ch)];
                const uint8_t b0 = data[cur++];
                int note = 0, ins = 0, vol = 0, fx = 0, param = 0;
                if (b0 & 0x80) {
                    if (b0 & 0x01) {
                        note = data[cur++];
                    }
                    if (b0 & 0x02) {
                        ins = data[cur++];
                    }
                    if (b0 & 0x04) {
                        vol = data[cur++];
                    }
                    if (b0 & 0x08) {
                        fx = data[cur++];
                    }
                    if (b0 & 0x10) {
                        param = data[cur++];
                    }
                } else {
                    note = b0;
                    ins = data[cur++];
                    vol = data[cur++];
                    fx = data[cur++];
                    param = data[cur++];
                }
                n.period = xm_note_to_period(note);
                n.instrument = std::clamp(ins, 0, 31);
                map_xm_effect(fx, param, n);
                if (vol >= 0x10 && vol <= 0x50 && n.effect == 0) {
                    n.effect = 0xC;
                    n.param = vol - 0x10;
                }
            }
        }
        pos = data_off + pdata_size;
    }

    mod.samples.assign(31, Sample{});
    int smp_slot = 0;
    for (int ii = 0; ii < int(nins) && smp_slot < 31; ++ii) {
        if (!in_range(pos, 4, data.size())) {
            break;
        }
        const uint32_t ih_size = le32(data.data() + pos);
        if (!in_range(pos, ih_size, data.size()) || ih_size < 29) {
            break;
        }
        const std::string iname = asciiz(data.data() + pos + 4, 22);
        const uint16_t n_samples = le16(data.data() + pos + 27);
        size_t sh_off = pos + ih_size;
        // Skip instrument header extra / sample headers
        std::vector<uint32_t> smp_lens(n_samples);
        std::vector<uint32_t> smp_loops(n_samples);
        std::vector<uint32_t> smp_looplens(n_samples);
        std::vector<uint8_t> smp_vols(n_samples);
        std::vector<uint8_t> smp_flags(n_samples);
        std::vector<std::string> smp_names(n_samples);
        if (n_samples > 0) {
            if (!in_range(sh_off, size_t(n_samples) * 40, data.size())) {
                break;
            }
            for (int s = 0; s < int(n_samples); ++s) {
                const uint8_t* sh = data.data() + sh_off + size_t(s) * 40;
                smp_lens[size_t(s)] = le32(sh + 0);
                smp_loops[size_t(s)] = le32(sh + 4);
                smp_looplens[size_t(s)] = le32(sh + 8);
                smp_vols[size_t(s)] = sh[12];
                smp_flags[size_t(s)] = sh[14];
                smp_names[size_t(s)] = asciiz(sh + 18, 22);
            }
            sh_off += size_t(n_samples) * 40;
        }
        for (int s = 0; s < int(n_samples) && smp_slot < 31; ++s) {
            const uint32_t slen = smp_lens[size_t(s)];
            if (!in_range(sh_off, slen, data.size())) {
                sh_off += slen;
                continue;
            }
            Sample& smp = mod.samples[size_t(smp_slot)];
            smp.name = smp_names[size_t(s)].empty() ? iname : smp_names[size_t(s)];
            smp.volume = std::clamp(int(smp_vols[size_t(s)]), 0, 64);
            const bool is16 = (smp_flags[size_t(s)] & 0x10) != 0;
            if (is16) {
                smp.wave = decode_delta16(data.data() + sh_off, slen);
            } else {
                smp.wave = decode_delta8(data.data() + sh_off, slen);
            }
            smp.length_words = int(smp.wave.size() / 2);
            if (smp_flags[size_t(s)] & 0x03) {
                const int loop_s = int(smp_loops[size_t(s)] / (is16 ? 2 : 1));
                const int loop_l = int(smp_looplens[size_t(s)] / (is16 ? 2 : 1));
                smp.repstart_words = loop_s / 2;
                smp.replen_words = std::max(1, loop_l / 2);
            } else {
                smp.replen_words = 1;
            }
            // Remap pattern instruments that pointed at XM instrument ii+1 → this slot+1
            // (lossy: multi-sample instruments collapse to first sample)
            if (s == 0) {
                for (auto& pat : mod.patterns) {
                    for (auto& row : pat) {
                        for (auto& n : row) {
                            if (n.instrument == ii + 1) {
                                n.instrument = smp_slot + 1;
                            }
                        }
                    }
                }
            }
            ++smp_slot;
            sh_off += slen;
        }
        pos = sh_off;
    }

    if (mod.patterns.empty()) {
        mod.patterns.push_back(
            std::vector<std::vector<Note>>(64, std::vector<Note>(size_t(mod.channels))));
    }
    return mod;
}

Module load_s3m(std::vector<uint8_t> data, std::filesystem::path path) {
    if (data.size() < 0x60 || std::memcmp(data.data() + 0x2C, "SCRM", 4) != 0) {
        throw std::runtime_error("not an S3M module");
    }
    Module mod;
    mod.path = std::move(path);
    mod.magic = "S3M ";
    mod.title = asciiz(data.data(), 28);
    const uint16_t ordnum = le16(data.data() + 0x20);
    const uint16_t insnum = le16(data.data() + 0x22);
    const uint16_t patnum = le16(data.data() + 0x24);
    const uint16_t flags = le16(data.data() + 0x28);
    (void)flags;
    const uint8_t initial_speed = data[0x31];
    const uint8_t initial_tempo = data[0x32];
    mod.initial_speed = std::max(1, int(initial_speed));
    mod.initial_tempo = std::max(32, int(initial_tempo));
    mod.channels = 0;
    for (int i = 0; i < 32; ++i) {
        if (data[0x40 + i] != 0xFF) {
            mod.channels = i + 1;
        }
    }
    mod.channels = std::clamp(mod.channels, 1, kMaxChannels);

    size_t pos = 0x60;
    mod.orders.assign(ordnum, 0);
    for (int i = 0; i < int(ordnum); ++i) {
        mod.orders[size_t(i)] = data[pos++];
    }
    while (!mod.orders.empty() && mod.orders.back() == 0xFF) {
        mod.orders.pop_back();
    }
    mod.song_length = std::max(1, int(mod.orders.size()));
    mod.restart = 0;

    std::vector<uint16_t> ins_paraptrs(insnum);
    for (int i = 0; i < int(insnum); ++i) {
        ins_paraptrs[size_t(i)] = le16(data.data() + pos);
        pos += 2;
    }
    std::vector<uint16_t> pat_paraptrs(patnum);
    for (int i = 0; i < int(patnum); ++i) {
        pat_paraptrs[size_t(i)] = le16(data.data() + pos);
        pos += 2;
    }

    mod.samples.assign(31, Sample{});
    for (int i = 0; i < int(insnum) && i < 31; ++i) {
        const size_t off = size_t(ins_paraptrs[size_t(i)]) * 16;
        if (!in_range(off, 0x50, data.size())) {
            continue;
        }
        if (data[off] != 1) {
            continue;  // only PCM samples
        }
        Sample& s = mod.samples[size_t(i)];
        s.name = asciiz(data.data() + off + 0x30, 28);
        const uint32_t length = le32(data.data() + off + 0x10);
        const uint32_t loop_beg = le32(data.data() + off + 0x14);
        const uint32_t loop_end = le32(data.data() + off + 0x18);
        s.volume = std::clamp(int(data[off + 0x1C]), 0, 64);
        const uint8_t flags8 = data[off + 0x1F];
        const uint32_t memseg = (uint32_t(data[off + 0x0D]) << 16) | le16(data.data() + off + 0x0E);
        const size_t smp_off = size_t(memseg) * 16;
        if (!in_range(smp_off, length, data.size())) {
            continue;
        }
        s.wave.resize(length ? length : 2);
        const bool unsigned_pcm = (data[0x2E] == 1);  // ffi / version quirks — use signed default
        (void)unsigned_pcm;
        for (size_t b = 0; b < length; ++b) {
            s.wave[b] = float(int8_t(data[smp_off + b] ^ 0x80)) / 128.f;  // S3M often unsigned
        }
        s.length_words = int(s.wave.size() / 2);
        if (flags8 & 1) {
            s.repstart_words = int(loop_beg / 2);
            s.replen_words = std::max(1, int((loop_end - loop_beg) / 2));
        } else {
            s.replen_words = 1;
        }
    }

    mod.patterns.resize(patnum);
    for (int p = 0; p < int(patnum); ++p) {
        const size_t off = size_t(pat_paraptrs[size_t(p)]) * 16;
        auto& pat = mod.patterns[size_t(p)];
        pat.assign(64, std::vector<Note>(size_t(mod.channels)));
        if (!in_range(off, 2, data.size())) {
            continue;
        }
        const uint16_t packed_len = le16(data.data() + off);
        size_t cur = off + 2;
        const size_t end = off + 2 + packed_len;
        int row = 0;
        while (cur < end && row < 64) {
            const uint8_t what = data[cur++];
            if (what == 0) {
                ++row;
                continue;
            }
            const int ch = what & 0x1F;
            int note = 0, ins = 0, vol = 0, fx = 0, param = 0;
            if (what & 0x20) {
                note = data[cur++];
                ins = data[cur++];
            }
            if (what & 0x40) {
                vol = data[cur++];
            }
            if (what & 0x80) {
                fx = data[cur++];
                param = data[cur++];
            }
            if (ch >= mod.channels) {
                continue;
            }
            Note& n = pat[size_t(row)][size_t(ch)];
            if (note < 254) {
                // hi nibble octave, lo note
                const int ni = (note >> 4) * 12 + (note & 0x0F);
                n.period = midi_to_period(ni + 24);
            }
            n.instrument = std::clamp(ins, 0, 31);
            if (vol <= 64) {
                n.effect = 0xC;
                n.param = vol;
            }
            // Map a few S3M effects to PT
            if (fx == 1) {  // A speed
                n.effect = 0xF;
                n.param = param;
            } else if (fx == 20) {  // T tempo
                n.effect = 0xF;
                n.param = param;
            } else if (fx == 4) {  // D volslide rough
                n.effect = 0xA;
                n.param = param;
            } else if (fx == 5 || fx == 6) {
                n.effect = (fx == 5) ? 0x2 : 0x1;
                n.param = param;
            }
        }
    }
    return mod;
}

Module load_it(std::vector<uint8_t> data, std::filesystem::path path) {
    if (data.size() < 0xC0 || std::memcmp(data.data(), "IMPM", 4) != 0) {
        throw std::runtime_error("not an IT module");
    }
    Module mod;
    mod.path = std::move(path);
    mod.magic = "IMPM";
    mod.title = asciiz(data.data() + 4, 26);
    const uint16_t ordnum = le16(data.data() + 0x20);
    const uint16_t insnum = le16(data.data() + 0x22);
    const uint16_t smpnum = le16(data.data() + 0x24);
    const uint16_t patnum = le16(data.data() + 0x26);
    mod.initial_speed = std::max(1, int(data[0x33]));
    mod.initial_tempo = std::max(32, int(data[0x32]));
    mod.channels = 8;  // IT often uses many; clamp for player
    mod.orders.assign(ordnum, 0);
    size_t pos = 0xC0;
    for (int i = 0; i < int(ordnum); ++i) {
        mod.orders[size_t(i)] = data[pos++];
    }
    while (!mod.orders.empty() && (mod.orders.back() >= 254)) {
        mod.orders.pop_back();
    }
    mod.song_length = std::max(1, int(mod.orders.size()));
    mod.restart = 0;

    pos = 0xC0 + ordnum;
    std::vector<uint32_t> ins_off(insnum), smp_off(smpnum), pat_off(patnum);
    for (int i = 0; i < int(insnum); ++i) {
        ins_off[size_t(i)] = le32(data.data() + pos);
        pos += 4;
    }
    for (int i = 0; i < int(smpnum); ++i) {
        smp_off[size_t(i)] = le32(data.data() + pos);
        pos += 4;
    }
    for (int i = 0; i < int(patnum); ++i) {
        pat_off[size_t(i)] = le32(data.data() + pos);
        pos += 4;
    }
    (void)ins_off;

    mod.samples.assign(31, Sample{});
    for (int i = 0; i < int(smpnum) && i < 31; ++i) {
        const size_t off = smp_off[size_t(i)];
        if (!in_range(off, 0x50, data.size()) || std::memcmp(data.data() + off, "IMPS", 4) != 0) {
            continue;
        }
        Sample& s = mod.samples[size_t(i)];
        s.name = asciiz(data.data() + off + 0x14, 26);
        const uint32_t length = le32(data.data() + off + 0x30);
        const uint32_t loop_beg = le32(data.data() + off + 0x34);
        const uint32_t loop_end = le32(data.data() + off + 0x38);
        s.volume = std::clamp(int(data[off + 0x3C]), 0, 64);
        const uint8_t flags8 = data[off + 0x12];
        const uint32_t sample_ptr = le32(data.data() + off + 0x48);
        if (!sample_ptr || !in_range(sample_ptr, length, data.size())) {
            continue;
        }
        const bool is16 = (flags8 & 2) != 0;
        const bool compressed = (flags8 & 8) != 0;
        if (compressed) {
            // Skip compressed — leave silent stub
            s.wave = {0.f, 0.f};
            s.length_words = 1;
            continue;
        }
        if (is16) {
            const size_t n = length;
            s.wave.resize(n ? n : 2);
            for (size_t b = 0; b < n; ++b) {
                s.wave[b] = float(int16_t(le16(data.data() + sample_ptr + b * 2))) / 32768.f;
            }
        } else {
            s.wave.resize(length ? length : 2);
            for (size_t b = 0; b < length; ++b) {
                s.wave[b] = float(int8_t(data[sample_ptr + b])) / 128.f;
            }
        }
        s.length_words = int(s.wave.size() / 2);
        if (flags8 & 0x10) {
            s.repstart_words = int(loop_beg / 2);
            s.replen_words = std::max(1, int((loop_end - loop_beg) / 2));
        } else {
            s.replen_words = 1;
        }
    }

    mod.patterns.resize(patnum);
    int max_ch = 4;
    for (int p = 0; p < int(patnum); ++p) {
        const size_t off = pat_off[size_t(p)];
        auto& pat = mod.patterns[size_t(p)];
        if (!off || !in_range(off, 8, data.size())) {
            pat.assign(64, std::vector<Note>(8));
            continue;
        }
        const uint16_t length = le16(data.data() + off);
        const uint16_t rows = le16(data.data() + off + 2);
        size_t cur = off + 8;
        const size_t end = off + 8 + length;
        pat.assign(size_t(std::max(1, int(rows))), std::vector<Note>(64));
        std::array<uint8_t, 64> mask{};
        std::array<Note, 64> last{};
        int row = 0;
        while (cur < end && row < int(rows)) {
            const uint8_t chn = data[cur++];
            if (chn == 0) {
                ++row;
                continue;
            }
            const int ch = (chn - 1) & 63;
            max_ch = std::max(max_ch, ch + 1);
            uint8_t m;
            if (chn & 0x80) {
                m = data[cur++];
                mask[size_t(ch)] = m;
            } else {
                m = mask[size_t(ch)];
            }
            Note n = last[size_t(ch)];
            if (m & 1) {
                const int note = data[cur++];
                if (note < 120) {
                    n.period = midi_to_period(note + 12);
                } else {
                    n.period = 0;
                }
            }
            if (m & 2) {
                n.instrument = std::clamp(int(data[cur++]), 0, 31);
            }
            if (m & 4) {
                const int vol = data[cur++];
                if (vol <= 64) {
                    n.effect = 0xC;
                    n.param = vol;
                }
            }
            if (m & 8) {
                const int fx = data[cur++];
                const int param = data[cur++];
                // Map a couple IT effects loosely
                if (fx == 1) {
                    n.effect = 0xF;
                    n.param = param;
                } else if (fx == 20) {
                    n.effect = 0xF;
                    n.param = param;
                }
            }
            if (m & 0x10) {
                /* note same */
            }
            last[size_t(ch)] = n;
            if (ch < int(pat[size_t(row)].size())) {
                pat[size_t(row)][size_t(ch)] = n;
            }
        }
        for (auto& r : pat) {
            r.resize(size_t(std::clamp(max_ch, 4, kMaxChannels)));
        }
    }
    mod.channels = std::clamp(max_ch, 2, kMaxChannels);
    for (auto& pat : mod.patterns) {
        for (auto& row : pat) {
            row.resize(size_t(mod.channels));
        }
    }
    return mod;
}

}  // namespace

Module load_extended(std::vector<uint8_t> data, std::filesystem::path path) {
    if (data.size() >= 17 && std::memcmp(data.data(), "Extended Module: ", 17) == 0) {
        return load_xm(std::move(data), std::move(path));
    }
    if (data.size() >= 0x30 && std::memcmp(data.data() + 0x2C, "SCRM", 4) == 0) {
        return load_s3m(std::move(data), std::move(path));
    }
    if (data.size() >= 4 && std::memcmp(data.data(), "IMPM", 4) == 0) {
        return load_it(std::move(data), std::move(path));
    }
    throw std::runtime_error("not XM/S3M/IT");
}

}  // namespace mod
