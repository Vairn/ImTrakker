#pragma once

#include "mod/module.hpp"

#include <filesystem>
#include <string>

namespace mod {

// Max ProTracker sample bytes (~64k words).
constexpr int kMaxSampleBytes = 0xFFFE * 2;

Sample load_sample_file(const std::filesystem::path& path, bool raw_unsigned = false);
Sample load_sample_from_mod(const std::filesystem::path& path, int instrument_1based);

void save_sample_wav(const Sample& s, const std::filesystem::path& path);
void save_sample_8svx(const Sample& s, const std::filesystem::path& path);
void save_sample_raw(const Sample& s, const std::filesystem::path& path);

void sync_sample_length(Sample& s);
void clamp_sample_pt(Sample& s);

}  // namespace mod
