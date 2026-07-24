#include "mod/sample_edit.hpp"
#include "mod/sample_io.hpp"

#include <algorithm>
#include <cmath>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace mod {

void sample_sel_all(Sample& s, SampleSel& sel) {
    sel.start = 0;
    sel.end = int(s.wave.size());
}

void sample_sel_clamp(const Sample& s, SampleSel& sel) {
    const int n = int(s.wave.size());
    sel.start = std::clamp(sel.start, 0, n);
    sel.end = std::clamp(sel.end, 0, n);
    if (sel.end < sel.start) {
        std::swap(sel.end, sel.start);
    }
}

static void sel_bounds(const Sample& s, SampleSel& sel, int& a, int& b) {
    sample_sel_clamp(s, sel);
    if (sel.active()) {
        a = sel.start;
        b = sel.end;
    } else {
        a = 0;
        b = int(s.wave.size());
    }
}

void sample_cut(Sample& s, SampleSel& sel, SampleClipboard& clip) {
    sample_copy(s, sel, clip);
    sample_clear(s, sel);
}

void sample_copy(const Sample& s, const SampleSel& sel_in, SampleClipboard& clip) {
    SampleSel sel = sel_in;
    int a, b;
    sel_bounds(s, sel, a, b);
    clip.wave.assign(s.wave.begin() + a, s.wave.begin() + b);
}

void sample_paste(Sample& s, SampleSel& sel, const SampleClipboard& clip) {
    if (clip.wave.empty()) {
        return;
    }
    sample_sel_clamp(s, sel);
    const int at = sel.active() ? sel.start : int(s.wave.size());
    s.wave.insert(s.wave.begin() + at, clip.wave.begin(), clip.wave.end());
    sel.start = at;
    sel.end = at + int(clip.wave.size());
    clamp_sample_pt(s);
    sample_zero_leadin(s);
}

void sample_clear(Sample& s, SampleSel& sel) {
    int a, b;
    sel_bounds(s, sel, a, b);
    if (a == 0 && b == int(s.wave.size())) {
        s.wave = {0.f, 0.f};
        s.repstart_words = 0;
        s.replen_words = 1;
    } else {
        s.wave.erase(s.wave.begin() + a, s.wave.begin() + b);
    }
    sel.start = a;
    sel.end = a;
    clamp_sample_pt(s);
    sample_zero_leadin(s);
}

void sample_crop(Sample& s, SampleSel& sel) {
    int a, b;
    sel_bounds(s, sel, a, b);
    std::vector<float> out(s.wave.begin() + a, s.wave.begin() + b);
    if (out.size() < 2) {
        out = {0.f, 0.f};
    }
    s.wave = std::move(out);
    sel.start = 0;
    sel.end = int(s.wave.size());
    s.repstart_words = 0;
    s.replen_words = 1;
    clamp_sample_pt(s);
    sample_zero_leadin(s);
}

void sample_reverse(Sample& s, SampleSel& sel) {
    int a, b;
    sel_bounds(s, sel, a, b);
    std::reverse(s.wave.begin() + a, s.wave.begin() + b);
    sample_zero_leadin(s);
}

void sample_invert(Sample& s, SampleSel& sel) {
    int a, b;
    sel_bounds(s, sel, a, b);
    for (int i = a; i < b; ++i) {
        s.wave[size_t(i)] = -s.wave[size_t(i)];
    }
    sample_zero_leadin(s);
}

void sample_amplify(Sample& s, SampleSel& sel, float gain) {
    int a, b;
    sel_bounds(s, sel, a, b);
    for (int i = a; i < b; ++i) {
        s.wave[size_t(i)] = std::clamp(s.wave[size_t(i)] * gain, -1.f, 1.f);
    }
    sample_zero_leadin(s);
}

void sample_fade_in(Sample& s, SampleSel& sel) {
    int a, b;
    sel_bounds(s, sel, a, b);
    const int n = b - a;
    if (n <= 1) {
        return;
    }
    for (int i = 0; i < n; ++i) {
        s.wave[size_t(a + i)] *= float(i) / float(n - 1);
    }
    sample_zero_leadin(s);
}

void sample_fade_out(Sample& s, SampleSel& sel) {
    int a, b;
    sel_bounds(s, sel, a, b);
    const int n = b - a;
    if (n <= 1) {
        return;
    }
    for (int i = 0; i < n; ++i) {
        s.wave[size_t(a + i)] *= float(n - 1 - i) / float(n - 1);
    }
    sample_zero_leadin(s);
}

void sample_boost(Sample& s, SampleSel& sel) {
    int a, b;
    sel_bounds(s, sel, a, b);
    float peak = 0.f;
    for (int i = a; i < b; ++i) {
        peak = std::max(peak, std::fabs(s.wave[size_t(i)]));
    }
    if (peak < 1e-6f) {
        return;
    }
    sample_amplify(s, sel, 0.99f / peak);
}

void sample_filter(Sample& s, SampleSel& sel) {
    int a, b;
    sel_bounds(s, sel, a, b);
    if (b - a < 2) {
        return;
    }
    std::vector<float> tmp(s.wave.begin() + a, s.wave.begin() + b);
    for (int i = 1; i < int(tmp.size()) - 1; ++i) {
        s.wave[size_t(a + i)] = (tmp[size_t(i - 1)] + tmp[size_t(i)] * 2.f + tmp[size_t(i + 1)]) * 0.25f;
    }
    sample_zero_leadin(s);
}

void sample_resample(Sample& s, SampleSel& sel, float ratio) {
    if (ratio <= 0.f) {
        return;
    }
    int a = 0;
    int b = 0;
    sel_bounds(s, sel, a, b);
    const int n = b - a;
    if (n < 2) {
        return;
    }
    const int out_n = (std::max)(2, int(std::lround(float(n) / ratio)));
    std::vector<float> resampled(static_cast<size_t>(out_n), 0.f);
    const float* src = s.wave.data();
    for (int i = 0; i < out_n; ++i) {
        const float pos = float(i) * float(n - 1) / float((std::max)(1, out_n - 1));
        const int i0 = int(pos);
        const int i1 = (std::min)(n - 1, i0 + 1);
        const float t = pos - float(i0);
        resampled[size_t(i)] = src[a + i0] * (1.f - t) + src[a + i1] * t;
    }
    auto& buf = s.wave;
    buf.erase(buf.begin() + a, buf.begin() + b);
    buf.insert(buf.begin() + a, resampled.begin(), resampled.end());
    sel.start = a;
    sel.end = a + out_n;
    clamp_sample_pt(s);
    sample_zero_leadin(s);
}

void sample_set_loop_from_sel(Sample& s, const SampleSel& sel_in) {
    SampleSel sel = sel_in;
    sample_sel_clamp(s, sel);
    if (!sel.active()) {
        return;
    }
    // Word-align
    int a = sel.start & ~1;
    int b = (sel.end + 1) & ~1;
    if (b <= a + 2) {
        b = a + 2;
    }
    s.repstart_words = a / 2;
    s.replen_words = std::max(1, (b - a) / 2);
    clamp_sample_pt(s);
}

void sample_disable_loop(Sample& s) {
    s.repstart_words = 0;
    s.replen_words = 1;
}

void sample_zero_leadin(Sample& s) {
    if (s.wave.size() >= 2) {
        s.wave[0] = 0.f;
        s.wave[1] = 0.f;
    }
    sync_sample_length(s);
}

}  // namespace mod
