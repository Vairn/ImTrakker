#include "smus/smus.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace smus {
namespace {

constexpr uint8_t kSidRest = 0x80;
constexpr uint8_t kSidInstrument = 0x81;
constexpr uint8_t kSidDynamic = 0x84;
constexpr uint8_t kSidTempo = 0x88;

uint16_t be16(const uint8_t* p) {
    return uint16_t((p[0] << 8) | p[1]);
}
uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

std::string read_cstring(const uint8_t* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len] != 0) {
        ++len;
    }
    std::string s(reinterpret_cast<const char*>(p), len);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) {
        s.pop_back();
    }
    return s;
}

std::vector<uint8_t> read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open: " + path.string());
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

template <typename Fn>
void for_iff_chunks(const uint8_t* data, size_t size, size_t start, Fn&& fn) {
    size_t pos = start;
    while (pos + 8 <= size) {
        char id[4];
        std::memcpy(id, data + pos, 4);
        const uint32_t sz = be32(data + pos + 4);
        pos += 8;
        if (pos + sz > size) {
            break;
        }
        fn(id, data + pos, sz);
        pos += sz + (sz & 1);
    }
}

int16_t to_i16(int x) {
    x &= 0xFFFF;
    return int16_t(x >= 0x8000 ? x - 0x10000 : x);
}
int32_t to_i32(int64_t x) {
    x &= 0xFFFFFFFF;
    return int32_t(x >= 0x80000000LL ? x - 0x100000000LL : x);
}

const uint16_t kFilterCoeffs[64] = {
    0x8000, 0x7683, 0x6DBA, 0x6597, 0x5E10, 0x5717, 0x50A2, 0x4AA8, 0x451F, 0x4000, 0x3B41,
    0x36DD, 0x32CB, 0x2F08, 0x2B8B, 0x2851, 0x2554, 0x228F, 0x2000, 0x1DA0, 0x1B6E, 0x1965,
    0x1784, 0x15C5, 0x1428, 0x12AA, 0x1147, 0x1000, 0x0ED0, 0x0DB7, 0x0CB2, 0x0BC2, 0x0AE2,
    0x0A14, 0x0955, 0x08A3, 0x0800, 0x0768, 0x06DB, 0x0659, 0x05E1, 0x0571, 0x050A, 0x04AA,
    0x0451, 0x0400, 0x03B4, 0x036D, 0x032C, 0x02F0, 0x02B8, 0x0285, 0x0255, 0x0228, 0x0200,
    0x01DA, 0x01B6, 0x0196, 0x0178, 0x015C, 0x0142, 0x012A, 0x0114, 0x0100,
};

std::vector<float> sonix_one_filter(const uint8_t* wave128) {
    int wave[128];
    for (int i = 0; i < 128; ++i) {
        wave[i] = int8_t(wave128[i]);
    }
    std::vector<float> out(64 * 128);
    int d3 = 0;
    int d4 = to_i16(wave[127] << 7);
    int oi = 0;
    for (int step = 0; step < 64; ++step) {
        int d1 = kFilterCoeffs[step];
        int d2 = (0x8000 - d1) & 0xFFFF;
        d2 = int(((uint32_t(d2) * 0xE666u) & 0xFFFFFFFFu) >> 16);
        d1 >>= 1;
        for (int s = 0; s < 128; ++s) {
            int d6 = to_i16(to_i16(wave[s] << 7) - d4);
            int32_t prod = to_i32(int32_t(to_i16(d1)) * d6);
            prod = to_i32(int64_t(prod) << 2);
            d3 = to_i16(d3 + (prod >> 16));
            d4 = to_i16(d4 + d3);
            const uint16_t d4u = uint16_t(d4);
            const uint16_t ror = uint16_t((d4u >> 7) | ((d4u & 0x7F) << 9));
            out[size_t(oi++)] = float(int8_t(ror & 0xFF)) / 128.f;
            prod = to_i32(int32_t(to_i16(d3)) * int32_t(to_i16(d2)));
            d3 = to_i16(to_i32(int64_t(prod) << 1) >> 16);
        }
    }
    return out;
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

const float kNotePeriod[12] = {
    float(0x8000), float(0x78D1), float(0x7209), float(0x6BA2), float(0x6598), float(0x5FE4),
    float(0x5A82), float(0x556E), float(0x50A3), float(0x4C1C), float(0x47D6), float(0x43CE),
};

int sample_octave_for_midi(int midi, int lo, int hi) {
    int octv = 10 - (midi / 12);
    return std::clamp(octv, lo, hi);
}

std::vector<float> i8_to_f32(const uint8_t* p, size_t n) {
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = float(int8_t(p[i])) / 128.f;
    }
    return out;
}

Instrument load_8svx(const std::vector<uint8_t>& data, const std::string& name) {
    if (data.size() < 12 || std::memcmp(data.data(), "FORM", 4) != 0 ||
        std::memcmp(data.data() + 8, "8SVX", 4) != 0) {
        throw std::runtime_error("Not 8SVX");
    }
    uint32_t oneshot = 0, repeat = 0;
    int rate = 8363;
    const uint8_t* body = nullptr;
    size_t body_sz = 0;
    for_iff_chunks(data.data(), data.size(), 12, [&](const char* id, const uint8_t* chunk, uint32_t sz) {
        if (std::memcmp(id, "VHDR", 4) == 0 && sz >= 14) {
            oneshot = be32(chunk);
            repeat = be32(chunk + 4);
            rate = be16(chunk + 12) ? be16(chunk + 12) : 8363;
        } else if (std::memcmp(id, "BODY", 4) == 0) {
            body = chunk;
            body_sz = sz;
        }
    });
    Instrument ins;
    ins.name = name;
    ins.kind = InstrKind::S8svx;
    ins.wave = body ? i8_to_f32(body, body_sz) : std::vector<float>{0.f, 0.f};
    ins.loop_start = int(oneshot);
    ins.loop_end = repeat ? int(oneshot + repeat) : 0;
    ins.base_midi = 60;
    ins.base_rate = float(rate);
    return ins;
}

Instrument load_ss(const std::filesystem::path& path, const std::string& name, float volume,
                   const std::array<int, 4>& levels, const std::array<int, 4>& rates, int vib_depth,
                   int vib_rate, int vib_delay) {
    auto data = read_all(path);
    if (data.size() < 64) {
        throw std::runtime_error("Truncated .ss");
    }
    const int oneshot = be16(data.data());
    const int repeat = be16(data.data() + 2);
    int lo = data[4], hi = data[5];
    if (hi < lo) {
        hi = lo;
    }
    auto payload = i8_to_f32(data.data() + 0x3E, data.size() - 0x3E);
    const int mid = sample_octave_for_midi(60, lo, hi);
    const int off = oneshot * ((1 << mid) - (1 << lo));
    const int ln = oneshot << mid;
    std::vector<float> wave;
    if (off + ln <= int(payload.size()) && ln > 0) {
        wave.assign(payload.begin() + off, payload.begin() + off + ln);
    } else {
        wave = payload;
    }

    Instrument ins;
    ins.name = name;
    ins.kind = InstrKind::Sample;
    ins.wave = std::move(wave);
    ins.volume = volume;
    ins.env_levels = levels;
    ins.env_rates = rates;
    ins.ss_oneshot = oneshot;
    ins.ss_repeat = repeat;
    ins.ss_lo = lo;
    ins.ss_hi = hi;
    ins.ss_data = std::move(payload);
    ins.vib_depth = vib_depth;
    ins.vib_rate = vib_rate;
    ins.vib_delay = vib_delay;
    ins.base_rate = 8363.f;
    return ins;
}

Instrument load_synth_instr(const std::vector<uint8_t>& data, const std::string& name) {
    if (data.size() < 68 + 128) {
        throw std::runtime_error("Truncated Synthesis instrument");
    }
    auto banks = sonix_one_filter(data.data() + 68);
    const uint8_t* body = data.data() + 32;
    const size_t blen = data.size() - 32;

    auto u16 = [&](int off) -> int {
        if (size_t(off + 2) > blen) {
            return 0;
        }
        return be16(body + off);
    };

    Instrument ins;
    ins.name = name;
    ins.kind = InstrKind::Synth;
    ins.filter_banks = std::move(banks);
    ins.mod_table.resize(256);
    if (blen >= 0xA4 + 256) {
        for (int i = 0; i < 256; ++i) {
            ins.mod_table[size_t(i)] = float(int8_t(body[0xA4 + i])) / 128.f;
        }
    }
    const int vol_raw = u16(0x1AC) & 0xFF;
    ins.volume = 0.35f + 0.65f * (float(std::max(vol_raw, 1)) / 255.f);
    ins.vol_env = u16(0x1AE) != 0;
    ins.vol_mod = u16(0x1B0) & 0xFF;
    ins.pitch_mod = u16(0x1B4) & 0xFF;
    for (int i = 0; i < 4; ++i) {
        ins.env_levels[size_t(i)] = u16(0x1C6 + i * 2) & 0xFF;
        ins.env_rates[size_t(i)] = u16(0x1CE + i * 2);
    }
    ins.f_base = u16(0x1B6) & 0xFF;
    ins.f_env = u16(0x1B8) & 0xFF;
    ins.f_mod = u16(0x1BA) & 0xFF;
    ins.lfo_inc = u16(0x1BC) & 0xFF;
    ins.lfo_rate = u16(0x1C0) & 0xFF;
    const int lfo_word = u16(0x1BE);
    const int lfo_signed = lfo_word >= 0x8000 ? lfo_word - 0x10000 : lfo_word;
    ins.lfo_enable = lfo_word != 0;
    ins.lfo_oneshot = lfo_signed >= 0;

    const int bank0 =
        std::clamp(((255 - ins.f_base) - ((255 * ins.f_env) >> 8)) >> 2, 0, 63);
    ins.wave.assign(ins.filter_banks.begin() + bank0 * 128,
                    ins.filter_banks.begin() + bank0 * 128 + 128);
    ins.loop_start = 0;
    ins.loop_end = 128;
    ins.base_rate = 16574.27f;
    return ins;
}

Instrument load_sampled_instr(const std::filesystem::path& instr_path, const std::vector<uint8_t>& data) {
    if (data.size() < 68 + 24) {
        throw std::runtime_error("Truncated SampledSound");
    }
    const std::string ss_name = read_cstring(data.data() + 68, 24);
    if (ss_name.empty()) {
        throw std::runtime_error("No .ss name in instrument");
    }
    const uint8_t* body = data.size() >= 32 ? data.data() + 32 : data.data();
    const size_t blen = data.size() >= 32 ? data.size() - 32 : data.size();
    auto u16 = [&](int off) -> int {
        if (size_t(off + 2) > blen) {
            return 0;
        }
        return be16(body + off);
    };
    const int vol_word = blen > 0x4A ? u16(0x48) : 0xC0;
    const float volume = float(std::max(vol_word, 1)) / 255.f;
    std::array<int, 4> levels{}, rates{};
    for (int i = 0; i < 4; ++i) {
        levels[size_t(i)] = u16(0x4A + i * 2) & 0xFF;
        rates[size_t(i)] = u16(0x52 + i * 2);
    }
    const int vib_depth = u16(0x5A) & 0xFF;
    const int vib_rate = u16(0x5C) & 0xFF;
    const int vib_delay = u16(0x5E) & 0xFF;

    const auto folder = instr_path.parent_path();
    std::filesystem::path ss_path;
    for (const auto& ent : std::filesystem::directory_iterator(folder)) {
        if (!ent.is_regular_file()) {
            continue;
        }
        auto ext = ent.path().extension().string();
        for (char& c : ext) {
            c = char(std::tolower(unsigned(c)));
        }
        if (ext == ".ss") {
            auto stem = ent.path().stem().string();
            std::string a = stem, b = ss_name;
            std::transform(a.begin(), a.end(), a.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            std::transform(b.begin(), b.end(), b.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (a == b) {
                ss_path = ent.path();
                break;
            }
        }
    }
    if (ss_path.empty()) {
        throw std::runtime_error("Missing sample '" + ss_name + ".ss'");
    }
    return load_ss(ss_path, instr_path.stem().string(), volume, levels, rates, vib_depth, vib_rate,
                   vib_delay);
}

int duration_rows(uint8_t data_byte) {
    const int n_tuplet = (data_byte >> 4) & 3;
    const bool dotted = (data_byte & 0x08) != 0;
    const int division = data_byte & 7;
    // 32nd note = 1 row (same mapping as the earlier SMUS→pattern bake)
    int rows = 1 << std::max(0, 5 - division);
    if (dotted) {
        rows = rows * 3 / 2;
    }
    if (n_tuplet) {
        const int order = 2 * n_tuplet + 1;
        rows = std::max(1, rows * 2 / order);
    }
    return std::max(1, rows);
}

void format_cell(PatternCell& cell) {
    static const char* kNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-",
                                     "F#", "G-", "G#", "A-", "A#", "B-"};
    if (cell.rest) {
        std::snprintf(cell.text, sizeof(cell.text), "--- .. ...");
        return;
    }
    if (cell.midi <= 0) {
        std::snprintf(cell.text, sizeof(cell.text), "--- .. ...");
        return;
    }
    const int oct = cell.midi / 12 - 1;
    char note[8];
    std::snprintf(note, sizeof(note), "%s%d", kNames[cell.midi % 12], oct);
    if (cell.instrument >= 0) {
        if (cell.volume >= 0) {
            std::snprintf(cell.text, sizeof(cell.text), "%s %02X C%02X", note, cell.instrument,
                          cell.volume);
        } else {
            std::snprintf(cell.text, sizeof(cell.text), "%s %02X ...", note, cell.instrument);
        }
    } else if (cell.volume >= 0) {
        std::snprintf(cell.text, sizeof(cell.text), "%s .. C%02X", note, cell.volume);
    } else {
        std::snprintf(cell.text, sizeof(cell.text), "%s .. ...", note);
    }
}

}  // namespace

DisplayPattern bake_display_pattern(const Score& score) {
    DisplayPattern pat;
    pat.channels = std::min(4, int(score.tracks.size()));
    if (pat.channels <= 0) {
        return pat;
    }

    struct Ev {
        int row = 0;
        int midi = 0;
        int instrument = 0;
        int vol = -1;
        bool rest = false;
    };
    std::vector<std::vector<Ev>> placed(size_t(pat.channels));
    int max_row = 1;

    for (int ti = 0; ti < pat.channels; ++ti) {
        const auto& trak = score.tracks[size_t(ti)];
        int cursor = 0;
        int cur_ins = 0;
        int cur_vol = -1;
        if (!score.instruments.empty()) {
            cur_ins = score.instruments.begin()->first;
            auto it = score.instruments.find(ti);
            if (it != score.instruments.end()) {
                cur_ins = it->first;
            }
        }

        for (size_t i = 0; i < trak.size(); ++i) {
            const SEvent& ev = trak[i];
            if (ev.sid < 0x80) {
                const bool chord = (ev.data & 0x80) != 0;
                const int dur = duration_rows(ev.data);
                if (!chord) {
                    Ev e;
                    e.row = cursor;
                    e.midi = ev.sid;
                    e.instrument = cur_ins;
                    e.vol = cur_vol;
                    placed[size_t(ti)].push_back(e);
                }
                cursor += dur;
            } else if (ev.sid == 0x80) {
                cursor += duration_rows(ev.data);
            } else if (ev.sid == 0x81) {
                cur_ins = ev.data;
            } else if (ev.sid == 0x84) {
                cur_vol = std::clamp(int(ev.data) * 64 / 127, 1, 64);
            }
        }
        max_row = std::max(max_row, cursor);
    }

    max_row = std::clamp(max_row, 1, 8192);
    pat.rows = max_row;
    pat.cells.resize(size_t(max_row));
    for (int r = 0; r < max_row; ++r) {
        pat.cells[size_t(r)].assign(size_t(pat.channels), PatternCell{});
        for (auto& cell : pat.cells[size_t(r)]) {
            format_cell(cell);
        }
    }
    for (int ti = 0; ti < pat.channels; ++ti) {
        for (const Ev& e : placed[size_t(ti)]) {
            if (e.row < 0 || e.row >= max_row) {
                continue;
            }
            PatternCell& cell = pat.cells[size_t(e.row)][size_t(ti)];
            cell.midi = e.midi;
            cell.instrument = e.instrument;
            cell.volume = e.vol;
            cell.rest = e.rest;
            format_cell(cell);
        }
    }
    return pat;
}

bool is_smus_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    char hdr[12]{};
    in.read(hdr, 12);
    return in && std::memcmp(hdr, "FORM", 4) == 0 && std::memcmp(hdr + 8, "SMUS", 4) == 0;
}

Score parse_file(const std::filesystem::path& path) {
    auto data = read_all(path);
    if (data.size() < 12 || std::memcmp(data.data(), "FORM", 4) != 0 ||
        std::memcmp(data.data() + 8, "SMUS", 4) != 0) {
        throw std::runtime_error("Not a FORM SMUS file");
    }
    Score score;
    score.path = path;
    score.name = path.stem().string();
    score.tempo = 128 * 120;
    score.volume = 127;

    for_iff_chunks(data.data(), data.size(), 12, [&](const char* id, const uint8_t* body, uint32_t sz) {
        if (std::memcmp(id, "SHDR", 4) == 0 && sz >= 4) {
            score.tempo = be16(body);
            score.volume = body[2];
        } else if (std::memcmp(id, "NAME", 4) == 0) {
            auto n = read_cstring(body, sz);
            if (!n.empty()) {
                score.name = n;
            }
        } else if (std::memcmp(id, "INS1", 4) == 0 && sz >= 4) {
            score.instruments[body[0]] = read_cstring(body + 4, sz - 4);
        } else if (std::memcmp(id, "TRAK", 4) == 0) {
            std::vector<SEvent> evs;
            for (uint32_t i = 0; i + 1 < sz; i += 2) {
                evs.push_back({body[i], body[i + 1]});
            }
            score.tracks.push_back(std::move(evs));
        }
    });
    if (score.tracks.empty()) {
        throw std::runtime_error("SMUS has no TRAK chunks");
    }
    return score;
}

Instrument default_instrument(const std::string& name) {
    Instrument ins;
    ins.name = name;
    ins.kind = InstrKind::Synth;
    ins.wave.resize(128);
    for (int i = 0; i < 128; ++i) {
        const float t = float(i) / 128.f * 6.2831853f;
        ins.wave[size_t(i)] = 0.4f * std::sin(t) + 0.2f * std::sin(2.f * t);
    }
    ins.loop_end = 128;
    ins.base_rate = 16574.27f;
    ins.filter_banks.assign(64 * 128, 0.f);
    for (int b = 0; b < 64; ++b) {
        std::copy(ins.wave.begin(), ins.wave.end(), ins.filter_banks.begin() + b * 128);
    }
    return ins;
}

Instrument load_instrument(const std::filesystem::path& folder, const std::string& name) {
    std::filesystem::path instr_path;
    std::string target = name;
    std::transform(target.begin(), target.end(), target.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    for (const auto& ent : std::filesystem::directory_iterator(folder)) {
        if (!ent.is_regular_file()) {
            continue;
        }
        auto stem = ent.path().stem().string();
        std::transform(stem.begin(), stem.end(), stem.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        auto ext = ent.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (stem == target && (ext == ".instr" || ext.empty())) {
            instr_path = ent.path();
            if (ext == ".instr") {
                break;
            }
        }
    }
    if (instr_path.empty()) {
        throw std::runtime_error("Instrument not found: " + name);
    }
    auto data = read_all(instr_path);
    if (data.size() >= 12 && std::memcmp(data.data(), "FORM", 4) == 0 &&
        std::memcmp(data.data() + 8, "8SVX", 4) == 0) {
        return load_8svx(data, name);
    }
    if (data.size() == 128 && std::memcmp(data.data(), "SampledSound", 12) == 0) {
        return load_sampled_instr(instr_path, data);
    }
    if (data.size() == 502) {
        return load_synth_instr(data, name);
    }
    if (data.size() == 128) {
        return load_sampled_instr(instr_path, data);
    }
    throw std::runtime_error("Unknown instrument format: " + instr_path.string());
}

}  // namespace smus
