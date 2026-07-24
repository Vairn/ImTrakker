#pragma once

#include "mod/tables.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mod {

struct Sample {
    std::string name;
    int length_words = 0;
    int finetune = 0;
    int volume = 0;
    int repstart_words = 0;
    int replen_words = 0;
    std::vector<float> data;  // signed PCM normalized to [-1,1]
};

struct Note {
    int period = 0;
    int instrument = 0;
    int effect = 0;
    int param = 0;

    std::string text() const;
};

struct Module {
    std::string title;
    std::string magic;
    int channels = 4;
    std::vector<Sample> samples;          // 31
    int song_length = 0;
    int restart = 0;
    std::vector<int> orders;              // 128
    // patterns[pat][row][ch]
    std::vector<std::vector<std::vector<Note>>> patterns;
    std::filesystem::path path;

    int pattern_count() const { return int(patterns.size()); }
};

// Load ProTracker/Startrekker-family module. HSQ-unpacks if needed.
Module load_module(const std::filesystem::path& path);
Module load_module_bytes(std::vector<uint8_t> data, std::filesystem::path path = {});

}  // namespace mod
