#pragma once

#include "mod/module.hpp"

#include <vector>

namespace mod {

struct SampleSel {
    int start = 0;  // byte index into Sample::wave
    int end = 0;    // exclusive
    bool active() const { return end > start; }
};

// Sample-buffer clipboard (separate from pattern clipboard).
struct SampleClipboard {
    std::vector<float> wave;
};

void sample_sel_all(Sample& s, SampleSel& sel);
void sample_sel_clamp(const Sample& s, SampleSel& sel);

void sample_cut(Sample& s, SampleSel& sel, SampleClipboard& clip);
void sample_copy(const Sample& s, const SampleSel& sel, SampleClipboard& clip);
void sample_paste(Sample& s, SampleSel& sel, const SampleClipboard& clip);
void sample_clear(Sample& s, SampleSel& sel);
void sample_crop(Sample& s, SampleSel& sel);
void sample_reverse(Sample& s, SampleSel& sel);
void sample_invert(Sample& s, SampleSel& sel);
void sample_amplify(Sample& s, SampleSel& sel, float gain);
void sample_fade_in(Sample& s, SampleSel& sel);
void sample_fade_out(Sample& s, SampleSel& sel);
void sample_boost(Sample& s, SampleSel& sel);
void sample_filter(Sample& s, SampleSel& sel);
void sample_resample(Sample& s, SampleSel& sel, float ratio);  // 0.5 = octave down (longer)
void sample_set_loop_from_sel(Sample& s, const SampleSel& sel);
void sample_disable_loop(Sample& s);
void sample_zero_leadin(Sample& s);

}  // namespace mod
