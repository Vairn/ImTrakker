#include "mod/sample_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace mod {
namespace {

std::vector<uint8_t> read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open: " + path.string());
    }
    in.seekg(0, std::ios::end);
    const auto len = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(std::max<std::streamoff>(0, len)));
    if (!buf.empty()) {
        in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
    }
    return buf;
}

uint16_t be16(const uint8_t* p) {
    return uint16_t((p[0] << 8) | p[1]);
}

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint16_t le16(const uint8_t* p) {
    return uint16_t(p[0] | (p[1] << 8));
}

uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void put_be32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(uint8_t((v >> 24) & 0xFF));
    o.push_back(uint8_t((v >> 16) & 0xFF));
    o.push_back(uint8_t((v >> 8) & 0xFF));
    o.push_back(uint8_t(v & 0xFF));
}

void put_be16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(uint8_t((v >> 8) & 0xFF));
    o.push_back(uint8_t(v & 0xFF));
}

Sample from_pcm8(const int8_t* data, size_t nbytes, const std::string& name) {
    Sample s;
    s.name = name.substr(0, 22);
    s.volume = 64;
    s.replen_words = 1;
    size_t n = std::min(nbytes, size_t(kMaxSampleBytes));
    if (n & 1) {
        ++n;  // even byte length for words
    }
    n = std::min(n, size_t(kMaxSampleBytes));
    s.wave.assign(n, 0.f);
    for (size_t i = 0; i < nbytes && i < n; ++i) {
        s.wave[i] = float(data[i]) / 128.f;
    }
    if (n >= 2) {
        s.wave[0] = 0.f;
        s.wave[1] = 0.f;
    }
    sync_sample_length(s);
    return s;
}

Sample load_wav(const std::vector<uint8_t>& data, const std::string& name) {
    if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0 ||
        std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
        throw std::runtime_error("not a WAV file");
    }
    size_t pos = 12;
    uint16_t fmt = 0, channels = 0, bps = 0;
    const uint8_t* pcm = nullptr;
    size_t pcm_bytes = 0;
    while (pos + 8 <= data.size()) {
        const char* id = reinterpret_cast<const char*>(data.data() + pos);
        const uint32_t sz = le32(data.data() + pos + 4);
        const size_t body = pos + 8;
        if (body + sz > data.size()) {
            break;
        }
        if (std::memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            fmt = le16(data.data() + body);
            channels = le16(data.data() + body + 2);
            bps = le16(data.data() + body + 14);
        } else if (std::memcmp(id, "data", 4) == 0) {
            pcm = data.data() + body;
            pcm_bytes = sz;
        }
        pos = body + sz + (sz & 1);
    }
    if (!pcm || channels == 0 || (fmt != 1 && fmt != 3)) {
        throw std::runtime_error("unsupported WAV (need PCM)");
    }

    std::vector<float> mono;
    if (fmt == 1 && bps == 8) {
        const size_t frames = pcm_bytes / channels;
        mono.resize(frames);
        for (size_t i = 0; i < frames; ++i) {
            float sum = 0.f;
            for (int c = 0; c < channels; ++c) {
                sum += (float(pcm[i * channels + size_t(c)]) - 128.f) / 128.f;
            }
            mono[i] = sum / float(channels);
        }
    } else if (fmt == 1 && bps == 16) {
        const size_t frames = pcm_bytes / (2 * channels);
        mono.resize(frames);
        for (size_t i = 0; i < frames; ++i) {
            float sum = 0.f;
            for (int c = 0; c < channels; ++c) {
                const int16_t v = int16_t(le16(pcm + (i * channels + size_t(c)) * 2));
                sum += float(v) / 32768.f;
            }
            mono[i] = sum / float(channels);
        }
    } else if (fmt == 1 && bps == 24) {
        const size_t frames = pcm_bytes / (3 * channels);
        mono.resize(frames);
        for (size_t i = 0; i < frames; ++i) {
            float sum = 0.f;
            for (int c = 0; c < channels; ++c) {
                const size_t o = (i * channels + size_t(c)) * 3;
                int32_t v = int32_t(pcm[o] | (pcm[o + 1] << 8) | (pcm[o + 2] << 16));
                if (v & 0x800000) {
                    v |= ~0xFFFFFF;
                }
                sum += float(v) / 8388608.f;
            }
            mono[i] = sum / float(channels);
        }
    } else if (fmt == 3 && bps == 32) {
        const size_t frames = pcm_bytes / (4 * channels);
        mono.resize(frames);
        for (size_t i = 0; i < frames; ++i) {
            float sum = 0.f;
            for (int c = 0; c < channels; ++c) {
                float v;
                std::memcpy(&v, pcm + (i * channels + size_t(c)) * 4, 4);
                sum += v;
            }
            mono[i] = sum / float(channels);
        }
    } else {
        throw std::runtime_error("unsupported WAV bit depth");
    }

    if (mono.size() > size_t(kMaxSampleBytes)) {
        mono.resize(size_t(kMaxSampleBytes));
    }
    if (mono.size() & 1) {
        mono.push_back(0.f);
    }
    Sample s;
    s.name = name.substr(0, 22);
    s.volume = 64;
    s.replen_words = 1;
    s.wave = std::move(mono);
    if (s.wave.size() >= 2) {
        s.wave[0] = 0.f;
        s.wave[1] = 0.f;
    }
    sync_sample_length(s);
    return s;
}

Sample load_8svx(const std::vector<uint8_t>& data, const std::string& name) {
    if (data.size() < 12 || std::memcmp(data.data(), "FORM", 4) != 0 ||
        std::memcmp(data.data() + 8, "8SVX", 4) != 0) {
        throw std::runtime_error("not an IFF 8SVX file");
    }
    size_t pos = 12;
    uint32_t one_shot = 0, repeat = 0;
    int volume = 64;
    const uint8_t* body = nullptr;
    size_t body_len = 0;
    std::string sample_name = name;
    while (pos + 8 <= data.size()) {
        const char* id = reinterpret_cast<const char*>(data.data() + pos);
        const uint32_t sz = be32(data.data() + pos + 4);
        const size_t body_off = pos + 8;
        if (body_off + sz > data.size()) {
            break;
        }
        if (std::memcmp(id, "VHDR", 4) == 0 && sz >= 14) {
            one_shot = be32(data.data() + body_off);
            repeat = be32(data.data() + body_off + 4);
            // samplesPerHiCycle at +8, samplesPerSec at +10, ctOctave at +12, sCompression at +13
            if (sz >= 20) {
                const uint32_t vol = be32(data.data() + body_off + 16);
                volume = std::clamp(int(vol >> 16), 0, 64);
                if (volume == 0 && vol != 0) {
                    volume = 64;
                }
            }
        } else if (std::memcmp(id, "NAME", 4) == 0 && sz > 0) {
            sample_name.assign(reinterpret_cast<const char*>(data.data() + body_off),
                               strnlen(reinterpret_cast<const char*>(data.data() + body_off), sz));
        } else if (std::memcmp(id, "BODY", 4) == 0) {
            body = data.data() + body_off;
            body_len = sz;
        }
        pos = body_off + sz + (sz & 1);
    }
    if (!body || body_len == 0) {
        throw std::runtime_error("8SVX missing BODY");
    }
    Sample s = from_pcm8(reinterpret_cast<const int8_t*>(body), body_len, sample_name);
    s.volume = volume ? volume : 64;
    if (repeat > 0) {
        s.repstart_words = int(one_shot / 2);
        s.replen_words = std::max(1, int(repeat / 2));
    } else {
        s.repstart_words = 0;
        s.replen_words = 1;
    }
    clamp_sample_pt(s);
    return s;
}

}  // namespace

void sync_sample_length(Sample& s) {
    s.length_words = int(s.wave.size() / 2);
    if (s.length_words < 0) {
        s.length_words = 0;
    }
    if (s.replen_words <= 0) {
        s.replen_words = 1;
    }
}

void clamp_sample_pt(Sample& s) {
    if (int(s.wave.size()) > kMaxSampleBytes) {
        s.wave.resize(size_t(kMaxSampleBytes));
    }
    if (s.wave.size() & 1) {
        s.wave.push_back(0.f);
    }
    sync_sample_length(s);
    s.volume = std::clamp(s.volume, 0, 64);
    s.finetune = std::clamp(s.finetune, -8, 7);
    s.repstart_words = std::clamp(s.repstart_words, 0, std::max(0, s.length_words));
    if (s.replen_words <= 0) {
        s.replen_words = 1;
    }
    if (s.repstart_words + s.replen_words > s.length_words && s.length_words > 0) {
        s.replen_words = std::max(1, s.length_words - s.repstart_words);
    }
    if (s.name.size() > 22) {
        s.name.resize(22);
    }
}

Sample load_sample_file(const std::filesystem::path& path, bool raw_unsigned) {
    const auto data = read_all(path);
    const std::string stem = path.stem().string();
    if (data.size() >= 12 && std::memcmp(data.data(), "RIFF", 4) == 0) {
        return load_wav(data, stem);
    }
    if (data.size() >= 12 && std::memcmp(data.data(), "FORM", 4) == 0) {
        return load_8svx(data, stem);
    }
    // RAW signed or unsigned 8-bit
    std::vector<int8_t> pcm(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        if (raw_unsigned) {
            pcm[i] = int8_t(int(data[i]) - 128);
        } else {
            pcm[i] = int8_t(data[i]);
        }
    }
    return from_pcm8(pcm.data(), pcm.size(), stem);
}

Sample load_sample_from_mod(const std::filesystem::path& path, int instrument_1based) {
    Module m = load_module(path);
    if (instrument_1based < 1 || instrument_1based > int(m.samples.size())) {
        throw std::runtime_error("instrument out of range");
    }
    Sample s = m.samples[size_t(instrument_1based - 1)];
    clamp_sample_pt(s);
    return s;
}

void save_sample_wav(const Sample& s, const std::filesystem::path& path) {
    const int n = int(s.wave.size());
    std::vector<int16_t> pcm16(static_cast<size_t>(n));
    const float* src = s.wave.data();
    for (int i = 0; i < n; ++i) {
        float v = src[i];
        if (v < -1.f) {
            v = -1.f;
        }
        if (v > 1.f) {
            v = 1.f;
        }
        pcm16[static_cast<size_t>(i)] = static_cast<int16_t>(v * 32767.f);
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot write: " + path.string());
    }
    const int sr = 16726;  // ~C-3 Paula PAL-ish
    const int data_bytes = n * 2;
    const int byte_rate = sr * 2;
    f.write("RIFF", 4);
    const uint32_t chunk = 36 + uint32_t(data_bytes);
    f.write(reinterpret_cast<const char*>(&chunk), 4);
    f.write("WAVEfmt ", 8);
    const uint32_t fmt_size = 16;
    const uint16_t audio_fmt = 1;
    const uint16_t ch = 1;
    const uint16_t bps = 16;
    const uint16_t block_align = 2;
    f.write(reinterpret_cast<const char*>(&fmt_size), 4);
    f.write(reinterpret_cast<const char*>(&audio_fmt), 2);
    f.write(reinterpret_cast<const char*>(&ch), 2);
    f.write(reinterpret_cast<const char*>(&sr), 4);
    f.write(reinterpret_cast<const char*>(&byte_rate), 4);
    f.write(reinterpret_cast<const char*>(&block_align), 2);
    f.write(reinterpret_cast<const char*>(&bps), 2);
    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&data_bytes), 4);
    f.write(reinterpret_cast<const char*>(pcm16.data()), data_bytes);
}

void save_sample_8svx(const Sample& s, const std::filesystem::path& path) {
    const size_t nbytes = s.wave.size();
    std::vector<uint8_t> body(nbytes);
    for (size_t i = 0; i < nbytes; ++i) {
        body[i] = uint8_t(int8_t(std::clamp(int(std::lround(s.wave[i] * 127.f)), -128, 127)));
    }
    if (nbytes >= 2) {
        body[0] = 0;
        body[1] = 0;
    }

    const uint32_t one_shot = uint32_t(s.repstart_words * 2);
    const uint32_t repeat = (s.replen_words > 1) ? uint32_t(s.replen_words * 2) : 0;

    std::vector<uint8_t> form;
    form.insert(form.end(), {'F', 'O', 'R', 'M'});
    form.insert(form.end(), 4, 0);  // size later
    form.insert(form.end(), {'8', 'S', 'V', 'X'});

    // VHDR
    form.insert(form.end(), {'V', 'H', 'D', 'R'});
    put_be32(form, 20);
    put_be32(form, one_shot);
    put_be32(form, repeat);
    put_be32(form, 32);       // samplesPerHiCycle
    put_be16(form, 16726);    // samplesPerSec
    form.push_back(1);        // ctOctave
    form.push_back(0);        // sCompression
    put_be32(form, uint32_t(std::clamp(s.volume, 0, 64)) << 16);

    // NAME
    std::string nm = s.name;
    if (nm.empty()) {
        nm = "sample";
    }
    if (nm.size() & 1) {
        nm.push_back(0);
    }
    form.insert(form.end(), {'N', 'A', 'M', 'E'});
    put_be32(form, uint32_t(nm.size()));
    form.insert(form.end(), nm.begin(), nm.end());

    // BODY
    form.insert(form.end(), {'B', 'O', 'D', 'Y'});
    put_be32(form, uint32_t(body.size()));
    form.insert(form.end(), body.begin(), body.end());
    if (body.size() & 1) {
        form.push_back(0);
    }

    const uint32_t form_size = uint32_t(form.size() - 8);
    form[4] = uint8_t((form_size >> 24) & 0xFF);
    form[5] = uint8_t((form_size >> 16) & 0xFF);
    form[6] = uint8_t((form_size >> 8) & 0xFF);
    form[7] = uint8_t(form_size & 0xFF);

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot write: " + path.string());
    }
    f.write(reinterpret_cast<const char*>(form.data()), std::streamsize(form.size()));
}

void save_sample_raw(const Sample& s, const std::filesystem::path& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot write: " + path.string());
    }
    for (size_t i = 0; i < s.wave.size(); ++i) {
        float v = s.wave[i];
        if (i < 2) {
            v = 0.f;
        }
        const int8_t q = int8_t(std::clamp(int(std::lround(v * 127.f)), -128, 127));
        f.put(char(q));
    }
}

}  // namespace mod
