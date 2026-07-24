#include "hsq.hpp"

#include <stdexcept>

bool hsq_header_valid(const uint8_t* data, size_t size, uint32_t* unpacked_out, uint16_t* packed_out) {
    if (size < 6) {
        return false;
    }
    uint8_t sum = 0;
    for (int i = 0; i < 6; ++i) {
        sum = static_cast<uint8_t>(sum + data[i]);
    }
    if (sum != 0xAB) {
        return false;
    }
    if (unpacked_out) {
        *unpacked_out = uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16);
    }
    if (packed_out) {
        *packed_out = uint16_t(data[3]) | (uint16_t(data[4]) << 8);
    }
    return true;
}

std::vector<uint8_t> hsq_decompress(const uint8_t* data, size_t size) {
    if (!hsq_header_valid(data, size)) {
        return {};
    }

    size_t i = 6;
    std::vector<uint8_t> out;
    out.reserve(size * 2);
    uint16_t queue = 0;

    auto get_bit = [&]() -> int {
        int bit = queue & 1;
        queue >>= 1;
        if (queue == 0) {
            if (i + 2 > size) {
                throw std::runtime_error("HSQ bitstream exhausted");
            }
            queue = uint16_t(data[i]) | (uint16_t(data[i + 1]) << 8);
            i += 2;
            bit = queue & 1;
            queue = uint16_t((queue >> 1) | 0x8000);
        }
        return bit;
    };

    try {
        for (;;) {
            if (get_bit()) {
                if (i >= size) {
                    break;
                }
                out.push_back(data[i++]);
            } else {
                int length = 0;
                int ofs = 0;
                if (!get_bit()) {
                    length = (length << 1) | get_bit();
                    length = (length << 1) | get_bit();
                    if (i >= size) {
                        throw std::runtime_error("HSQ method0 distance");
                    }
                    ofs = int(int16_t(uint16_t(0xFF00u | data[i++])));
                } else {
                    if (i + 2 > size) {
                        throw std::runtime_error("HSQ method1 word");
                    }
                    uint16_t word = uint16_t(data[i]) | (uint16_t(data[i + 1]) << 8);
                    i += 2;
                    length = word & 7;
                    ofs = int(int16_t((word >> 3) | 0xE000));
                    if (length == 0) {
                        if (i >= size) {
                            throw std::runtime_error("HSQ method1 len");
                        }
                        length = data[i++];
                        if (length == 0) {
                            break;
                        }
                    }
                }
                const int ptr = int(out.size()) + ofs;
                length += 2;
                if (ptr < 0) {
                    throw std::runtime_error("HSQ bad backref");
                }
                for (int n = 0; n < length; ++n) {
                    out.push_back(out[size_t(ptr + n)]);
                }
            }
        }
    } catch (const std::runtime_error&) {
        return {};
    }
    return out;
}
