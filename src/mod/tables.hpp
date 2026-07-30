#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <string_view>

namespace mod {

constexpr int kSampleRate = 44100;
constexpr double kPaulaClock = 3546895.0;  // PAL
constexpr int kRows = 64;                 // ProTracker default; patterns may be longer/shorter
constexpr int kMaxChannels = 16;
constexpr int kScopeSamples = 96;

// Finetune 0 (also exposed as kPeriodTable for loaders / UI note names).
inline constexpr std::array<int, 36> kPeriodTable = {
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
};

// ProTracker 2.3d mt_periodtab — 16 finetune tables × 36 notes (C-1..B-3).
// Index 0..7 = finetune 0..+7, 8..15 = finetune -8..-1.
inline constexpr std::array<std::array<int, 36>, 16> kPeriodTables = {{
    {{856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
      428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
      214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113}},
    {{850, 802, 757, 715, 674, 637, 601, 567, 535, 505, 477, 450,
      425, 401, 379, 357, 337, 318, 300, 284, 268, 253, 239, 225,
      213, 201, 189, 179, 169, 159, 150, 142, 134, 126, 119, 113}},
    {{844, 796, 752, 709, 670, 632, 597, 563, 532, 502, 474, 447,
      422, 398, 376, 355, 335, 316, 298, 282, 266, 251, 237, 224,
      211, 199, 188, 177, 167, 158, 149, 141, 133, 125, 118, 112}},
    {{838, 791, 746, 704, 665, 628, 592, 559, 528, 498, 470, 444,
      419, 395, 373, 352, 332, 314, 296, 280, 264, 249, 235, 222,
      209, 198, 187, 176, 166, 157, 148, 140, 132, 125, 118, 111}},
    {{832, 785, 741, 699, 660, 623, 588, 555, 524, 495, 467, 441,
      416, 392, 370, 350, 330, 312, 294, 278, 262, 247, 233, 220,
      208, 196, 185, 175, 165, 156, 147, 139, 131, 124, 117, 110}},
    {{826, 779, 736, 694, 655, 619, 584, 551, 520, 491, 463, 437,
      413, 390, 368, 347, 328, 309, 292, 276, 260, 245, 232, 219,
      206, 195, 184, 174, 164, 155, 146, 138, 130, 123, 116, 109}},
    {{820, 774, 730, 689, 651, 614, 580, 547, 516, 487, 460, 434,
      410, 387, 365, 345, 325, 307, 290, 274, 258, 244, 230, 217,
      205, 193, 183, 172, 163, 154, 145, 137, 129, 122, 115, 109}},
    {{814, 768, 725, 684, 646, 610, 575, 543, 513, 484, 457, 431,
      407, 384, 363, 342, 323, 305, 288, 272, 256, 242, 228, 216,
      204, 192, 181, 171, 161, 152, 144, 136, 128, 121, 114, 108}},
    {{907, 856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480,
      453, 428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240,
      226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120}},
    {{900, 850, 802, 757, 715, 675, 636, 601, 567, 535, 505, 477,
      450, 425, 401, 379, 357, 337, 318, 300, 284, 268, 253, 238,
      225, 212, 200, 189, 179, 169, 159, 150, 142, 134, 126, 119}},
    {{894, 844, 796, 752, 709, 670, 632, 597, 563, 532, 502, 474,
      447, 422, 398, 376, 355, 335, 316, 298, 282, 266, 251, 237,
      223, 211, 199, 188, 177, 167, 158, 149, 141, 133, 125, 118}},
    {{887, 838, 791, 746, 704, 665, 628, 592, 559, 528, 498, 470,
      444, 419, 395, 373, 352, 332, 314, 296, 280, 264, 249, 235,
      222, 209, 198, 187, 176, 166, 157, 148, 140, 132, 125, 118}},
    {{881, 832, 785, 741, 699, 660, 623, 588, 555, 524, 494, 467,
      441, 416, 392, 370, 350, 330, 312, 294, 278, 262, 247, 233,
      220, 208, 196, 185, 175, 165, 156, 147, 139, 131, 123, 117}},
    {{875, 826, 779, 736, 694, 655, 619, 584, 551, 520, 491, 463,
      437, 413, 390, 368, 347, 328, 309, 292, 276, 260, 245, 232,
      219, 206, 195, 184, 174, 164, 155, 146, 138, 130, 123, 116}},
    {{868, 820, 774, 730, 689, 651, 614, 580, 547, 516, 487, 460,
      434, 410, 387, 365, 345, 325, 307, 290, 274, 258, 244, 230,
      217, 205, 193, 183, 172, 163, 154, 145, 137, 129, 122, 115}},
    {{862, 814, 768, 725, 684, 646, 610, 575, 543, 513, 484, 457,
      431, 407, 384, 363, 342, 323, 305, 288, 272, 256, 242, 228,
      216, 203, 192, 181, 171, 161, 152, 144, 136, 128, 121, 114}},
}};

// ProTracker vibrato/tremolo sine table (first quadrant, 32 entries).
inline constexpr std::array<int, 32> kVibTable = {
    0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97,  74,  49,  24,
};

// Invert-loop ("funkrepeat") speed table.
inline constexpr std::array<int, 16> kFunkTable = {
    0, 5, 6, 7, 8, 10, 11, 13, 16, 19, 22, 26, 32, 43, 64, 128,
};

inline constexpr std::array<const char*, 36> kNoteNames = {
    "C-1", "C#1", "D-1", "D#1", "E-1", "F-1", "F#1", "G-1", "G#1", "A-1", "A#1", "B-1",
    "C-2", "C#2", "D-2", "D#2", "E-2", "F-2", "F#2", "G-2", "G#2", "A-2", "A#2", "B-2",
    "C-3", "C#3", "D-3", "D#3", "E-3", "F-3", "F#3", "G-3", "G#3", "A-3", "A#3", "B-3",
};

// Signed finetune (-8..7) → period-table index (0..15).
inline int finetune_table_index(int finetune) {
    const int ft = std::clamp(finetune, -8, 7);
    return ft < 0 ? ft + 16 : ft;
}

inline int period_for_note(int note_index, int finetune) {
    const int idx = std::clamp(note_index, 0, 35);
    return kPeriodTables[size_t(finetune_table_index(finetune))][size_t(idx)];
}

inline int channels_for_magic(std::string_view magic) {
    if (magic == "FLT4" || magic == "M.K." || magic == "M!K!" || magic == "4CHN" ||
        magic == "2CHN" || magic == "3CHN") {
        if (magic == "2CHN") {
            return 2;
        }
        if (magic == "3CHN") {
            return 3;
        }
        return 4;
    }
    if (magic == "5CHN") {
        return 5;
    }
    if (magic == "6CHN") {
        return 6;
    }
    if (magic == "7CHN") {
        return 7;
    }
    if (magic == "8CHN" || magic == "FLT8") {
        return 8;
    }
    return 0;
}

// MED/OctaMED note number: 1 = C-1 ... ; 0 = empty
inline int med_note_to_period(int note) {
    if (note <= 0) {
        return 0;
    }
    int idx = note - 1;
    int period = kPeriodTable[size_t(idx % 12 + 24)];  // start from C-3 octave as base mid
    // Prefer exact table for first 3 octaves
    if (idx < 36) {
        return kPeriodTable[size_t(idx)];
    }
    // Higher octaves: halve periods
    int oct = idx / 12;
    int semi = idx % 12;
    period = kPeriodTable[size_t(24 + semi)];  // C-3..B-3
    for (int o = 3; o < oct; ++o) {
        period = std::max(28, period / 2);
    }
    return period;
}

// SMUS / MIDI tone (60 = middle C) -> Amiga period. C-1 ≈ MIDI 24 in Instant Music.
inline int midi_to_period(int midi) {
    if (midi <= 0) {
        return 0;
    }
    // Map MIDI 24 (C1) to period table C-1 (856)
    const double semis = double(midi - 24);
    const double period = 856.0 * std::pow(2.0, -semis / 12.0);
    return std::clamp(int(std::lround(period)), 28, 907);
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

// Nearest note index within a specific finetune table (for arpeggio / glissando).
inline int nearest_period_index_ft(int period, int finetune) {
    const auto& tab = kPeriodTables[size_t(finetune_table_index(finetune))];
    int best_i = 0;
    int best_d = 100000;
    for (int i = 0; i < 36; ++i) {
        const int d = std::abs(tab[size_t(i)] - period);
        if (d < best_d) {
            best_d = d;
            best_i = i;
        }
    }
    return best_i;
}

}  // namespace mod
