#pragma once

#include <array>
#include <cstdlib>
#include <cstdint>
#include <string_view>

namespace mod {

constexpr int kSampleRate = 44100;
constexpr double kPaulaClock = 3546895.0;  // PAL
constexpr int kRows = 64;
constexpr int kMaxChannels = 8;
constexpr int kScopeSamples = 96;

inline constexpr std::array<int, 36> kPeriodTable = {
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
};

inline constexpr std::array<const char*, 36> kNoteNames = {
    "C-1", "C#1", "D-1", "D#1", "E-1", "F-1", "F#1", "G-1", "G#1", "A-1", "A#1", "B-1",
    "C-2", "C#2", "D-2", "D#2", "E-2", "F-2", "F#2", "G-2", "G#2", "A-2", "A#2", "B-2",
    "C-3", "C#3", "D-3", "D#3", "E-3", "F-3", "F#3", "G-3", "G#3", "A-3", "A#3", "B-3",
};

inline int channels_for_magic(std::string_view magic) {
    if (magic == "FLT4" || magic == "M.K." || magic == "M!K!" || magic == "4CHN") {
        return 4;
    }
    if (magic == "6CHN") {
        return 6;
    }
    if (magic == "8CHN" || magic == "FLT8") {
        return 8;
    }
    return 0;
}

inline const char* period_to_note(int period) {
    if (period <= 0) {
        return "---";
    }
    int best_i = 0;
    int best_d = 100000;
    for (int i = 0; i < int(kPeriodTable.size()); ++i) {
        const int d = std::abs(kPeriodTable[size_t(i)] - period);
        if (d < best_d) {
            best_d = d;
            best_i = i;
        }
    }
    return kNoteNames[size_t(best_i)];
}

inline int nearest_period_index(int period) {
    int best_i = 0;
    int best_d = 100000;
    for (int i = 0; i < int(kPeriodTable.size()); ++i) {
        const int d = std::abs(kPeriodTable[size_t(i)] - period);
        if (d < best_d) {
            best_d = d;
            best_i = i;
        }
    }
    return best_i;
}

}  // namespace mod
