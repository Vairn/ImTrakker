#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Cryo HSQ (Amiga Dune LAB_0886): optional unpack for still-packed modules.
std::vector<uint8_t> hsq_decompress(const uint8_t* data, size_t size);
bool hsq_header_valid(const uint8_t* data, size_t size, uint32_t* unpacked_out = nullptr,
                      uint16_t* packed_out = nullptr);
