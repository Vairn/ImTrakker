#include "smus/smus.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace smus {
namespace {

void put_be16(std::vector<uint8_t>& o, int v) {
    o.push_back(uint8_t((v >> 8) & 0xFF));
    o.push_back(uint8_t(v & 0xFF));
}

void put_be32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(uint8_t((v >> 24) & 0xFF));
    o.push_back(uint8_t((v >> 16) & 0xFF));
    o.push_back(uint8_t((v >> 8) & 0xFF));
    o.push_back(uint8_t(v & 0xFF));
}

void put_id(std::vector<uint8_t>& o, const char* id) {
    o.insert(o.end(), id, id + 4);
}

void put_chunk(std::vector<uint8_t>& o, const char* id, const std::vector<uint8_t>& body) {
    put_id(o, id);
    put_be32(o, uint32_t(body.size()));
    o.insert(o.end(), body.begin(), body.end());
    if (body.size() & 1) {
        o.push_back(0);
    }
}

}  // namespace

void save_score(const Score& score, const std::filesystem::path& path) {
    std::vector<uint8_t> inner;

    {
        std::vector<uint8_t> shdr;
        put_be16(shdr, score.tempo);
        shdr.push_back(uint8_t(std::clamp(score.volume, 0, 255)));
        shdr.push_back(0);
        put_chunk(inner, "SHDR", shdr);
    }

    if (!score.name.empty()) {
        std::vector<uint8_t> name(score.name.begin(), score.name.end());
        name.push_back(0);
        put_chunk(inner, "NAME", name);
    }

    for (const auto& [reg, name] : score.instruments) {
        std::vector<uint8_t> body(4, 0);
        body[0] = uint8_t(reg);
        body.insert(body.end(), name.begin(), name.end());
        body.push_back(0);
        put_chunk(inner, "INS1", body);
    }

    for (const auto& trak : score.tracks) {
        std::vector<uint8_t> body;
        body.reserve(trak.size() * 2);
        for (const SEvent& ev : trak) {
            body.push_back(ev.sid);
            body.push_back(ev.data);
        }
        put_chunk(inner, "TRAK", body);
    }

    std::vector<uint8_t> form;
    put_id(form, "FORM");
    put_be32(form, uint32_t(4 + inner.size()));
    put_id(form, "SMUS");
    form.insert(form.end(), inner.begin(), inner.end());

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write: " + path.string());
    }
    out.write(reinterpret_cast<const char*>(form.data()), std::streamsize(form.size()));
}

}  // namespace smus
