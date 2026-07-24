#include "mod/module.hpp"
#include "hsq.hpp"

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
    if (data.size() < 1084) {
        throw std::runtime_error("too small for a ProTracker module");
    }

    Module mod;
    mod.path = std::move(path);

    auto asciiz = [](const uint8_t* p, size_t n) {
        size_t len = 0;
        while (len < n && p[len] != 0) {
            ++len;
        }
        return std::string(reinterpret_cast<const char*>(p), len);
    };

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

    mod.magic.assign(reinterpret_cast<const char*>(data.data() + 1080), 4);
    mod.channels = channels_for_magic(mod.magic);
    if (mod.channels <= 0) {
        // 15-instrument SoundTracker: no magic, patterns start at 600
        throw std::runtime_error("unsupported magic: " + mod.magic);
    }
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
            s.data = {0.f, 0.f};
            continue;
        }
        if (sample_pos + nbytes > data.size()) {
            s.data.assign(2, 0.f);
            break;
        }
        s.data.resize(nbytes);
        for (size_t i = 0; i < nbytes; ++i) {
            s.data[i] = float(int8_t(data[sample_pos + i])) / 128.f;
        }
        if (nbytes >= 2) {
            s.data[0] = 0.f;
            s.data[1] = 0.f;
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

    return load_protracker(std::move(data), std::move(path));
}

Module load_module(const std::filesystem::path& path) {
    return load_module_bytes(read_file(path), path);
}

}  // namespace mod
