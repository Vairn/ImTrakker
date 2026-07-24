#include "mod/module.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace mod {

std::string magic_for_channels(int channels) {
    switch (channels) {
    case 2:
        return "2CHN";
    case 3:
        return "3CHN";
    case 4:
        return "M.K.";
    case 5:
        return "5CHN";
    case 6:
        return "6CHN";
    case 7:
        return "7CHN";
    case 8:
        return "8CHN";
    default:
        return "M.K.";
    }
}

Module make_blank(int channels) {
    channels = std::clamp(channels, 2, 8);
    Module mod;
    mod.title = "untitled";
    mod.magic = magic_for_channels(channels);
    mod.channels = channels;
    mod.samples.assign(31, Sample{});
    for (Sample& s : mod.samples) {
        s.volume = 64;
        s.replen_words = 1;
        s.wave = {0.f, 0.f};
    }
    mod.song_length = 1;
    mod.restart = 0;
    mod.orders.assign(128, 0);
    mod.patterns.resize(1);
    mod.patterns[0].assign(size_t(kRows), std::vector<Note>(size_t(channels)));
    mod.initial_speed = 6;
    mod.initial_tempo = 125;
    return mod;
}

static void put_be16(std::vector<uint8_t>& out, int v) {
    out.push_back(uint8_t((v >> 8) & 0xFF));
    out.push_back(uint8_t(v & 0xFF));
}

static void pad_ascii(std::vector<uint8_t>& out, const std::string& s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        out.push_back(i < s.size() ? uint8_t(s[i]) : 0);
    }
}

void save_protracker(const Module& mod, const std::filesystem::path& path) {
    if (mod.channels <= 0 || mod.channels > 8) {
        throw std::runtime_error("unsupported channel count for ProTracker save");
    }
    if (mod.song_length <= 0 || mod.song_length > 128) {
        throw std::runtime_error("invalid song length");
    }

    int npat = 0;
    for (int i = 0; i < mod.song_length && i < int(mod.orders.size()); ++i) {
        npat = std::max(npat, mod.orders[size_t(i)]);
    }
    ++npat;
    npat = std::max(1, std::min(npat, std::max(1, mod.pattern_count())));

    std::vector<uint8_t> out;
    out.reserve(1084 + size_t(npat) * size_t(kRows) * size_t(mod.channels) * 4 + 65536);

    pad_ascii(out, mod.title, 20);

    for (int i = 0; i < 31; ++i) {
        const Sample empty{};
        const Sample& s = (i < int(mod.samples.size())) ? mod.samples[size_t(i)] : empty;
        pad_ascii(out, s.name, 22);
        int len_words = s.length_words;
        if (len_words <= 0 && s.wave.size() > 2) {
            len_words = int(s.wave.size() / 2);
        }
        len_words = std::clamp(len_words, 0, 0xFFFF);
        put_be16(out, len_words);
        int ft = s.finetune;
        if (ft < 0) {
            ft += 16;
        }
        out.push_back(uint8_t(ft & 0x0F));
        out.push_back(uint8_t(std::clamp(s.volume, 0, 64)));
        put_be16(out, std::clamp(s.repstart_words, 0, 0xFFFF));
        int replen = s.replen_words;
        if (replen <= 0) {
            replen = 1;
        }
        put_be16(out, std::clamp(replen, 1, 0xFFFF));
    }

    out.push_back(uint8_t(mod.song_length));
    out.push_back(uint8_t(std::clamp(mod.restart, 0, 127)));
    for (int i = 0; i < 128; ++i) {
        const int o = (i < int(mod.orders.size())) ? mod.orders[size_t(i)] : 0;
        out.push_back(uint8_t(std::clamp(o, 0, 127)));
    }

    const std::string magic =
        mod.magic.size() == 4 ? mod.magic : magic_for_channels(mod.channels);
    for (char c : magic) {
        out.push_back(uint8_t(c));
    }

    const int ch = mod.channels;
    for (int p = 0; p < npat; ++p) {
        for (int row = 0; row < kRows; ++row) {
            for (int c = 0; c < ch; ++c) {
                Note n;
                if (p < mod.pattern_count() && row < int(mod.patterns[size_t(p)].size()) &&
                    c < int(mod.patterns[size_t(p)][size_t(row)].size())) {
                    n = mod.patterns[size_t(p)][size_t(row)][size_t(c)];
                }
                const int period = std::clamp(n.period, 0, 0x0FFF);
                const int ins = std::clamp(n.instrument, 0, 31);
                const int fx = n.effect & 0x0F;
                const int param = n.param & 0xFF;
                out.push_back(uint8_t(((ins & 0xF0)) | ((period >> 8) & 0x0F)));
                out.push_back(uint8_t(period & 0xFF));
                out.push_back(uint8_t(((ins & 0x0F) << 4) | fx));
                out.push_back(uint8_t(param));
            }
        }
    }

    for (int i = 0; i < 31; ++i) {
        const Sample empty{};
        const Sample& s = (i < int(mod.samples.size())) ? mod.samples[size_t(i)] : empty;
        int len_words = s.length_words;
        if (len_words <= 0 && s.wave.size() > 2) {
            len_words = int(s.wave.size() / 2);
        }
        len_words = std::clamp(len_words, 0, 0xFFFF);
        const size_t nbytes = size_t(len_words) * 2;
        if (nbytes == 0) {
            continue;
        }
        for (size_t b = 0; b < nbytes; ++b) {
            float v = (b < s.wave.size()) ? s.wave[b] : 0.f;
            if (b < 2) {
                v = 0.f;
            }
            const int8_t q = int8_t(std::clamp(int(std::lround(v * 127.f)), -128, 127));
            out.push_back(uint8_t(q));
        }
    }

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot write: " + path.string());
    }
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
}

}  // namespace mod
