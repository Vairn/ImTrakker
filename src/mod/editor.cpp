#include "mod/editor.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace mod {

int note_key_to_period(int keycode, int octave) {
    // Lower row: Z S X D C V G B H N J M  -> octave
    // Upper row: Q 2 W 3 E R 5 T 6 Y 7 U I -> octave+1
    struct Map {
        int key;
        int semi;
        int oct_add;
    };
    static const Map kMaps[] = {
        {'z', 0, 0},  {'s', 1, 0},  {'x', 2, 0},  {'d', 3, 0},  {'c', 4, 0},  {'v', 5, 0},
        {'g', 6, 0},  {'b', 7, 0},  {'h', 8, 0},  {'n', 9, 0},  {'j', 10, 0}, {'m', 11, 0},
        {'q', 0, 1},  {'2', 1, 1},  {'w', 2, 1},  {'3', 3, 1},  {'e', 4, 1},  {'r', 5, 1},
        {'5', 6, 1},  {'t', 7, 1},  {'6', 8, 1},  {'y', 9, 1},  {'7', 10, 1}, {'u', 11, 1},
        {'i', 0, 2},
    };
    const int base_oct = std::clamp(octave, 1, 3) - 1;  // 0..2 for C-1..C-3 base
    for (const auto& m : kMaps) {
        if (m.key == keycode) {
            const int idx = (base_oct + m.oct_add) * 12 + m.semi;
            if (idx >= 0 && idx < int(kPeriodTable.size())) {
                return kPeriodTable[size_t(idx)];
            }
        }
    }
    return 0;
}

void EditorState::reset_for_module(const Module& m) {
    pat = 0;
    row = 0;
    ch = 0;
    field = CellField::Note;
    hex_nibble = -1;
    has_sel = false;
    sample_slot = 0;
    sample_sel = {};
    wave_zoom = 1.f;
    wave_scroll = 0.f;
    undo_stack.clear();
    redo_stack.clear();
    dirty = false;
    std::memset(title_buf, 0, sizeof(title_buf));
    std::strncpy(title_buf, m.title.c_str(), sizeof(title_buf) - 1);
    if (m.song_length > 0 && !m.orders.empty()) {
        pat = std::clamp(m.orders[0], 0, std::max(0, m.pattern_count() - 1));
    }
}

void EditorState::push_undo(UndoOp op) {
    undo_stack.push_back(std::move(op));
    while (undo_stack.size() > kMaxUndo) {
        undo_stack.pop_front();
    }
    redo_stack.clear();
    mark_dirty();
}

bool EditorState::undo(Module& m, std::mutex& mutex) {
    if (undo_stack.empty()) {
        return false;
    }
    UndoOp op = std::move(undo_stack.back());
    undo_stack.pop_back();
    {
        std::lock_guard lock(mutex);
        op.undo(m);
    }
    redo_stack.push_back(std::move(op));
    mark_dirty();
    return true;
}

bool EditorState::redo(Module& m, std::mutex& mutex) {
    if (redo_stack.empty()) {
        return false;
    }
    UndoOp op = std::move(redo_stack.back());
    redo_stack.pop_back();
    {
        std::lock_guard lock(mutex);
        op.redo(m);
    }
    undo_stack.push_back(std::move(op));
    mark_dirty();
    return true;
}

Note* EditorState::note_at(Module& m) {
    ensure_pattern(m, pat);
    if (pat < 0 || pat >= m.pattern_count()) {
        return nullptr;
    }
    auto& p = m.patterns[size_t(pat)];
    if (row < 0 || row >= int(p.size())) {
        return nullptr;
    }
    auto& r = p[size_t(row)];
    if (ch < 0 || ch >= int(r.size())) {
        return nullptr;
    }
    return &r[size_t(ch)];
}

const Note* EditorState::note_at(const Module& m) const {
    if (pat < 0 || pat >= m.pattern_count()) {
        return nullptr;
    }
    const auto& p = m.patterns[size_t(pat)];
    if (row < 0 || row >= int(p.size())) {
        return nullptr;
    }
    const auto& r = p[size_t(row)];
    if (ch < 0 || ch >= int(r.size())) {
        return nullptr;
    }
    return &r[size_t(ch)];
}

void EditorState::ensure_pattern(Module& m, int p) {
    if (p < 0) {
        return;
    }
    while (m.pattern_count() <= p) {
        m.patterns.push_back(
            std::vector<std::vector<Note>>(size_t(kRows), std::vector<Note>(size_t(m.channels))));
    }
}

void EditorState::set_period(Module& m, std::mutex& mutex, int period, bool apply_instr) {
    const int p = pat, r = row, c = ch, ins = instrument;
    Note before, after;
    {
        std::lock_guard lock(mutex);
        ensure_pattern(m, pat);
        Note* n = note_at(m);
        if (!n) {
            return;
        }
        before = *n;
        n->period = period;
        if (apply_instr && period) {
            n->instrument = ins;
        }
        after = *n;
    }
    UndoOp op;
    op.label = "note";
    op.undo = [p, r, c, before](Module& mod) {
        if (p < mod.pattern_count() && r < int(mod.patterns[size_t(p)].size()) &&
            c < int(mod.patterns[size_t(p)][size_t(r)].size())) {
            mod.patterns[size_t(p)][size_t(r)][size_t(c)] = before;
        }
    };
    op.redo = [p, r, c, after](Module& mod) {
        if (p < mod.pattern_count() && r < int(mod.patterns[size_t(p)].size()) &&
            c < int(mod.patterns[size_t(p)][size_t(r)].size())) {
            mod.patterns[size_t(p)][size_t(r)][size_t(c)] = after;
        }
    };
    push_undo(std::move(op));
    advance_step(m);
}

void EditorState::set_instrument_digit(Module& m, std::mutex& mutex, int digit) {
    digit &= 0xF;
    const int p = pat, r = row, c = ch;
    Note before, after;
    {
        std::lock_guard lock(mutex);
        ensure_pattern(m, pat);
        Note* n = note_at(m);
        if (!n) {
            return;
        }
        before = *n;
        if (hex_nibble < 0) {
            hex_nibble = digit;
            n->instrument = digit;
        } else {
            n->instrument = ((hex_nibble & 0xF) << 4) | digit;
            if (n->instrument > 31) {
                n->instrument = 31;
            }
            hex_nibble = -1;
            instrument = std::max(1, n->instrument);
        }
        after = *n;
    }
    if (hex_nibble < 0) {
        UndoOp op;
        op.label = "instr";
        op.undo = [p, r, c, before](Module& mod) {
            mod.patterns[size_t(p)][size_t(r)][size_t(c)] = before;
        };
        op.redo = [p, r, c, after](Module& mod) {
            mod.patterns[size_t(p)][size_t(r)][size_t(c)] = after;
        };
        push_undo(std::move(op));
        advance_step(m);
    } else {
        mark_dirty();
    }
}

void EditorState::set_effect_digit(Module& m, std::mutex& mutex, int digit) {
    digit &= 0xF;
    const int p = pat, r = row, c = ch;
    Note before, after;
    {
        std::lock_guard lock(mutex);
        ensure_pattern(m, pat);
        Note* n = note_at(m);
        if (!n) {
            return;
        }
        before = *n;
        n->effect = digit;
        after = *n;
    }
    UndoOp op;
    op.label = "fx";
    op.undo = [p, r, c, before](Module& mod) {
        mod.patterns[size_t(p)][size_t(r)][size_t(c)] = before;
    };
    op.redo = [p, r, c, after](Module& mod) {
        mod.patterns[size_t(p)][size_t(r)][size_t(c)] = after;
    };
    push_undo(std::move(op));
    field = CellField::Param;
    hex_nibble = -1;
}

void EditorState::set_param_digit(Module& m, std::mutex& mutex, int digit) {
    digit &= 0xF;
    const int p = pat, r = row, c = ch;
    Note before, after;
    {
        std::lock_guard lock(mutex);
        ensure_pattern(m, pat);
        Note* n = note_at(m);
        if (!n) {
            return;
        }
        before = *n;
        if (hex_nibble < 0) {
            hex_nibble = digit;
            n->param = (digit << 4);
        } else {
            n->param = ((hex_nibble & 0xF) << 4) | digit;
            hex_nibble = -1;
        }
        after = *n;
    }
    if (hex_nibble < 0) {
        UndoOp op;
        op.label = "param";
        op.undo = [p, r, c, before](Module& mod) {
            mod.patterns[size_t(p)][size_t(r)][size_t(c)] = before;
        };
        op.redo = [p, r, c, after](Module& mod) {
            mod.patterns[size_t(p)][size_t(r)][size_t(c)] = after;
        };
        push_undo(std::move(op));
        advance_step(m);
        field = CellField::Note;
    } else {
        mark_dirty();
    }
}

void EditorState::clear_cell(Module& m, std::mutex& mutex) {
    const int p = pat, r = row, c = ch;
    Note before, after;
    {
        std::lock_guard lock(mutex);
        ensure_pattern(m, pat);
        Note* n = note_at(m);
        if (!n) {
            return;
        }
        before = *n;
        *n = Note{};
        after = *n;
    }
    UndoOp op;
    op.label = "clear";
    op.undo = [p, r, c, before](Module& mod) {
        mod.patterns[size_t(p)][size_t(r)][size_t(c)] = before;
    };
    op.redo = [p, r, c, after](Module& mod) {
        mod.patterns[size_t(p)][size_t(r)][size_t(c)] = after;
    };
    push_undo(std::move(op));
    advance_step(m);
}

void EditorState::advance_step(Module& m) {
    const int rows =
        (pat >= 0 && pat < m.pattern_count()) ? int(m.patterns[size_t(pat)].size()) : kRows;
    row = std::min(rows - 1, row + std::max(1, step));
    hex_nibble = -1;
}

void EditorState::begin_sel() {
    has_sel = true;
    sel_row0 = sel_row1 = row;
    sel_ch0 = sel_ch1 = ch;
}

void EditorState::update_sel_to_cursor() {
    if (!has_sel) {
        begin_sel();
    }
    sel_row1 = row;
    sel_ch1 = ch;
}

static void normalize_sel(int& r0, int& r1, int& c0, int& c1) {
    if (r1 < r0) {
        std::swap(r0, r1);
    }
    if (c1 < c0) {
        std::swap(c0, c1);
    }
}

void EditorState::copy_block(const Module& m) {
    if (!has_sel) {
        begin_sel();
    }
    int r0 = sel_row0, r1 = sel_row1, c0 = sel_ch0, c1 = sel_ch1;
    normalize_sel(r0, r1, c0, c1);
    clipboard = {};
    clipboard.row0 = r0;
    clipboard.row1 = r1;
    clipboard.ch0 = c0;
    clipboard.ch1 = c1;
    if (pat < 0 || pat >= m.pattern_count()) {
        return;
    }
    const auto& p = m.patterns[size_t(pat)];
    clipboard.cells.assign(size_t(r1 - r0 + 1), {});
    for (int r = r0; r <= r1; ++r) {
        clipboard.cells[size_t(r - r0)].assign(size_t(c1 - c0 + 1), Note{});
        if (r < 0 || r >= int(p.size())) {
            continue;
        }
        for (int c = c0; c <= c1; ++c) {
            if (c >= 0 && c < int(p[size_t(r)].size())) {
                clipboard.cells[size_t(r - r0)][size_t(c - c0)] = p[size_t(r)][size_t(c)];
            }
        }
    }
}

void EditorState::cut_block(Module& m, std::mutex& mutex) {
    copy_block(m);
    clear_block(m, mutex);
}

void EditorState::paste_block(Module& m, std::mutex& mutex) {
    if (clipboard.cells.empty()) {
        return;
    }
    const int pidx = pat;
    const int base_r = row;
    const int base_c = ch;
    const auto clip = clipboard;

    std::vector<std::tuple<int, int, Note, Note>> changes;
    {
        std::lock_guard lock(mutex);
        ensure_pattern(m, pat);
        auto& p = m.patterns[size_t(pidx)];
        for (int dr = 0; dr < int(clip.cells.size()); ++dr) {
            const int r = base_r + dr;
            if (r < 0 || r >= int(p.size())) {
                continue;
            }
            for (int dc = 0; dc < int(clip.cells[size_t(dr)].size()); ++dc) {
                const int c = base_c + dc;
                if (c < 0 || c >= int(p[size_t(r)].size())) {
                    continue;
                }
                Note before = p[size_t(r)][size_t(c)];
                Note after = clip.cells[size_t(dr)][size_t(dc)];
                p[size_t(r)][size_t(c)] = after;
                changes.emplace_back(r, c, before, after);
            }
        }
    }
    if (changes.empty()) {
        return;
    }
    UndoOp op;
    op.label = "paste";
    op.undo = [pidx, changes](Module& mod) {
        auto& p = mod.patterns[size_t(pidx)];
        for (const auto& [r, c, before, after] : changes) {
            (void)after;
            p[size_t(r)][size_t(c)] = before;
        }
    };
    op.redo = [pidx, changes](Module& mod) {
        auto& p = mod.patterns[size_t(pidx)];
        for (const auto& [r, c, before, after] : changes) {
            (void)before;
            p[size_t(r)][size_t(c)] = after;
        }
    };
    push_undo(std::move(op));
}

void EditorState::clear_block(Module& m, std::mutex& mutex) {
    if (!has_sel) {
        clear_cell(m, mutex);
        return;
    }
    int r0 = sel_row0, r1 = sel_row1, c0 = sel_ch0, c1 = sel_ch1;
    normalize_sel(r0, r1, c0, c1);
    const int pidx = pat;
    std::vector<std::tuple<int, int, Note>> before;
    {
        std::lock_guard lock(mutex);
        ensure_pattern(m, pat);
        auto& p = m.patterns[size_t(pidx)];
        for (int r = r0; r <= r1; ++r) {
            if (r < 0 || r >= int(p.size())) {
                continue;
            }
            for (int c = c0; c <= c1; ++c) {
                if (c < 0 || c >= int(p[size_t(r)].size())) {
                    continue;
                }
                before.emplace_back(r, c, p[size_t(r)][size_t(c)]);
                p[size_t(r)][size_t(c)] = Note{};
            }
        }
    }
    UndoOp op;
    op.label = "clear block";
    op.undo = [pidx, before](Module& mod) {
        auto& p = mod.patterns[size_t(pidx)];
        for (const auto& [r, c, n] : before) {
            p[size_t(r)][size_t(c)] = n;
        }
    };
    op.redo = [pidx, before](Module& mod) {
        auto& p = mod.patterns[size_t(pidx)];
        for (const auto& [r, c, n] : before) {
            (void)n;
            p[size_t(r)][size_t(c)] = Note{};
        }
    };
    push_undo(std::move(op));
}

void EditorState::insert_order(Module& m, std::mutex& mutex, int at) {
    int before_len = 0;
    std::vector<int> before_orders;
    {
        std::lock_guard lock(mutex);
        before_len = m.song_length;
        before_orders = m.orders;
        at = std::clamp(at, 0, m.song_length);
        if (m.song_length >= 128) {
            return;
        }
        for (int i = 127; i > at; --i) {
            m.orders[size_t(i)] = m.orders[size_t(i - 1)];
        }
        m.orders[size_t(at)] = (at > 0) ? m.orders[size_t(at - 1)] : 0;
        ++m.song_length;
    }
    const int after_len = before_len + 1;
    std::vector<int> after_orders;
    {
        std::lock_guard lock(mutex);
        after_orders = m.orders;
    }
    UndoOp op;
    op.label = "ins order";
    op.undo = [before_len, before_orders](Module& mod) {
        mod.song_length = before_len;
        mod.orders = before_orders;
    };
    op.redo = [after_len, after_orders](Module& mod) {
        mod.song_length = after_len;
        mod.orders = after_orders;
    };
    push_undo(std::move(op));
}

void EditorState::delete_order(Module& m, std::mutex& mutex, int at) {
    int before_len = 0;
    std::vector<int> before_orders;
    {
        std::lock_guard lock(mutex);
        if (m.song_length <= 1 || at < 0 || at >= m.song_length) {
            return;
        }
        before_len = m.song_length;
        before_orders = m.orders;
        for (int i = at; i < 127; ++i) {
            m.orders[size_t(i)] = m.orders[size_t(i + 1)];
        }
        --m.song_length;
    }
    int after_len = 0;
    std::vector<int> after_orders;
    {
        std::lock_guard lock(mutex);
        after_len = m.song_length;
        after_orders = m.orders;
    }
    UndoOp op;
    op.label = "del order";
    op.undo = [before_len, before_orders](Module& mod) {
        mod.song_length = before_len;
        mod.orders = before_orders;
    };
    op.redo = [after_len, after_orders](Module& mod) {
        mod.song_length = after_len;
        mod.orders = after_orders;
    };
    push_undo(std::move(op));
}

void EditorState::set_order_pattern(Module& m, std::mutex& mutex, int at, int pat_index) {
    int before = 0, after = 0;
    {
        std::lock_guard lock(mutex);
        if (at < 0 || at >= m.song_length) {
            return;
        }
        before = m.orders[size_t(at)];
        after = std::max(0, pat_index);
        ensure_pattern(m, after);
        m.orders[size_t(at)] = after;
    }
    UndoOp op;
    op.label = "order pat";
    op.undo = [at, before](Module& mod) { mod.orders[size_t(at)] = before; };
    op.redo = [at, after](Module& mod) { mod.orders[size_t(at)] = after; };
    push_undo(std::move(op));
}

int EditorState::add_pattern(Module& m, std::mutex& mutex) {
    int idx = 0;
    int chn = 4;
    int rows = kRows;
    {
        std::lock_guard lock(mutex);
        idx = m.pattern_count();
        chn = m.channels;
        if (pat >= 0 && pat < m.pattern_count()) {
            rows = std::max(1, int(m.patterns[size_t(pat)].size()));
        }
        m.patterns.push_back(
            std::vector<std::vector<Note>>(size_t(rows), std::vector<Note>(size_t(chn))));
    }
    UndoOp op;
    op.label = "add pat";
    op.undo = [idx](Module& mod) {
        if (idx < mod.pattern_count()) {
            mod.patterns.resize(size_t(idx));
        }
    };
    op.redo = [idx, chn, rows](Module& mod) {
        while (mod.pattern_count() <= idx) {
            mod.patterns.push_back(
                std::vector<std::vector<Note>>(size_t(rows), std::vector<Note>(size_t(chn))));
        }
    };
    push_undo(std::move(op));
    return idx;
}

void EditorState::set_pattern_length(Module& m, std::mutex& mutex, int pat_index, int rows) {
    rows = std::clamp(rows, 1, 256);
    std::vector<std::vector<Note>> before;
    std::vector<std::vector<Note>> after;
    {
        std::lock_guard lock(mutex);
        ensure_pattern(m, pat_index);
        if (pat_index < 0 || pat_index >= m.pattern_count()) {
            return;
        }
        auto& p = m.patterns[size_t(pat_index)];
        before = p;
        const int chn = std::max(1, m.channels);
        if (int(p.size()) < rows) {
            p.resize(size_t(rows), std::vector<Note>(size_t(chn)));
        } else if (int(p.size()) > rows) {
            p.resize(size_t(rows));
        }
        for (auto& r : p) {
            r.resize(size_t(chn));
        }
        after = p;
        if (pat == pat_index) {
            row = std::clamp(row, 0, rows - 1);
            if (has_sel) {
                sel_row0 = std::clamp(sel_row0, 0, rows - 1);
                sel_row1 = std::clamp(sel_row1, 0, rows - 1);
            }
        }
    }
    if (before.size() == after.size()) {
        return;
    }
    UndoOp op;
    op.label = "pat len";
    op.undo = [pat_index, before](Module& mod) {
        if (pat_index < mod.pattern_count()) {
            mod.patterns[size_t(pat_index)] = before;
        }
    };
    op.redo = [pat_index, after](Module& mod) {
        if (pat_index < mod.pattern_count()) {
            mod.patterns[size_t(pat_index)] = after;
        }
    };
    push_undo(std::move(op));
}

void EditorState::insert_rows(Module& m, std::mutex& mutex, int pat_index, int at, int count) {
    count = std::max(1, count);
    std::vector<std::vector<Note>> before;
    std::vector<std::vector<Note>> after;
    {
        std::lock_guard lock(mutex);
        ensure_pattern(m, pat_index);
        if (pat_index < 0 || pat_index >= m.pattern_count()) {
            return;
        }
        auto& p = m.patterns[size_t(pat_index)];
        before = p;
        at = std::clamp(at, 0, int(p.size()));
        if (int(p.size()) + count > 256) {
            count = 256 - int(p.size());
        }
        if (count <= 0) {
            return;
        }
        const int chn = std::max(1, m.channels);
        p.insert(p.begin() + at, size_t(count), std::vector<Note>(size_t(chn)));
        after = p;
        if (pat == pat_index && row >= at) {
            row = std::min(255, row + count);
        }
    }
    UndoOp op;
    op.label = "ins rows";
    op.undo = [pat_index, before](Module& mod) {
        if (pat_index < mod.pattern_count()) {
            mod.patterns[size_t(pat_index)] = before;
        }
    };
    op.redo = [pat_index, after](Module& mod) {
        if (pat_index < mod.pattern_count()) {
            mod.patterns[size_t(pat_index)] = after;
        }
    };
    push_undo(std::move(op));
}

void EditorState::delete_rows(Module& m, std::mutex& mutex, int pat_index, int at, int count) {
    count = std::max(1, count);
    std::vector<std::vector<Note>> before;
    std::vector<std::vector<Note>> after;
    {
        std::lock_guard lock(mutex);
        ensure_pattern(m, pat_index);
        if (pat_index < 0 || pat_index >= m.pattern_count()) {
            return;
        }
        auto& p = m.patterns[size_t(pat_index)];
        if (p.size() <= 1 || at < 0 || at >= int(p.size())) {
            return;
        }
        before = p;
        count = std::min(count, int(p.size()) - at);
        if (count >= int(p.size())) {
            count = int(p.size()) - 1;  // keep at least one row
        }
        if (count <= 0) {
            return;
        }
        p.erase(p.begin() + at, p.begin() + at + count);
        after = p;
        if (pat == pat_index) {
            row = std::clamp(row, 0, std::max(0, int(p.size()) - 1));
            if (has_sel) {
                sel_row0 = std::clamp(sel_row0, 0, int(p.size()) - 1);
                sel_row1 = std::clamp(sel_row1, 0, int(p.size()) - 1);
            }
        }
    }
    UndoOp op;
    op.label = "del rows";
    op.undo = [pat_index, before](Module& mod) {
        if (pat_index < mod.pattern_count()) {
            mod.patterns[size_t(pat_index)] = before;
        }
    };
    op.redo = [pat_index, after](Module& mod) {
        if (pat_index < mod.pattern_count()) {
            mod.patterns[size_t(pat_index)] = after;
        }
    };
    push_undo(std::move(op));
}

void EditorState::replace_sample(Module& m, std::mutex& mutex, int slot, Sample next) {
    if (slot < 0 || slot >= 31) {
        return;
    }
    Sample before;
    {
        std::lock_guard lock(mutex);
        if (int(m.samples.size()) < 31) {
            m.samples.resize(31);
        }
        before = m.samples[size_t(slot)];
        clamp_sample_pt(next);
        m.samples[size_t(slot)] = std::move(next);
    }
    Sample after;
    {
        std::lock_guard lock(mutex);
        after = m.samples[size_t(slot)];
    }
    UndoOp op;
    op.label = "sample";
    op.undo = [slot, before](Module& mod) { mod.samples[size_t(slot)] = before; };
    op.redo = [slot, after](Module& mod) { mod.samples[size_t(slot)] = after; };
    push_undo(std::move(op));
    sample_sel.start = 0;
    sample_sel.end = int(after.wave.size());
}

Sample& EditorState::current_sample(Module& m) {
    if (int(m.samples.size()) < 31) {
        m.samples.resize(31);
    }
    sample_slot = std::clamp(sample_slot, 0, 30);
    return m.samples[size_t(sample_slot)];
}

void EditorState::with_sample_edit(Module& m, std::mutex& mutex, const char* label,
                                   const std::function<void(Sample&, SampleSel&)>& fn) {
    Sample before;
    SampleSel sel_before = sample_sel;
    {
        std::lock_guard lock(mutex);
        before = current_sample(m);
        Sample& s = current_sample(m);
        fn(s, sample_sel);
        clamp_sample_pt(s);
    }
    Sample after;
    SampleSel sel_after = sample_sel;
    {
        std::lock_guard lock(mutex);
        after = current_sample(m);
    }
    const int slot = sample_slot;
    UndoOp op;
    op.label = label ? label : "sample edit";
    op.undo = [slot, before, sel_before, this](Module& mod) {
        mod.samples[size_t(slot)] = before;
        sample_sel = sel_before;
    };
    op.redo = [slot, after, sel_after, this](Module& mod) {
        mod.samples[size_t(slot)] = after;
        sample_sel = sel_after;
    };
    push_undo(std::move(op));
}

bool EditorState::handle_note_key(Module& m, std::mutex& mutex, int keycode, bool /*shift*/,
                                  int* period_out) {
    if (view == EditorView::Sample) {
        const int period = note_key_to_period(keycode, octave);
        if (period && period_out) {
            *period_out = period;
            return true;
        }
        return false;
    }
    if (view != EditorView::Pattern || !edit_mode) {
        return false;
    }

    // Letter piano keys always enter notes. Number-row sharps (2/3/5/6/7) only in Note field
    // so hex entry still works on instr/fx/param.
    const bool num_key = keycode >= '0' && keycode <= '9';
    if (!num_key || field == CellField::Note) {
        const int period = note_key_to_period(std::tolower(keycode), octave);
        if (period) {
            set_period(m, mutex, period, true);
            if (period_out) {
                *period_out = period;
            }
            return true;
        }
    }

    int digit = -1;
    if (keycode >= '0' && keycode <= '9') {
        digit = keycode - '0';
    } else if (keycode >= 'a' && keycode <= 'f') {
        digit = 10 + (keycode - 'a');
    } else if (keycode >= 'A' && keycode <= 'F') {
        digit = 10 + (keycode - 'A');
    }
    if (digit >= 0) {
        if (field == CellField::Instr) {
            set_instrument_digit(m, mutex, digit);
            return true;
        }
        if (field == CellField::Effect) {
            set_effect_digit(m, mutex, digit);
            return true;
        }
        if (field == CellField::Param) {
            set_param_digit(m, mutex, digit);
            return true;
        }
    }
    return false;
}

bool EditorState::handle_nav_key(Module& m, int keycode, bool shift) {
    const int rows =
        (pat >= 0 && pat < m.pattern_count()) ? int(m.patterns[size_t(pat)].size()) : kRows;
    const int chn = m.channels;

    auto move = [&](int dr, int dc) {
        if (shift) {
            if (!has_sel) {
                begin_sel();
            }
        } else {
            has_sel = false;
            hex_nibble = -1;
        }
        row = std::clamp(row + dr, 0, std::max(0, rows - 1));
        ch = std::clamp(ch + dc, 0, std::max(0, chn - 1));
        if (shift) {
            update_sel_to_cursor();
        }
    };

    switch (keycode) {
    case 0x4000004F:  // SDLK_RIGHT — use numeric to avoid SDL include here; main uses SDL keys
    case 0x40000050:  // LEFT
    case 0x40000051:  // DOWN
    case 0x40000052:  // UP
        break;
    default:
        break;
    }

    // Use character / known codes from caller via SDL mapped in main — also accept letters:
    if (keycode == 273 || keycode == 0x40000052) {  // UP
        move(-1, 0);
        return true;
    }
    if (keycode == 274 || keycode == 0x40000051) {  // DOWN
        move(1, 0);
        return true;
    }
    if (keycode == 276 || keycode == 0x40000050) {  // LEFT
        if (field == CellField::Note) {
            move(0, -1);
            field = CellField::Param;
        } else {
            field = CellField(int(field) - 1);
            hex_nibble = -1;
        }
        if (shift) {
            update_sel_to_cursor();
        }
        return true;
    }
    if (keycode == 275 || keycode == 0x4000004F) {  // RIGHT
        if (field == CellField::Param) {
            move(0, 1);
            field = CellField::Note;
        } else {
            field = CellField(int(field) + 1);
            hex_nibble = -1;
        }
        if (shift) {
            update_sel_to_cursor();
        }
        return true;
    }
    if (keycode == '\t') {
        ch = (ch + 1) % std::max(1, chn);
        field = CellField::Note;
        hex_nibble = -1;
        return true;
    }
    return false;
}

}  // namespace mod
