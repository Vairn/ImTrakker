#pragma once

#include "mod/module.hpp"
#include "mod/sample_edit.hpp"
#include "mod/sample_io.hpp"

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace mod {

enum class CellField { Note = 0, Instr, Effect, Param };

enum class EditorView { Pattern, Sample };

struct PatternBlock {
    int row0 = 0, row1 = 0, ch0 = 0, ch1 = 0;
    std::vector<std::vector<Note>> cells;  // [row][ch]
};

struct UndoOp {
    std::string label;
    std::function<void(Module&)> undo;
    std::function<void(Module&)> redo;
};

struct EditorState {
    EditorView view = EditorView::Pattern;
    bool edit_mode = true;
    bool dirty = false;

    // Pattern cursor
    int pat = 0;
    int row = 0;
    int ch = 0;
    CellField field = CellField::Note;
    int octave = 2;       // 1..3 maps to C-1..B-3 table
    int instrument = 1;   // 1..31
    int step = 1;
    int hex_nibble = -1;  // staged high nibble for instr/fx/param (-1 = none)

    // Pattern selection (inclusive)
    bool has_sel = false;
    int sel_row0 = 0, sel_row1 = 0, sel_ch0 = 0, sel_ch1 = 0;
    PatternBlock clipboard;

    // Sample editor
    int sample_slot = 0;  // 0..30
    SampleSel sample_sel{};
    SampleClipboard sample_clip;
    float wave_zoom = 1.f;
    float wave_scroll = 0.f;  // 0..1
    float amplify_gain = 1.5f;
    bool raw_unsigned = false;
    int steal_instr = 1;

    // Title edit buffer (UI)
    char title_buf[21]{};

    std::deque<UndoOp> undo_stack;
    std::deque<UndoOp> redo_stack;
    static constexpr size_t kMaxUndo = 64;

    void reset_for_module(const Module& m);
    void mark_dirty() { dirty = true; }
    void clear_dirty() { dirty = false; }

    void push_undo(UndoOp op);
    bool undo(Module& m, std::mutex& mutex);
    bool redo(Module& m, std::mutex& mutex);

    Note* note_at(Module& m);
    const Note* note_at(const Module& m) const;

    void ensure_pattern(Module& m, int p);
    void set_period(Module& m, std::mutex& mutex, int period, bool apply_instr);
    void set_instrument_digit(Module& m, std::mutex& mutex, int digit);
    void set_effect_digit(Module& m, std::mutex& mutex, int digit);
    void set_param_digit(Module& m, std::mutex& mutex, int digit);
    void clear_cell(Module& m, std::mutex& mutex);
    void advance_step(Module& m);

    void begin_sel();
    void update_sel_to_cursor();
    void copy_block(const Module& m);
    void cut_block(Module& m, std::mutex& mutex);
    void paste_block(Module& m, std::mutex& mutex);
    void clear_block(Module& m, std::mutex& mutex);

    void insert_order(Module& m, std::mutex& mutex, int at);
    void delete_order(Module& m, std::mutex& mutex, int at);
    void set_order_pattern(Module& m, std::mutex& mutex, int at, int pat_index);
    int add_pattern(Module& m, std::mutex& mutex);

    void replace_sample(Module& m, std::mutex& mutex, int slot, Sample next);
    Sample& current_sample(Module& m);
    void with_sample_edit(Module& m, std::mutex& mutex, const char* label,
                         const std::function<void(Sample&, SampleSel&)>& fn);

    // Returns true if key was consumed as note entry. period_out set if note played.
    bool handle_note_key(Module& m, std::mutex& mutex, int keycode, bool shift, int* period_out);
    bool handle_nav_key(Module& m, int keycode, bool shift);
};

int note_key_to_period(int keycode, int octave);

}  // namespace mod
