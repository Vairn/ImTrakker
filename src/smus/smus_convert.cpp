#include "smus/smus.hpp"
#include "mod/module.hpp"
#include "mod/tables.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace smus {
namespace {

std::vector<float> bake_instrument_wave(const Instrument& inst) {
    if (!inst.ss_data.empty()) {
        return inst.ss_data;
    }
    if (!inst.wave.empty()) {
        return inst.wave;
    }
    if (inst.filter_banks.size() >= 128) {
        return std::vector<float>(inst.filter_banks.begin(), inst.filter_banks.begin() + 128);
    }
    // fallback soft square
    std::vector<float> w(64);
    for (size_t i = 0; i < w.size(); ++i) {
        w[i] = (i & 16) ? 0.2f : -0.2f;
    }
    return w;
}

int duration_div_for_rows(int rows) {
    // Inverse of display bake: 32nd=1 row → division 5
    rows = std::max(1, rows);
    int best = 5;
    int best_d = 1000;
    for (int div = 0; div <= 5; ++div) {
        int r = 1 << std::max(0, 5 - div);
        const int d = std::abs(r - rows);
        if (d < best_d) {
            best_d = d;
            best = div;
        }
    }
    return best;
}

}  // namespace

mod::Module convert_to_mod(const Score& score,
                           const std::unordered_map<int, Instrument>& instruments) {
    const DisplayPattern disp = bake_display_pattern(score);
    mod::Module mod = mod::make_blank(std::clamp(disp.channels, 2, 8));
    mod.title = score.name.empty() ? "smus" : score.name;
    if (mod.title.size() > 20) {
        mod.title.resize(20);
    }
    mod.initial_tempo = std::clamp(int(std::lround(std::max(score.tempo / 128.f, 32.f))), 32, 255);
    mod.initial_speed = 6;
    mod.magic = mod::magic_for_channels(mod.channels);
    mod.path.clear();

    // Map SMUS registers → sample slots 1..31
    std::unordered_map<int, int> reg_to_slot;
    int next_slot = 0;
    auto ensure_slot = [&](int reg) -> int {
        auto it = reg_to_slot.find(reg);
        if (it != reg_to_slot.end()) {
            return it->second;
        }
        if (next_slot >= 31) {
            return 0;
        }
        const int slot = next_slot++;
        reg_to_slot[reg] = slot;
        mod::Sample& s = mod.samples[size_t(slot)];
        auto iit = instruments.find(reg);
        const Instrument* inst = (iit != instruments.end()) ? &iit->second : nullptr;
        std::string name;
        auto nit = score.instruments.find(reg);
        if (nit != score.instruments.end()) {
            name = nit->second;
        } else if (inst) {
            name = inst->name;
        } else {
            name = "ins" + std::to_string(reg);
        }
        if (name.size() > 22) {
            name.resize(22);
        }
        s.name = name;
        s.wave = inst ? bake_instrument_wave(*inst) : bake_instrument_wave(default_instrument());
        if (s.wave.size() < 2) {
            s.wave = {0.f, 0.f};
        }
        s.length_words = int(s.wave.size() / 2);
        s.volume = inst ? std::clamp(int(inst->volume * 64.f), 1, 64) : 64;
        if (inst && inst->loop_end > inst->loop_start + 2) {
            s.repstart_words = inst->loop_start / 2;
            s.replen_words = std::max(1, (inst->loop_end - inst->loop_start) / 2);
        } else {
            s.repstart_words = 0;
            s.replen_words = std::max(1, s.length_words);
        }
        return slot;
    };

    for (const auto& [reg, _] : score.instruments) {
        ensure_slot(reg);
    }

    const int rows = std::max(1, disp.rows);
    const int ch = mod.channels;
    // Split into 64-row PT patterns when possible, else one long pattern (save will warn).
    const int pat_len = (rows <= 256) ? rows : 64;
    const int npats = (rows + pat_len - 1) / pat_len;
    mod.patterns.assign(size_t(npats), {});
    mod.song_length = npats;
    mod.orders.assign(128, 0);
    for (int p = 0; p < npats; ++p) {
        mod.orders[size_t(p)] = p;
        const int row0 = p * pat_len;
        const int n = std::min(pat_len, rows - row0);
        auto& pat = mod.patterns[size_t(p)];
        pat.resize(size_t(n));
        for (int r = 0; r < n; ++r) {
            pat[size_t(r)].assign(size_t(ch), mod::Note{});
            if (row0 + r >= int(disp.cells.size())) {
                continue;
            }
            const auto& crow = disp.cells[size_t(row0 + r)];
            for (int c = 0; c < ch && c < int(crow.size()); ++c) {
                const PatternCell& cell = crow[size_t(c)];
                if (cell.midi <= 0 || cell.rest) {
                    continue;
                }
                mod::Note& note = pat[size_t(r)][size_t(c)];
                note.period = mod::midi_to_period(cell.midi);
                const int reg = cell.instrument >= 0 ? cell.instrument : 0;
                note.instrument = ensure_slot(reg) + 1;
                if (cell.volume >= 0) {
                    note.effect = 0xC;
                    note.param = std::clamp(cell.volume, 0, 64);
                }
            }
        }
    }
    return mod;
}

std::vector<SEvent> events_from_display_track(const DisplayPattern& pat, int ch, int default_ins) {
    std::vector<SEvent> out;
    if (ch < 0 || ch >= pat.channels || pat.rows <= 0) {
        return out;
    }
    int cur_ins = default_ins;
    int r = 0;
    while (r < pat.rows) {
        const PatternCell& cell = pat.cells[size_t(r)][size_t(ch)];
        if (cell.midi > 0 && !cell.rest) {
            if (cell.instrument >= 0 && cell.instrument != cur_ins) {
                cur_ins = cell.instrument;
                out.push_back(SEvent{0x81, uint8_t(cur_ins)});
            }
            if (cell.volume >= 0) {
                out.push_back(SEvent{0x84, uint8_t(std::clamp(cell.volume * 127 / 64, 1, 127))});
            }
            int span = 1;
            while (r + span < pat.rows) {
                const PatternCell& n = pat.cells[size_t(r + span)][size_t(ch)];
                if (n.midi > 0 || n.rest) {
                    break;
                }
                ++span;
            }
            SEvent note;
            note.sid = uint8_t(std::clamp(cell.midi, 0, 127));
            note.data = uint8_t(duration_div_for_rows(span));
            out.push_back(note);
            r += span;
        } else {
            int span = 1;
            while (r + span < pat.rows) {
                const PatternCell& n = pat.cells[size_t(r + span)][size_t(ch)];
                if (n.midi > 0 && !n.rest) {
                    break;
                }
                ++span;
            }
            out.push_back(SEvent{0x80, uint8_t(duration_div_for_rows(span))});
            r += span;
        }
    }
    return out;
}

}  // namespace smus
