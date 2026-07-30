#include "mod/module.hpp"
#include "hsq.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace mod {

static uint16_t be16(const uint8_t* p) {
    return uint16_t((p[0] << 8) | p[1]);
}

std::string Note::text() const {
    char buf[16];
    const char* note = period_to_note(period);
    if (instrument) {
        std::snprintf(buf, sizeof(buf), "%s %02X ", note, instrument);
    } else {
        std::snprintf(buf, sizeof(buf), "%s .. ", note);
    }
    std::string out = buf;
    if (effect || param) {
        char fx[8];
        std::snprintf(fx, sizeof(fx), "%X%02X", effect, param);
        out += fx;
    } else {
        out += "...";
    }
    return out;
}

static std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open: " + path.string());
    }
    in.seekg(0, std::ios::end);
    const auto len = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    if (len > 0) {
        in.read(reinterpret_cast<char*>(buf.data()), len);
    }
    return buf;
}

Module load_protracker(std::vector<uint8_t> data, std::filesystem::path path) {
    if (data.size() < 600) {
        throw std::runtime_error("too small for a ProTracker / SoundTracker module");
    }

    auto asciiz = [](const uint8_t* p, size_t n) {
        size_t len = 0;
        while (len < n && p[len] != 0) {
            ++len;
        }
        return std::string(reinterpret_cast<const char*>(p), len);
    };

    std::string magic;
    int channels = 0;
    if (data.size() >= 1084) {
        magic.assign(reinterpret_cast<const char*>(data.data() + 1080), 4);
        channels = channels_for_magic(magic);
    }

    if (channels <= 0) {
        // No ProTracker magic — try Ultimate SoundTracker / 15-instrument layout.
        return load_soundtracker15(std::move(data), std::move(path));
    }

    if (data.size() < 1084) {
        throw std::runtime_error("too small for a ProTracker module");
    }

    Module mod;
    mod.path = std::move(path);
    mod.magic = magic;
    mod.channels = channels;

    mod.title = asciiz(data.data(), 20);
    while (!mod.title.empty() && (mod.title.back() == ' ' || mod.title.back() == '\0')) {
        mod.title.pop_back();
    }

    mod.samples.resize(31);
    for (int i = 0; i < 31; ++i) {
        const size_t off = 20 + size_t(i) * 30;
        Sample& s = mod.samples[size_t(i)];
        s.name = asciiz(data.data() + off, 22);
        s.length_words = be16(data.data() + off + 22);
        int ft = data[off + 24] & 0x0F;
        if (ft >= 8) {
            ft -= 16;
        }
        s.finetune = ft;
        s.volume = data[off + 25];
        s.repstart_words = be16(data.data() + off + 26);
        s.replen_words = be16(data.data() + off + 28);
    }

    mod.song_length = data[950];
    mod.restart = data[951];
    mod.orders.assign(data.begin() + 952, data.begin() + 952 + 128);

    if (mod.restart >= mod.song_length) {
        mod.restart = 0;
    }

    int npat = 0;
    for (int i = 0; i < mod.song_length; ++i) {
        if (mod.orders[size_t(i)] > npat) {
            npat = mod.orders[size_t(i)];
        }
    }
    if (mod.song_length > 0) {
        ++npat;
    }

    const int ch = mod.channels;
    const int row_bytes = ch * 4;
    const int pat_bytes = kRows * row_bytes;
    const size_t base = 1084;
    if (base + size_t(npat) * size_t(pat_bytes) > data.size()) {
        throw std::runtime_error("truncated pattern data");
    }

    mod.patterns.resize(size_t(npat));
    for (int p = 0; p < npat; ++p) {
        mod.patterns[size_t(p)].resize(kRows);
        for (int row = 0; row < kRows; ++row) {
            mod.patterns[size_t(p)][size_t(row)].resize(size_t(ch));
            for (int c = 0; c < ch; ++c) {
                const size_t i =
                    base + size_t(p) * size_t(pat_bytes) + size_t(row) * size_t(row_bytes) + size_t(c) * 4;
                const uint8_t b0 = data[i];
                const uint8_t b1 = data[i + 1];
                const uint8_t b2 = data[i + 2];
                const uint8_t b3 = data[i + 3];
                Note& n = mod.patterns[size_t(p)][size_t(row)][size_t(c)];
                n.period = ((b0 & 0x0F) << 8) | b1;
                n.instrument = (b0 & 0xF0) | ((b2 & 0xF0) >> 4);
                n.effect = b2 & 0x0F;
                n.param = b3;
            }
        }
    }

    size_t sample_pos = base + size_t(npat) * size_t(pat_bytes);
    for (Sample& s : mod.samples) {
        const size_t nbytes = size_t(s.length_words) * 2;
        if (nbytes == 0) {
            s.wave = {0.f, 0.f};
            continue;
        }
        if (sample_pos + nbytes > data.size()) {
            s.wave.assign(2, 0.f);
            break;
        }
        s.wave.resize(nbytes);
        for (size_t i = 0; i < nbytes; ++i) {
            s.wave[i] = float(int8_t(data[sample_pos + i])) / 128.f;
        }
        if (nbytes >= 2) {
            s.wave[0] = 0.f;
            s.wave[1] = 0.f;
        }
        sample_pos += nbytes;
    }

    if (mod.title.empty() && !mod.path.empty()) {
        mod.title = mod.path.stem().string();
    }
    return mod;
}

static bool looks_like_soundtracker15(const std::vector<uint8_t>& data) {
    if (data.size() < 600) {
        return false;
    }
    const int song_length = data[470];
    if (song_length < 1 || song_length > 128) {
        return false;
    }
    int npat = 0;
    for (int i = 0; i < song_length; ++i) {
        const int ord = data[472 + i];
        if (ord > 63) {
            // Still possible but suspicious; allow up to 127 like PT.
            if (ord > 127) {
                return false;
            }
        }
        npat = std::max(npat, ord);
    }
    ++npat;
    const size_t pat_bytes = size_t(npat) * 1024;
    if (600 + pat_bytes > data.size()) {
        return false;
    }
    for (int i = 0; i < 15; ++i) {
        const size_t off = 20 + size_t(i) * 30;
        const int len_words = (data[off + 22] << 8) | data[off + 23];
        if (len_words < 0 || len_words > 0x7FFF) {
            return false;
        }
    }
    return true;
}

Module load_soundtracker15(std::vector<uint8_t> data, std::filesystem::path path) {
    if (!looks_like_soundtracker15(data)) {
        std::string magic;
        if (data.size() >= 1084) {
            magic.assign(reinterpret_cast<const char*>(data.data() + 1080), 4);
        }
        throw std::runtime_error(magic.empty() ? "unsupported module format"
                                               : ("unsupported magic: " + magic));
    }

    auto asciiz = [](const uint8_t* p, size_t n) {
        size_t len = 0;
        while (len < n && p[len] != 0) {
            ++len;
        }
        return std::string(reinterpret_cast<const char*>(p), len);
    };

    Module mod;
    mod.path = std::move(path);
    mod.magic = "M.K.";  // normalised for save / UI
    mod.channels = 4;
    mod.title = asciiz(data.data(), 20);
    while (!mod.title.empty() && (mod.title.back() == ' ' || mod.title.back() == '\0')) {
        mod.title.pop_back();
    }

    mod.samples.assign(31, Sample{});
    for (int i = 0; i < 15; ++i) {
        const size_t off = 20 + size_t(i) * 30;
        Sample& s = mod.samples[size_t(i)];
        s.name = asciiz(data.data() + off, 22);
        s.length_words = be16(data.data() + off + 22);
        s.finetune = 0;  // UST has no finetune nibble
        s.volume = std::min(64, int(data[off + 25]));
        // Ultimate SoundTracker stores repeat start in *bytes*; later trackers use words.
        const int rep_raw = be16(data.data() + off + 26);
        const int replen = be16(data.data() + off + 28);
        s.replen_words = replen;
        const int len_bytes = s.length_words * 2;
        if (rep_raw > 0 && rep_raw + replen * 2 > len_bytes && rep_raw <= len_bytes) {
            // Treat as byte offset.
            s.repstart_words = rep_raw / 2;
        } else {
            s.repstart_words = rep_raw;
        }
    }
    for (int i = 15; i < 31; ++i) {
        mod.samples[size_t(i)].volume = 64;
        mod.samples[size_t(i)].replen_words = 1;
        mod.samples[size_t(i)].wave = {0.f, 0.f};
    }

    mod.song_length = data[470];
    const int speed_byte = data[471];
    mod.restart = 0;
    // UST: byte 471 is tempo (BPM). NoiseTracker-ish reuse as restart if small.
    if (speed_byte >= 32) {
        mod.initial_tempo = speed_byte;
    } else if (speed_byte > 0 && speed_byte < mod.song_length) {
        mod.restart = speed_byte;
    }

    mod.orders.assign(128, 0);
    for (int i = 0; i < 128; ++i) {
        mod.orders[size_t(i)] = data[472 + i];
    }

    int npat = 0;
    for (int i = 0; i < mod.song_length; ++i) {
        npat = std::max(npat, mod.orders[size_t(i)]);
    }
    ++npat;

    constexpr int ch = 4;
    constexpr int row_bytes = ch * 4;
    constexpr int pat_bytes = kRows * row_bytes;
    const size_t base = 600;
    if (base + size_t(npat) * size_t(pat_bytes) > data.size()) {
        throw std::runtime_error("truncated SoundTracker pattern data");
    }

    mod.patterns.resize(size_t(npat));
    for (int p = 0; p < npat; ++p) {
        mod.patterns[size_t(p)].resize(kRows);
        for (int row = 0; row < kRows; ++row) {
            mod.patterns[size_t(p)][size_t(row)].resize(ch);
            for (int c = 0; c < ch; ++c) {
                const size_t i =
                    base + size_t(p) * size_t(pat_bytes) + size_t(row) * size_t(row_bytes) + size_t(c) * 4;
                const uint8_t b0 = data[i];
                const uint8_t b1 = data[i + 1];
                const uint8_t b2 = data[i + 2];
                const uint8_t b3 = data[i + 3];
                Note& n = mod.patterns[size_t(p)][size_t(row)][size_t(c)];
                n.period = ((b0 & 0x0F) << 8) | b1;
                n.instrument = (b0 & 0xF0) | ((b2 & 0xF0) >> 4);
                n.effect = b2 & 0x0F;
                n.param = b3;
            }
        }
    }

    size_t sample_pos = base + size_t(npat) * size_t(pat_bytes);
    for (int i = 0; i < 15; ++i) {
        Sample& s = mod.samples[size_t(i)];
        const size_t nbytes = size_t(s.length_words) * 2;
        if (nbytes == 0) {
            s.wave = {0.f, 0.f};
            continue;
        }
        if (sample_pos + nbytes > data.size()) {
            s.wave.assign(2, 0.f);
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
        sample_pos += nbytes;
    }

    if (mod.title.empty() && !mod.path.empty()) {
        mod.title = mod.path.stem().string();
    }
    return mod;
}

Module load_module_bytes(std::vector<uint8_t> data, std::filesystem::path path) {
    if (data.size() >= 6 && hsq_header_valid(data.data(), data.size()) && data[0] < 0x40) {
        try {
            auto dec = hsq_decompress(data.data(), data.size());
            if (!dec.empty()) {
                data = std::move(dec);
            }
        } catch (...) {
            // keep original bytes
        }
    }

    if (data.size() >= 12 && std::memcmp(data.data(), "FORM", 4) == 0 &&
        std::memcmp(data.data() + 8, "SMUS", 4) == 0) {
        throw std::runtime_error("SMUS scores use the Sonix engine — open via the UI/CLI smus path");
    }

    if (data.size() >= 52 && (std::memcmp(data.data(), "MMD0", 4) == 0 ||
                              std::memcmp(data.data(), "MMD1", 4) == 0 ||
                              std::memcmp(data.data(), "MMD2", 4) == 0 ||
                              std::memcmp(data.data(), "MMD3", 4) == 0)) {
        return load_mmd(std::move(data), std::move(path));
    }

    if ((data.size() >= 64 && std::memcmp(data.data() + 60, "SONG", 4) == 0) ||
        (data.size() >= 128 && std::memcmp(data.data() + 124, "SO31", 4) == 0)) {
        return load_sfx(std::move(data), std::move(path));
    }

    if ((data.size() >= 17 && std::memcmp(data.data(), "Extended Module: ", 17) == 0) ||
        (data.size() >= 0x30 && std::memcmp(data.data() + 0x2C, "SCRM", 4) == 0) ||
        (data.size() >= 4 && std::memcmp(data.data(), "IMPM", 4) == 0)) {
        return load_extended(std::move(data), std::move(path));
    }

    return load_protracker(std::move(data), std::move(path));
}

Module load_module(const std::filesystem::path& path) {
    return load_module_bytes(read_file(path), path);
}

}  // namespace mod
