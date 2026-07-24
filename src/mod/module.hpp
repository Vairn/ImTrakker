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
    int volume = 64;
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
    std::vector<Sample> samples;
    int song_length = 0;
    int restart = 0;
    std::vector<int> orders;
    // patterns[pat][row][ch] — row count may vary (MED/SMUS)
    std::vector<std::vector<std::vector<Note>>> patterns;
    std::filesystem::path path;
    int initial_speed = 6;
    int initial_tempo = 125;

    int pattern_count() const { return int(patterns.size()); }
};

Module load_module(const std::filesystem::path& path);
Module load_module_bytes(std::vector<uint8_t> data, std::filesystem::path path = {});

Module load_protracker(std::vector<uint8_t> data, std::filesystem::path path);
Module load_mmd(std::vector<uint8_t> data, std::filesystem::path path);

}  // namespace mod
