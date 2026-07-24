#include "mod/module.hpp"
#include "mod/player.hpp"
#include "mod/editor.hpp"
#include "mod/sample_io.hpp"
#include "mod/sample_edit.hpp"
#include "smus/smus.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "portable-file-dialogs.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static fs::path find_data_dir() {
    if (const char* env = std::getenv("IMTRAKKER_DATA")) {
        if (*env) {
            return env;
        }
    }
    const char* candidates[] = {
        IMTRAKKER_DEFAULT_DATA_DIR,
        "data",
        "../data",
    };
    for (const char* c : candidates) {
        const fs::path p = c;
        if (fs::is_directory(p)) {
            return p;
        }
    }
    return IMTRAKKER_DEFAULT_DATA_DIR;
}

static fs::path resolve_song(const fs::path& data, const std::string& arg) {
    fs::path p = arg;
    if (fs::is_regular_file(p)) {
        return p;
    }
    p = data / arg;
    if (fs::is_regular_file(p)) {
        return p;
    }
    if (arg.find('.') == std::string::npos) {
        for (const char* ext : {".mod", ".hsq", ".mmd0", ".mmd1", ".smus"}) {
            p = data / (arg + ext);
            if (fs::is_regular_file(p)) {
                return p;
            }
        }
    }
    return fs::path(arg);
}

static void apply_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 4.f;
    s.FrameRounding = 3.f;
    s.GrabRounding = 3.f;
    s.ScrollbarRounding = 3.f;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.05f, 0.04f, 1.f);
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.07f, 0.05f, 1.f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.11f, 0.07f, 1.f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.28f, 0.18f, 0.10f, 1.f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.45f, 0.28f, 0.12f, 1.f);
    c[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.08f, 0.05f, 1.f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.55f, 0.30f, 0.10f, 1.f);
    c[ImGuiCol_Button] = ImVec4(0.45f, 0.25f, 0.10f, 1.f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.78f, 0.42f, 0.12f, 1.f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.92f, 0.55f, 0.18f, 1.f);
    c[ImGuiCol_Header] = ImVec4(0.45f, 0.25f, 0.10f, 1.f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.70f, 0.38f, 0.12f, 1.f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.85f, 0.48f, 0.14f, 1.f);
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.84f, 0.66f, 1.f);
    c[ImGuiCol_Border] = ImVec4(0.55f, 0.35f, 0.15f, 0.6f);
    c[ImGuiCol_PlotLines] = ImVec4(0.90f, 0.55f, 0.20f, 1.f);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.45f, 0.12f, 1.f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.90f, 0.50f, 0.15f, 1.f);
    c[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.70f, 0.25f, 1.f);
    c[ImGuiCol_Tab] = ImVec4(0.22f, 0.14f, 0.08f, 1.f);
    c[ImGuiCol_TabHovered] = ImVec4(0.78f, 0.42f, 0.12f, 1.f);
    c[ImGuiCol_TabActive] = ImVec4(0.78f, 0.42f, 0.12f, 1.f);
}

struct ViewOpts {
    bool show_scopes = true;
    bool show_vu = true;
    bool show_spectrum = true;
    bool show_orders = true;
    float meter_height = 56.f;
    float spectrum_height = 44.f;
    float render_seconds = 30.f;
    char render_path[512]{};
    char note_path[512]{};
    std::string render_status;
};

struct App {
    fs::path data_dir;
    std::unique_ptr<mod::Player> player;
    std::unique_ptr<smus::Engine> smus;
    mod::EditorState editor;
    std::string status = "Open a module or New to compose";
    char open_path[512]{};
    int zoom_rows = 14;
    float flash = 0.f;
    bool show_options = false;
    bool show_help = false;
    ViewOpts view;
    std::shared_ptr<pfd::open_file> pending_open;
    std::shared_ptr<pfd::save_file> pending_save;
    std::shared_ptr<pfd::open_file> pending_sample_open;
    std::shared_ptr<pfd::save_file> pending_sample_save;
    std::string pending_load;
    std::string pending_save_path;
    std::string pending_sample_load;
    std::string pending_sample_save_path;
    int pending_sample_save_fmt = 0;  // 0 wav, 1 8svx, 2 raw
    int pending_steal_instr = 1;
    bool pending_steal_mod = false;
    char sample_name_buf[23]{};

    bool has_song() const { return player || smus; }

    bool load_path(const fs::path& path) {
        try {
            if (smus::is_smus_file(path)) {
                player.reset();
                smus = smus::Engine::load(path);
                status = path.string() + "  (SMUS / Sonix, " +
                         std::to_string(smus->score().tracks.size()) + " tracks, " +
                         std::to_string(int(smus->bpm())) + " BPM)";
            } else {
                smus.reset();
                auto module = mod::load_module(path);
                if (!player) {
                    player = std::make_unique<mod::Player>(std::move(module));
                } else {
                    player->load(std::move(module));
                }
                editor.reset_for_module(player->module());
                {
                    std::lock_guard lock(player->mutex());
                    if (player->module().samples.size() < 31) {
                        player->module().samples.resize(31);
                    }
                }
                sync_sample_name_buf();
                status = path.string() + "  (" + player->module().magic + ", " +
                         std::to_string(player->module().channels) + "ch)";
            }
            std::strncpy(open_path, path.string().c_str(), sizeof(open_path) - 1);
            open_path[sizeof(open_path) - 1] = '\0';
            flash = 0.5f;
            return true;
        } catch (const std::exception& e) {
            status = std::string("load failed: ") + e.what();
            return false;
        }
    }

    void sync_sample_name_buf() {
        std::memset(sample_name_buf, 0, sizeof(sample_name_buf));
        if (!player) {
            return;
        }
        const auto& s = player->module().samples[size_t(std::clamp(editor.sample_slot, 0, 30))];
        std::strncpy(sample_name_buf, s.name.c_str(), sizeof(sample_name_buf) - 1);
    }

    void new_song(int channels = 4) {
        smus.reset();
        auto module = mod::make_blank(channels);
        if (!player) {
            player = std::make_unique<mod::Player>(std::move(module));
        } else {
            player->load(std::move(module));
        }
        editor.reset_for_module(player->module());
        editor.mark_dirty();
        sync_sample_name_buf();
        std::strncpy(open_path, "untitled.mod", sizeof(open_path) - 1);
        open_path[sizeof(open_path) - 1] = '\0';
        status = "New song (4ch M.K.) — edit and Save As…";
        flash = 0.5f;
    }

    bool save_to(const fs::path& path) {
        if (!player) {
            return false;
        }
        try {
            {
                std::lock_guard lock(player->mutex());
                player->module().title = editor.title_buf;
                player->module().path = path;
                mod::save_protracker(player->module(), path);
            }
            editor.clear_dirty();
            std::strncpy(open_path, path.string().c_str(), sizeof(open_path) - 1);
            open_path[sizeof(open_path) - 1] = '\0';
            status = "saved " + path.string();
            return true;
        } catch (const std::exception& e) {
            status = std::string("save failed: ") + e.what();
            return false;
        }
    }

    void begin_browse() {
        if (pending_open) {
            return;
        }
        const std::string start = open_path[0] ? open_path : ".";
        pending_open = std::make_shared<pfd::open_file>(
            "Open module", start,
            std::vector<std::string>{
                "Modules", "*.mod *.hsq *.mmd0 *.mmd1 *.mmd2 *.mmd3 *.smus",
                "All files", "*.*"});
    }

    void begin_save_as() {
        if (pending_save || !player) {
            return;
        }
        const std::string start = open_path[0] ? open_path : "untitled.mod";
        pending_save = std::make_shared<pfd::save_file>(
            "Save module", start,
            std::vector<std::string>{"ProTracker module", "*.mod", "All files", "*.*"});
    }

    void begin_sample_load(bool from_mod) {
        if (pending_sample_open || !player) {
            return;
        }
        pending_steal_mod = from_mod;
        const std::string start = open_path[0] ? open_path : ".";
        if (from_mod) {
            pending_sample_open = std::make_shared<pfd::open_file>(
                "Load sample from module", start,
                std::vector<std::string>{"Modules", "*.mod *.mmd0 *.mmd1 *.mmd2 *.mmd3", "All files",
                                         "*.*"});
        } else {
            pending_sample_open = std::make_shared<pfd::open_file>(
                "Load sample", start,
                std::vector<std::string>{
                    "Samples", "*.wav *.iff *.8svx *.raw *.smp", "WAV", "*.wav", "IFF 8SVX",
                    "*.iff *.8svx", "RAW", "*.raw *.smp", "All files", "*.*"});
        }
    }

    void begin_sample_save(int fmt) {
        if (pending_sample_save || !player) {
            return;
        }
        pending_sample_save_fmt = fmt;
        const char* def = fmt == 1 ? "sample.iff" : (fmt == 2 ? "sample.raw" : "sample.wav");
        pending_sample_save = std::make_shared<pfd::save_file>(
            "Save sample", def,
            std::vector<std::string>{"Sample", "*.wav *.iff *.raw", "All files", "*.*"});
    }

    void request_open_typed() {
        if (open_path[0]) {
            pending_load = resolve_song(data_dir, open_path).string();
        }
    }
};

struct AudioBridge {
    mod::Player* player = nullptr;
    smus::Engine* smus = nullptr;
};

static void SDLCALL audio_callback(void* userdata, Uint8* stream, int len) {
    auto* bridge = static_cast<AudioBridge*>(userdata);
    const int n_frames = len / int(sizeof(float) * 2);
    auto* out = reinterpret_cast<float*>(stream);
    if (bridge->smus) {
        bridge->smus->render(out, n_frames);
    } else if (bridge->player) {
        bridge->player->render(out, n_frames);
    } else {
        std::memset(out, 0, size_t(len));
    }
}

static void write_wav(const fs::path& out, const std::vector<float>& pcm, int sr) {
    std::vector<int16_t> i16(pcm.size());
    float peak = 0.f;
    for (size_t i = 0; i < pcm.size(); ++i) {
        peak = std::max(peak, std::fabs(pcm[i]));
        i16[i] = int16_t(std::clamp(pcm[i], -1.f, 1.f) * 32767.f);
    }
    std::ofstream f(out, std::ios::binary);
    const int data_bytes = int(i16.size() * sizeof(int16_t));
    const int byte_rate = sr * 2 * 2;
    f.write("RIFF", 4);
    const uint32_t chunk = 36 + uint32_t(data_bytes);
    f.write(reinterpret_cast<const char*>(&chunk), 4);
    f.write("WAVEfmt ", 8);
    const uint32_t fmt_size = 16;
    const uint16_t audio_fmt = 1;
    const uint16_t wav_channels = 2;
    const uint16_t bps = 16;
    const uint16_t block_align = 4;
    f.write(reinterpret_cast<const char*>(&fmt_size), 4);
    f.write(reinterpret_cast<const char*>(&audio_fmt), 2);
    f.write(reinterpret_cast<const char*>(&wav_channels), 2);
    f.write(reinterpret_cast<const char*>(&sr), 4);
    f.write(reinterpret_cast<const char*>(&byte_rate), 4);
    f.write(reinterpret_cast<const char*>(&block_align), 2);
    f.write(reinterpret_cast<const char*>(&bps), 2);
    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&data_bytes), 4);
    f.write(reinterpret_cast<const char*>(i16.data()), data_bytes);
    std::printf("wrote %s (%.1fs, peak=%.3f)\n", out.string().c_str(),
                float(pcm.size() / 2) / float(sr), peak);
}

static void dump_wav_mod(mod::Player& player, float seconds, const fs::path& out) {
    const int n = int(mod::kSampleRate * seconds);
    std::vector<float> pcm(size_t(n) * 2);
    player.set_playing(true);
    player.render(pcm.data(), n);
    write_wav(out, pcm, mod::kSampleRate);
}

static void dump_wav_smus(smus::Engine& eng, float seconds, const fs::path& out) {
    const int n = int(eng.sample_rate() * seconds);
    std::vector<float> pcm(size_t(n) * 2);
    eng.set_playing(true);
    eng.render(pcm.data(), n);
    write_wav(out, pcm, eng.sample_rate());
}

static void dump_notes_mod(const mod::Module& mod, const fs::path& out) {
    std::ofstream f(out);
    if (!f) {
        throw std::runtime_error("cannot write: " + out.string());
    }
    f << "ImTrakker note dump\n";
    f << "Title: " << mod.title << "\n";
    f << "Format: " << mod.magic << "  Channels: " << mod.channels << "\n";
    f << "Speed: " << mod.initial_speed << "  Tempo: " << mod.initial_tempo << "\n";
    f << "Length: " << mod.song_length << "  Restart: " << mod.restart << "\n";
    f << "Orders:";
    for (int i = 0; i < mod.song_length && i < int(mod.orders.size()); ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), " %02d", mod.orders[size_t(i)]);
        f << buf;
    }
    f << "\n\n";

    f << "Samples:\n";
    for (size_t i = 0; i < mod.samples.size(); ++i) {
        const auto& s = mod.samples[i];
        if (s.length_words <= 0 && s.name.empty()) {
            continue;
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "  %02d  ", int(i + 1));
        f << buf << s.name << "  len=" << (s.length_words * 2) << "  vol=" << s.volume
          << "  ft=" << s.finetune << "\n";
    }
    f << "\n";

    for (int p = 0; p < mod.pattern_count(); ++p) {
        const auto& pat = mod.patterns[size_t(p)];
        char hdr[64];
        std::snprintf(hdr, sizeof(hdr), "=== PATTERN %02d (%zu rows) ===\n", p, pat.size());
        f << hdr;
        for (size_t row = 0; row < pat.size(); ++row) {
            char rb[8];
            std::snprintf(rb, sizeof(rb), "%02d |", int(row));
            f << rb;
            for (int c = 0; c < mod.channels; ++c) {
                if (c < int(pat[row].size())) {
                    f << " " << pat[row][size_t(c)].text() << " |";
                } else {
                    f << " --- .. ... |";
                }
            }
            f << "\n";
        }
        f << "\n";
    }
}

static std::string format_smus_event(const smus::SEvent& ev);

static void dump_notes_smus(const smus::Score& score, float bpm, const fs::path& out) {
    std::ofstream f(out);
    if (!f) {
        throw std::runtime_error("cannot write: " + out.string());
    }
    f << "ImTrakker SMUS note dump\n";
    f << "Title: " << score.name << "\n";
    f << "BPM: " << int(bpm) << "  Volume: " << score.volume << "\n";
    f << "Tracks: " << score.tracks.size() << "\n\n";

    f << "Instruments:\n";
    for (const auto& [reg, name] : score.instruments) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "  %02d  ", reg);
        f << buf << name << "\n";
    }
    f << "\n";

    for (size_t t = 0; t < score.tracks.size(); ++t) {
        const auto& track = score.tracks[t];
        f << "=== TRACK " << t << " (" << track.size() << " events) ===\n";
        for (size_t i = 0; i < track.size(); ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%04d  ", int(i));
            f << buf << format_smus_event(track[i]) << "\n";
        }
        f << "\n";
    }
}

static ImVec4 ch_color(int i) {
    static const ImVec4 cols[] = {
        {1.f, 0.73f, 0.36f, 1.f},
        {0.43f, 0.82f, 0.73f, 1.f},
        {0.73f, 0.58f, 1.f, 1.f},
        {1.f, 0.50f, 0.58f, 1.f},
        {0.95f, 0.85f, 0.40f, 1.f},
        {0.55f, 0.75f, 1.f, 1.f},
        {0.90f, 0.60f, 0.90f, 1.f},
        {0.70f, 0.90f, 0.50f, 1.f},
    };
    return cols[i & 7];
}

// ProTracker-style green → yellow → red LED segment colour (t in [0,1], bottom→top).
static ImU32 pt_rainbow(float t, bool muted) {
    t = std::clamp(t, 0.f, 1.f);
    int r, g, b;
    if (t < 0.5f) {
        const float u = t / 0.5f;
        r = int(255.f * u);
        g = 220 + int(35.f * u);
        b = 0;
    } else {
        const float u = (t - 0.5f) / 0.5f;
        r = 255;
        g = int(255.f * (1.f - u));
        b = 0;
    }
    if (muted) {
        r = r * 2 / 5;
        g = g * 2 / 5;
        b = b * 2 / 5;
    }
    return IM_COL32(r, g, b, 255);
}

// Vertical ProTracker peak meter (segmented LED bars).
static void draw_vu(const char* id, float level, float hold, bool muted, float height = 56.f) {
    ImGui::PushID(id);
    const float w = 12.f;
    const float h = height;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(16, 16, 20, 255));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(48, 48, 56, 255));

    constexpr int kSegs = 16;
    const float gap = 1.f;
    const float seg_h = (h - 2.f - (kSegs - 1) * gap) / kSegs;
    const int lit = int(std::lround(std::clamp(level, 0.f, 1.f) * kSegs));
    const int hold_seg = int(std::lround(std::clamp(hold, 0.f, 1.f) * kSegs));

    for (int i = 0; i < kSegs; ++i) {
        const float t = (kSegs <= 1) ? 0.f : float(i) / float(kSegs - 1);
        const float y = p.y + h - 1.f - (i + 1) * seg_h - i * gap;
        const ImVec2 a(p.x + 1.f, y);
        const ImVec2 b(p.x + w - 1.f, y + seg_h);
        if (i < lit) {
            dl->AddRectFilled(a, b, pt_rainbow(t, muted));
        } else if (i == hold_seg - 1 && hold_seg > 0) {
            dl->AddRectFilled(a, b, pt_rainbow(t, muted));
        } else {
            dl->AddRectFilled(a, b, IM_COL32(28, 28, 34, 255));
        }
    }
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
}

static void draw_waveform(const char* id, const mod::Sample& s, mod::SampleSel& sel, float zoom,
                          float scroll, float height = 120.f) {
    ImGui::PushID(id);
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = height;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(16, 12, 10, 255));
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(80, 50, 25, 255));

    const int n = int(s.wave.size());
    if (n > 1 && w > 2.f) {
        const float view = std::clamp(1.f / std::max(0.1f, zoom), 0.02f, 1.f);
        const float start_f = std::clamp(scroll, 0.f, 1.f - view);
        const int i0 = int(start_f * float(n));
        const int i1 = std::min(n, int((start_f + view) * float(n)));
        const int span = std::max(1, i1 - i0);
        const float mid = p0.y + h * 0.5f;
        dl->AddLine(ImVec2(p0.x, mid), ImVec2(p0.x + w, mid), IM_COL32(40, 30, 20, 255));

        if (sel.active()) {
            const float xa = p0.x + float(sel.start - i0) / float(span) * w;
            const float xb = p0.x + float(sel.end - i0) / float(span) * w;
            dl->AddRectFilled(ImVec2(std::max(p0.x, xa), p0.y + 1),
                              ImVec2(std::min(p0.x + w, xb), p0.y + h - 1),
                              IM_COL32(180, 90, 30, 60));
        }

        if (s.replen_words > 1) {
            const int ls = s.repstart_words * 2;
            const int le = ls + s.replen_words * 2;
            const float xa = p0.x + float(ls - i0) / float(span) * w;
            const float xb = p0.x + float(le - i0) / float(span) * w;
            dl->AddLine(ImVec2(xa, p0.y), ImVec2(xa, p0.y + h), IM_COL32(80, 200, 120, 200));
            dl->AddLine(ImVec2(xb, p0.y), ImVec2(xb, p0.y + h), IM_COL32(80, 200, 120, 200));
        }

        const int step = std::max(1, span / int(w));
        for (int x = 0; x < int(w); ++x) {
            const int ia = i0 + int(float(x) / w * float(span));
            float mn = 1.f, mx = -1.f;
            for (int i = ia; i < ia + step && i < i1; ++i) {
                mn = std::min(mn, s.wave[size_t(i)]);
                mx = std::max(mx, s.wave[size_t(i)]);
            }
            const float y0 = mid - mx * (h * 0.45f);
            const float y1 = mid - mn * (h * 0.45f);
            const float xx = p0.x + float(x);
            dl->AddLine(ImVec2(xx, y0), ImVec2(xx, y1), IM_COL32(230, 160, 70, 220));
        }

        ImGui::InvisibleButton("##wave", ImVec2(w, h));
        if (ImGui::IsItemActive() || ImGui::IsItemClicked()) {
            const float mx = ImGui::GetIO().MousePos.x - p0.x;
            const int idx = std::clamp(i0 + int(mx / w * float(span)), 0, n);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                sel.start = idx;
                sel.end = idx;
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                sel.end = idx;
            }
        }
    } else {
        ImGui::Dummy(ImVec2(w, h));
    }
    ImGui::PopID();
}

static void draw_sample_editor(App& app) {
    auto& ed = app.editor;
    auto& player = *app.player;
    auto& modu = player.module();

    if (ImGui::BeginTable("smp_layout", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthFixed, 220.f);
        ImGui::TableSetupColumn("edit", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextColumn();
        ImGui::BeginChild("smp_list", ImVec2(0, 280), true);
        for (int i = 0; i < 31; ++i) {
            const auto& samp = modu.samples[size_t(i)];
            char lab[64];
            std::snprintf(lab, sizeof(lab), "%02d %s  (%d)", i + 1,
                          samp.name.empty() ? "--------" : samp.name.c_str(), samp.length_words * 2);
            if (ImGui::Selectable(lab, ed.sample_slot == i)) {
                ed.sample_slot = i;
                app.sync_sample_name_buf();
                {
                    std::lock_guard lock(player.mutex());
                    mod::sample_sel_all(ed.current_sample(modu), ed.sample_sel);
                }
            }
        }
        ImGui::EndChild();

        ImGui::TableNextColumn();
        int vol = 64, ft = 0, rep_s = 0, rep_l = 1;
        int data_len = 0;
        mod::Sample wave_copy;
        {
            std::lock_guard lock(player.mutex());
            auto& s = ed.current_sample(modu);
            vol = s.volume;
            ft = s.finetune;
            rep_s = s.repstart_words;
            rep_l = s.replen_words;
            data_len = int(s.wave.size());
            wave_copy = s;
        }

        ImGui::SetNextItemWidth(180);
        if (ImGui::InputText("Name", app.sample_name_buf, sizeof(app.sample_name_buf))) {
            std::lock_guard lock(player.mutex());
            ed.current_sample(modu).name = app.sample_name_buf;
            ed.mark_dirty();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        if (ImGui::SliderInt("Vol", &vol, 0, 64)) {
            std::lock_guard lock(player.mutex());
            ed.current_sample(modu).volume = vol;
            ed.mark_dirty();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        if (ImGui::SliderInt("Ft", &ft, -8, 7)) {
            std::lock_guard lock(player.mutex());
            ed.current_sample(modu).finetune = ft;
            ed.mark_dirty();
        }

        ImGui::Text("Len %d bytes   Loop %d + %d words", data_len, rep_s, rep_l);
        ImGui::SliderFloat("Zoom", &ed.wave_zoom, 1.f, 32.f, "%.1fx");
        ImGui::SameLine();
        ImGui::SliderFloat("Scroll", &ed.wave_scroll, 0.f, 1.f, "%.2f");
        draw_waveform("wf", wave_copy, ed.sample_sel, ed.wave_zoom, ed.wave_scroll, 130.f);

        if (ImGui::Button("Load…")) {
            app.begin_sample_load(false);
        }
        ImGui::SameLine();
        if (ImGui::Button("From MOD…")) {
            app.begin_sample_load(true);
        }
        ImGui::SameLine();
        ImGui::Checkbox("RAW unsigned", &ed.raw_unsigned);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(50);
        ImGui::DragInt("Steal#", &ed.steal_instr, 1, 1, 31);
        ImGui::SameLine();
        if (ImGui::Button("Save WAV")) {
            app.begin_sample_save(0);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save 8SVX")) {
            app.begin_sample_save(1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save RAW")) {
            app.begin_sample_save(2);
        }

        auto op = [&](const char* label, auto fn) {
            if (ImGui::SmallButton(label)) {
                ed.with_sample_edit(modu, player.mutex(), label, fn);
                app.sync_sample_name_buf();
            }
        };
        op("Cut", [&](mod::Sample& sm, mod::SampleSel& sel) {
            mod::sample_cut(sm, sel, ed.sample_clip);
        });
        ImGui::SameLine();
        op("Copy", [&](mod::Sample& sm, mod::SampleSel& sel) {
            mod::sample_copy(sm, sel, ed.sample_clip);
        });
        ImGui::SameLine();
        op("Paste", [&](mod::Sample& sm, mod::SampleSel& sel) {
            mod::sample_paste(sm, sel, ed.sample_clip);
        });
        ImGui::SameLine();
        op("Clear", [&](mod::Sample& sm, mod::SampleSel& sel) { mod::sample_clear(sm, sel); });
        ImGui::SameLine();
        op("Crop", [&](mod::Sample& sm, mod::SampleSel& sel) { mod::sample_crop(sm, sel); });
        ImGui::SameLine();
        op("Rev", [&](mod::Sample& sm, mod::SampleSel& sel) { mod::sample_reverse(sm, sel); });
        ImGui::SameLine();
        op("Inv", [&](mod::Sample& sm, mod::SampleSel& sel) { mod::sample_invert(sm, sel); });
        ImGui::SameLine();
        op("Boost", [&](mod::Sample& sm, mod::SampleSel& sel) { mod::sample_boost(sm, sel); });
        ImGui::SameLine();
        op("Filter", [&](mod::Sample& sm, mod::SampleSel& sel) { mod::sample_filter(sm, sel); });
        ImGui::SameLine();
        op("FadeIn", [&](mod::Sample& sm, mod::SampleSel& sel) { mod::sample_fade_in(sm, sel); });
        ImGui::SameLine();
        op("FadeOut", [&](mod::Sample& sm, mod::SampleSel& sel) { mod::sample_fade_out(sm, sel); });

        ImGui::SetNextItemWidth(70);
        ImGui::DragFloat("Gain", &ed.amplify_gain, 0.05f, 0.1f, 4.f, "%.2f");
        ImGui::SameLine();
        op("Amp", [&](mod::Sample& sm, mod::SampleSel& sel) {
            mod::sample_amplify(sm, sel, ed.amplify_gain);
        });
        ImGui::SameLine();
        op("Oct-", [&](mod::Sample& sm, mod::SampleSel& sel) { mod::sample_resample(sm, sel, 0.5f); });
        ImGui::SameLine();
        op("Oct+", [&](mod::Sample& sm, mod::SampleSel& sel) { mod::sample_resample(sm, sel, 2.f); });
        ImGui::SameLine();
        op("LoopSel",
           [&](mod::Sample& sm, mod::SampleSel& sel) { mod::sample_set_loop_from_sel(sm, sel); });
        ImGui::SameLine();
        op("NoLoop", [&](mod::Sample& sm, mod::SampleSel&) { mod::sample_disable_loop(sm); });

        ImGui::TextDisabled("Audition: tracker note keys  ·  Esc stops preview");
        ImGui::EndTable();
    }
}

static void draw_spectrum(const char* id, const float* samples, int n, float height = 48.f) {
    ImGui::PushID(id);
    constexpr int kBins = 16;
    float bins[kBins]{};
    if (samples && n > 0) {
        for (int bin = 0; bin < kBins; ++bin) {
            // Skip DC; spread bins across useful spectrum.
            const float freq = 1.f + float(bin) * 0.55f;
            float re = 0.f, im = 0.f;
            for (int i = 0; i < n; ++i) {
                const float ang = 6.2831853f * freq * float(i) / float(n);
                re += samples[i] * std::cos(ang);
                im += samples[i] * std::sin(ang);
            }
            bins[bin] = std::sqrt(re * re + im * im) / float(n) * 14.f;
        }
    }

    const float w = ImGui::GetContentRegionAvail().x;
    const float h = height;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(16, 16, 20, 255));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(48, 48, 56, 255));

    constexpr int kSegs = 12;
    const float gap = 2.f;
    const float bar_w = (w - 2.f - (kBins - 1) * gap) / kBins;
    const float seg_gap = 1.f;
    const float seg_h = (h - 2.f - (kSegs - 1) * seg_gap) / kSegs;

    for (int bin = 0; bin < kBins; ++bin) {
        const int lit = int(std::lround(std::clamp(bins[bin], 0.f, 1.f) * kSegs));
        const float x = p.x + 1.f + bin * (bar_w + gap);
        for (int i = 0; i < kSegs; ++i) {
            const float t = (kSegs <= 1) ? 0.f : float(i) / float(kSegs - 1);
            const float y = p.y + h - 1.f - (i + 1) * seg_h - i * seg_gap;
            const ImVec2 a(x, y);
            const ImVec2 b(x + bar_w, y + seg_h);
            if (i < lit) {
                dl->AddRectFilled(a, b, pt_rainbow(t, false));
            } else {
                dl->AddRectFilled(a, b, IM_COL32(28, 28, 34, 255));
            }
        }
    }
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
}

static void draw_help_window(App& app) {
    if (!app.show_help) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("How to use ImTrakker", &app.show_help)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("help_tabs")) {
        if (ImGui::BeginTabItem("Quick start")) {
            ImGui::TextWrapped(
                "ImTrakker is an Amiga ProTracker-style player and editor. Compose in the "
                "pattern grid, load samples, then Save as a .mod file.");
            ImGui::Spacing();
            ImGui::BulletText("New — start a blank 4-channel song");
            ImGui::BulletText("Browse / Open — load .mod, MMD, or .smus");
            ImGui::BulletText("Turn Edit on (toolbar) before typing notes");
            ImGui::BulletText("F9 Pattern editor  ·  F10 Sample editor");
            ImGui::BulletText("Save / Save As… — write a ProTracker .mod");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.35f, 1.f), "First song recipe");
            ImGui::TextWrapped(
                "1. Click New\n"
                "2. Open Sample (F10) → Load… a WAV into slot 01\n"
                "3. Switch to Pattern (F9), keep Edit on\n"
                "4. Press Z / X / C … or Q / W / E … to place notes\n"
                "5. Space to play  ·  Save As… when happy");
            ImGui::Spacing();
            ImGui::TextDisabled("H toggles this window  ·  P options  ·  Esc quits "
                                "(or stops sample preview)");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Pattern")) {
            ImGui::TextWrapped(
                "Click a cell to move the cursor. With Edit on, the keyboard writes into "
                "the current pattern.");
            ImGui::Spacing();
            if (ImGui::BeginTable("patkeys", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, 200.f);
                ImGui::TableSetupColumn("Action");
                ImGui::TableHeadersRow();
                auto row = [](const char* k, const char* a) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(k);
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", a);
                };
                row("Z S X D C V G B H N J M", "Notes at current octave");
                row("Q 2 W 3 E R 5 T 6 Y 7 U I", "Notes one octave up");
                row("Arrows / Tab", "Move cursor / note·instr·fx·param");
                row("0-9 A-F", "Hex digits on instr / effect / param");
                row("Del / Backspace", "Clear cell (or selection)");
                row("Shift+Arrows", "Block select");
                row("Ctrl+C / X / V", "Copy / cut / paste block");
                row("Ctrl+Z / Y", "Undo / redo");
                row("Oct / Ins / Step", "Toolbar: octave, instrument, row step");
                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Orders strip: click to jump. Right-click an order to change pattern index, "
                "insert, or delete. +Ord appends an order. New Pat adds an empty pattern.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Samples")) {
            ImGui::TextWrapped(
                "F10 opens the sample editor. Pick a slot (01–31), load audio, set loop "
                "points, then use that instrument number in the pattern.");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.35f, 1.f), "Load / save");
            ImGui::BulletText("Load… — WAV, IFF 8SVX, or RAW 8-bit");
            ImGui::BulletText("From MOD… — copy instrument from another module (Steal#)");
            ImGui::BulletText("RAW unsigned — treat raw bytes as 0–255 PCM");
            ImGui::BulletText("Save WAV / 8SVX / RAW — export the current slot");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.35f, 1.f), "Waveform");
            ImGui::BulletText("Click-drag on the wave to select a range");
            ImGui::BulletText("Green lines mark the loop (repstart / replen)");
            ImGui::BulletText("Loop Sel — loop = selection; No Loop — one-shot");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.35f, 1.f), "Ops (undoable)");
            ImGui::TextWrapped(
                "Cut Copy Paste Clear Crop · Reverse Invert Boost Filter · Fade In/Out · "
                "Amplify · Oct− / Oct+ (resample)");
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Audition: on the Sample page, press piano keys to preview the selected "
                "slot without changing the pattern. Esc stops the preview.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Playback")) {
            if (ImGui::BeginTable("playkeys", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, 160.f);
                ImGui::TableSetupColumn("Action");
                ImGui::TableHeadersRow();
                auto row = [](const char* k, const char* a) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(k);
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", a);
                };
                row("Space", "Play / pause");
                row("R", "Restart from beginning");
                row("Play from cursor", "Jump playhead to edit row");
                row("← → (Edit off)", "Seek previous / next order");
                row("F1–F8", "Mute / unmute channel");
                row("U", "Unmute all");
                row("Ctrl+N / Ctrl+S", "New song / Save");
                row("O", "Open file browser");
                row("P", "Options (display + WAV/note export)");
                row("H", "This help page");
                row("Esc", "Stop audition, or quit");
                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Options → Render WAV bounces the song offline. Render Notes dumps patterns "
                "(or SMUS events) to a text file.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Formats")) {
            if (ImGui::BeginTable("fmts", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Format");
                ImGui::TableSetupColumn("Play");
                ImGui::TableSetupColumn("Edit / save");
                ImGui::TableHeadersRow();
                auto row = [](const char* f, const char* p, const char* e) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(f);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(p);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(e);
                };
                row(".mod (M.K. / FLT4 / …)", "yes", "yes → Save as .mod");
                row("MMD0–3 (OctaMED)", "yes", "edit → export .mod");
                row(".smus (Sonix)", "yes", "play only");
                row(".hsq packed mod", "yes", "—");
                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Effects supported in playback: arpeggio (0), porta (1/2), tone porta (3), "
                "volume (C), break (D), jump (B), speed/tempo (F).");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

static void draw_options_window(App& app, SDL_AudioDeviceID adev, AudioBridge& bridge) {
    if (!app.show_options) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(420, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Options / Render", &app.show_options)) {
        ImGui::End();
        return;
    }

    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Scopes", &app.view.show_scopes);
        ImGui::Checkbox("Rainbow VU meters", &app.view.show_vu);
        ImGui::Checkbox("Spectrum analyzer", &app.view.show_spectrum);
        ImGui::Checkbox("Orders strip", &app.view.show_orders);
        ImGui::SliderInt("Pattern zoom (rows)", &app.zoom_rows, 6, 48);
        ImGui::SliderFloat("Meter height", &app.view.meter_height, 32.f, 96.f, "%.0f");
        ImGui::SliderFloat("Spectrum height", &app.view.spectrum_height, 24.f, 80.f, "%.0f");
    }

    if (ImGui::CollapsingHeader("Render WAV", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Seconds", &app.view.render_seconds, 0.5f, 1.f, 600.f, "%.1f");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##render_path", "out.wav", app.view.render_path,
                                 sizeof(app.view.render_path));
        const bool can_render = app.has_song() && app.view.render_seconds > 0.f;
        if (!can_render) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Export WAV")) {
            fs::path out = app.view.render_path[0]
                               ? fs::path(app.view.render_path)
                               : fs::path(app.open_path).stem().string() + ".wav";
            try {
                SDL_LockAudioDevice(adev);
                bridge.player = nullptr;
                bridge.smus = nullptr;
                SDL_UnlockAudioDevice(adev);

                if (app.smus) {
                    app.smus->restart();
                    dump_wav_smus(*app.smus, app.view.render_seconds, out);
                    app.smus->restart();
                } else if (app.player) {
                    app.player->restart();
                    dump_wav_mod(*app.player, app.view.render_seconds, out);
                    app.player->restart();
                }

                SDL_LockAudioDevice(adev);
                bridge.player = app.player.get();
                bridge.smus = app.smus.get();
                SDL_UnlockAudioDevice(adev);

                app.view.render_status = "wrote " + out.string();
                std::strncpy(app.view.render_path, out.string().c_str(),
                             sizeof(app.view.render_path) - 1);
                app.view.render_path[sizeof(app.view.render_path) - 1] = '\0';
            } catch (const std::exception& ex) {
                app.view.render_status = std::string("render failed: ") + ex.what();
                SDL_LockAudioDevice(adev);
                bridge.player = app.player.get();
                bridge.smus = app.smus.get();
                SDL_UnlockAudioDevice(adev);
            }
        }
        if (!can_render) {
            ImGui::EndDisabled();
        }
        ImGui::TextDisabled("Offline bounce — pauses live audio briefly.");
    }

    if (ImGui::CollapsingHeader("Render Notes", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##note_path", "out.txt", app.view.note_path,
                                 sizeof(app.view.note_path));
        if (!app.has_song()) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Export Notes")) {
            fs::path out = app.view.note_path[0]
                               ? fs::path(app.view.note_path)
                               : fs::path(app.open_path).stem().string() + ".txt";
            try {
                if (app.smus) {
                    dump_notes_smus(app.smus->score(), app.smus->bpm(), out);
                } else if (app.player) {
                    dump_notes_mod(app.player->module(), out);
                }
                app.view.render_status = "wrote " + out.string();
                std::strncpy(app.view.note_path, out.string().c_str(), sizeof(app.view.note_path) - 1);
                app.view.note_path[sizeof(app.view.note_path) - 1] = '\0';
            } catch (const std::exception& ex) {
                app.view.render_status = std::string("note dump failed: ") + ex.what();
            }
        }
        if (!app.has_song()) {
            ImGui::EndDisabled();
        }
        ImGui::TextDisabled("Plain-text pattern / track dump.");
    }

    if (!app.view.render_status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", app.view.render_status.c_str());
    }

    ImGui::Separator();
    ImGui::TextDisabled("P  toggle this window");
    ImGui::End();
}

static std::string format_smus_event(const smus::SEvent& ev) {
    char buf[24];
    if (ev.sid < 0x80) {
        static const char* kNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-",
                                         "F#", "G-", "G#", "A-", "A#", "B-"};
        const int midi = ev.sid;
        const int oct = midi / 12 - 1;
        const bool chord = (ev.data & 0x80) != 0;
        const bool tie = (ev.data & 0x40) != 0;
        const int div = ev.data & 0x07;
        std::snprintf(buf, sizeof(buf), "%s%d%s%s d%d", kNames[midi % 12], oct, chord ? "+" : " ",
                      tie ? "~" : " ", div);
        return buf;
    }
    if (ev.sid == 0x80) {
        std::snprintf(buf, sizeof(buf), "--- RST d%d", ev.data & 0x07);
        return buf;
    }
    if (ev.sid == 0x81) {
        std::snprintf(buf, sizeof(buf), "INS %02d", int(ev.data));
        return buf;
    }
    if (ev.sid == 0x84) {
        std::snprintf(buf, sizeof(buf), "VOL %02d", int(ev.data));
        return buf;
    }
    if (ev.sid == 0x88) {
        std::snprintf(buf, sizeof(buf), "TMP %d", int(ev.data));
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "CTL %02X %02X", int(ev.sid), int(ev.data));
    return buf;
}

int main(int argc, char** argv) {
    std::string song_arg;
    float dump_sec = -1.f;
    fs::path dump_path;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dump-wav" && i + 1 < argc) {
            dump_sec = float(std::atof(argv[++i]));
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                dump_path = argv[++i];
            }
        } else if (a[0] != '-') {
            song_arg = a;
        }
    }

    App app;
    app.data_dir = find_data_dir();

    if (!song_arg.empty()) {
        const fs::path initial = resolve_song(app.data_dir, song_arg);
        if (!app.load_path(initial)) {
            std::fprintf(stderr, "%s\n", app.status.c_str());
            return 1;
        }
    }

    if (dump_sec > 0.f) {
        if (!app.has_song()) {
            std::fprintf(stderr, "usage: imtrakker <module> --dump-wav <seconds> [out.wav]\n");
            return 1;
        }
        if (dump_path.empty()) {
            dump_path = fs::path(app.open_path).stem().string() + ".wav";
        }
        if (app.smus) {
            dump_wav_smus(*app.smus, dump_sec, dump_path);
        } else {
            dump_wav_mod(*app.player, dump_sec, dump_path);
        }
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "ImTrakker", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!window || !renderer) {
        std::fprintf(stderr, "SDL window/renderer failed\n");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Tracker owns arrows / letters; don't let ImGui nav steal them.
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    apply_style();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    AudioBridge bridge{app.player.get(), app.smus.get()};
    SDL_AudioSpec want{}, have{};
    want.freq = mod::kSampleRate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    want.userdata = &bridge;
    const SDL_AudioDeviceID adev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!adev) {
        std::fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return 1;
    }
    SDL_PauseAudioDevice(adev, 0);

    auto apply_loaded_module = [&]() {
        SDL_LockAudioDevice(adev);
        bridge.player = app.player.get();
        bridge.smus = app.smus.get();
        SDL_UnlockAudioDevice(adev);
    };

    bool running = true;
    Uint64 prev = SDL_GetPerformanceCounter();
    while (running) {
        const Uint64 now = SDL_GetPerformanceCounter();
        const float dt =
            float(now - prev) / float(SDL_GetPerformanceFrequency());
        prev = now;
        app.flash = std::max(0.f, app.flash - dt);

        // Complete async file dialog / typed open outside the ImGui frame.
        if (app.pending_open && app.pending_open->ready()) {
            auto selection = app.pending_open->result();
            app.pending_open.reset();
            if (!selection.empty()) {
                app.pending_load = selection[0];
            }
        }
        if (app.pending_save && app.pending_save->ready()) {
            auto path = app.pending_save->result();
            app.pending_save.reset();
            if (!path.empty()) {
                app.pending_save_path = path;
            }
        }
        if (app.pending_sample_open && app.pending_sample_open->ready()) {
            auto selection = app.pending_sample_open->result();
            app.pending_sample_open.reset();
            if (!selection.empty()) {
                app.pending_sample_load = selection[0];
            }
        }
        if (app.pending_sample_save && app.pending_sample_save->ready()) {
            auto path = app.pending_sample_save->result();
            app.pending_sample_save.reset();
            if (!path.empty()) {
                app.pending_sample_save_path = path;
            }
        }
        if (!app.pending_save_path.empty() && app.player) {
            const fs::path path = app.pending_save_path;
            app.pending_save_path.clear();
            app.save_to(path);
        }
        if (!app.pending_sample_load.empty() && app.player) {
            const fs::path path = app.pending_sample_load;
            app.pending_sample_load.clear();
            try {
                mod::Sample s;
                if (app.pending_steal_mod) {
                    s = mod::load_sample_from_mod(path, app.editor.steal_instr);
                } else {
                    s = mod::load_sample_file(path, app.editor.raw_unsigned);
                }
                app.editor.replace_sample(app.player->module(), app.player->mutex(),
                                          app.editor.sample_slot, std::move(s));
                app.sync_sample_name_buf();
                app.status = "loaded sample " + path.string();
            } catch (const std::exception& ex) {
                app.status = std::string("sample load failed: ") + ex.what();
            }
        }
        if (!app.pending_sample_save_path.empty() && app.player) {
            const fs::path path = app.pending_sample_save_path;
            app.pending_sample_save_path.clear();
            try {
                mod::Sample s;
                {
                    std::lock_guard lock(app.player->mutex());
                    s = app.editor.current_sample(app.player->module());
                }
                if (app.pending_sample_save_fmt == 1) {
                    mod::save_sample_8svx(s, path);
                } else if (app.pending_sample_save_fmt == 2) {
                    mod::save_sample_raw(s, path);
                } else {
                    mod::save_sample_wav(s, path);
                }
                app.status = "saved sample " + path.string();
            } catch (const std::exception& ex) {
                app.status = std::string("sample save failed: ") + ex.what();
            }
        }
        if (!app.pending_load.empty()) {
            const fs::path path = app.pending_load;
            app.pending_load.clear();
            SDL_LockAudioDevice(adev);
            bridge.player = nullptr;
            bridge.smus = nullptr;
            SDL_UnlockAudioDevice(adev);
            if (app.load_path(path)) {
                apply_loaded_module();
            } else if (app.has_song()) {
                apply_loaded_module();
            }
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) {
                running = false;
            }
            // Only yield keys while typing in an ImGui text field. WantCaptureKeyboard is
            // almost always true (focused buttons/selectables) and was blocking the editor.
            if (e.type == SDL_KEYDOWN && !io.WantTextInput) {
                const SDL_Keycode k = e.key.keysym.sym;
                const bool ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0;
                const bool shift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
                if (k == SDLK_ESCAPE) {
                    if (app.player && app.player->auditioning()) {
                        app.player->stop_audition();
                    } else {
                        running = false;
                    }
                } else if (k == SDLK_o && !ctrl) {
                    app.begin_browse();
                } else if (k == SDLK_n && ctrl && !app.smus) {
                    SDL_LockAudioDevice(adev);
                    bridge.player = nullptr;
                    bridge.smus = nullptr;
                    SDL_UnlockAudioDevice(adev);
                    app.new_song();
                    apply_loaded_module();
                } else if (k == SDLK_s && ctrl && app.player) {
                    if (app.open_path[0] && std::strstr(app.open_path, "untitled") == nullptr) {
                        app.save_to(app.open_path);
                    } else {
                        app.begin_save_as();
                    }
                } else if (k == SDLK_p && !ctrl) {
                    app.show_options = !app.show_options;
                } else if (k == SDLK_h && !ctrl) {
                    app.show_help = !app.show_help;
                } else if (app.smus) {
                    if (k == SDLK_SPACE) {
                        app.smus->set_playing(!app.smus->playing());
                    } else if (k == SDLK_r) {
                        app.smus->restart();
                        app.flash = 0.4f;
                    }
                } else if (app.player) {
                    auto& ed = app.editor;
                    if (ctrl && k == SDLK_z) {
                        ed.undo(app.player->module(), app.player->mutex());
                    } else if (ctrl && k == SDLK_y) {
                        ed.redo(app.player->module(), app.player->mutex());
                    } else if (ctrl && k == SDLK_c && ed.view == mod::EditorView::Pattern) {
                        ed.copy_block(app.player->module());
                    } else if (ctrl && k == SDLK_x && ed.view == mod::EditorView::Pattern) {
                        ed.cut_block(app.player->module(), app.player->mutex());
                    } else if (ctrl && k == SDLK_v && ed.view == mod::EditorView::Pattern) {
                        ed.paste_block(app.player->module(), app.player->mutex());
                    } else if (k == SDLK_F9) {
                        ed.view = mod::EditorView::Pattern;
                    } else if (k == SDLK_F10) {
                        ed.view = mod::EditorView::Sample;
                        app.sync_sample_name_buf();
                    } else if (k == SDLK_SPACE) {
                        app.player->set_playing(!app.player->playing());
                    } else if (k == SDLK_r && !ctrl) {
                        app.player->restart();
                        app.flash = 0.4f;
                    } else if (k == SDLK_DELETE || k == SDLK_BACKSPACE) {
                        if (ed.view == mod::EditorView::Pattern && ed.edit_mode) {
                            if (ed.has_sel) {
                                ed.clear_block(app.player->module(), app.player->mutex());
                            } else {
                                ed.clear_cell(app.player->module(), app.player->mutex());
                            }
                        }
                    } else if (ed.edit_mode &&
                               ed.handle_nav_key(app.player->module(), int(k), shift)) {
                        // cursor / field move
                    } else if (k == SDLK_LEFT && !ed.edit_mode) {
                        app.player->seek_order(-1);
                        app.flash = 0.3f;
                    } else if (k == SDLK_RIGHT && !ed.edit_mode) {
                        app.player->seek_order(1);
                        app.flash = 0.3f;
                    } else if (k >= SDLK_F1 && k <= SDLK_F8) {
                        app.player->toggle_mute(k - SDLK_F1);
                    } else if (k == SDLK_u) {
                        app.player->unmute_all();
                    } else {
                        int period = 0;
                        const int key =
                            (k >= 0 && k < 256) ? int(std::tolower(char(k))) : int(k);
                        if (ed.handle_note_key(app.player->module(), app.player->mutex(), key, shift,
                                               &period)) {
                            if (period) {
                                const int ins = (ed.view == mod::EditorView::Sample)
                                                    ? (ed.sample_slot + 1)
                                                    : ed.instrument;
                                app.player->audition(ins, period);
                            }
                        }
                    }
                }
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        mod::Player::Snapshot snap{};
        smus::Engine::Snapshot ssnap{};
        if (app.player) {
            snap = app.player->snapshot();
            if (snap.row_event) {
                app.flash = std::max(app.flash, 0.1f);
            }
        } else if (app.smus) {
            ssnap = app.smus->snapshot();
            if (ssnap.event_flash) {
                app.flash = std::max(app.flash, 0.1f);
            }
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("ImTrakker", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::TextColored(ImVec4(0.86f, 0.69f, 0.38f, 1.f), "IMTRAKKER");
        ImGui::SameLine();
        if (app.smus) {
            ImGui::TextDisabled("  Sonix SMUS  ·  %.0f BPM", ssnap.bpm);
        } else {
            ImGui::TextDisabled("  Amiga tracker  ·  %s",
                                snap.magic.empty() ? "—" : snap.magic.c_str());
        }
        ImGui::SameLine();
        {
            const float help_w =
                ImGui::CalcTextSize("Help").x + ImGui::GetStyle().FramePadding.x * 2.f;
            const float opts_w =
                ImGui::CalcTextSize("Options").x + ImGui::GetStyle().FramePadding.x * 2.f;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - help_w - opts_w - gap -
                                 ImGui::GetStyle().WindowPadding.x);
            if (ImGui::SmallButton("Help")) {
                app.show_help = !app.show_help;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Options")) {
                app.show_options = !app.show_options;
            }
        }

        ImGui::Separator();

        ImGui::SetNextItemWidth(420);
        ImGui::InputText("##open", app.open_path, sizeof(app.open_path));
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            app.begin_browse();
        }
        ImGui::SameLine();
        if (ImGui::Button("Open") && app.open_path[0]) {
            app.request_open_typed();
        }
        ImGui::SameLine();
        if (ImGui::Button("New")) {
            SDL_LockAudioDevice(adev);
            bridge.player = nullptr;
            bridge.smus = nullptr;
            SDL_UnlockAudioDevice(adev);
            app.new_song();
            apply_loaded_module();
        }
        if (app.player) {
            ImGui::SameLine();
            if (ImGui::Button("Save")) {
                if (app.open_path[0] && std::strstr(app.open_path, "untitled") == nullptr &&
                    fs::path(app.open_path).extension() == ".mod") {
                    app.save_to(app.open_path);
                } else {
                    app.begin_save_as();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Save As…")) {
                app.begin_save_as();
            }
        }

        ImGui::Separator();

        if (!app.has_song()) {
            ImGui::TextWrapped("%s", app.status.c_str());
            ImGui::TextDisabled("New  ·  O open  ·  H help  ·  P options  ·  Esc quit");
            ImGui::End();
            draw_help_window(app);
            draw_options_window(app, adev, bridge);
            ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 12, 8, 6, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
            continue;
        }

        if (app.smus) {
            if (ImGui::Button(ssnap.playing ? "Pause" : "Play")) {
                app.smus->set_playing(!ssnap.playing);
            }
            ImGui::SameLine();
            if (ImGui::Button("Restart")) {
                app.smus->restart();
                app.flash = 0.4f;
            }
            ImGui::SameLine();
            ImGui::Text("%s   %.1f BPM   %s", ssnap.title.c_str(), ssnap.bpm,
                        ssnap.finished ? "END" : (ssnap.playing ? "PLAYING" : "PAUSED"));

            {
                const float total = float(std::max(1, ssnap.pattern_rows));
                ImGui::ProgressBar(float(ssnap.playhead_row) / total, ImVec2(-1, 10), "");
            }

            ImGui::TextDisabled("Space  ·  R restart  ·  O open  ·  H help  ·  P options");
            ImGui::TextWrapped("%s", app.status.c_str());

            const int chn = std::max(1, std::min(4, ssnap.tracks));
            if (ImGui::BeginTable("smus_chs", chn, ImGuiTableFlags_SizingStretchSame)) {
                for (int ci = 0; ci < chn; ++ci) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(ci);
                    const auto& ch = ssnap.channels[size_t(ci)];
                    ImGui::PushStyleColor(ImGuiCol_Text, ch_color(ci));
                    ImGui::Text("AUD%d %s", ci, ch.active ? "" : "·");
                    ImGui::PopStyleColor();
                    ImGui::Text("%s", ch.last_note);
                    ImGui::TextDisabled("%02d %s", ch.instrument_reg, ch.instrument_name);
                    ImGui::TextDisabled("row %d / %d%s", ssnap.playhead_row, ssnap.pattern_rows,
                                        ssnap.track_done[size_t(ci)] ? " done" : "");
                    if (app.view.show_vu) {
                        draw_vu("vu", std::min(1.f, ch.peak * 2.4f),
                                std::min(1.f, ch.peak_hold * 2.4f), false, app.view.meter_height);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            ImGui::Separator();
            ImGui::Text("PATTERN");
            const auto& pat = app.smus->display_pattern();
            const int pat_rows = pat.rows;
            const int visible = std::min(app.zoom_rows, std::max(1, pat_rows));
            const int start = std::clamp(ssnap.playhead_row - visible / 2, 0,
                                         std::max(0, pat_rows - visible));
            if (ImGui::BeginTable("smus_pat", 1 + chn,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY,
                                  ImVec2(-1, 320))) {
                ImGui::TableSetupColumn("ROW", ImGuiTableColumnFlags_WidthFixed, 40);
                for (int c = 0; c < chn; ++c) {
                    char hd[8];
                    std::snprintf(hd, sizeof(hd), "AUD%d", c);
                    ImGui::TableSetupColumn(hd);
                }
                ImGui::TableHeadersRow();
                for (int vi = 0; vi < visible; ++vi) {
                    const int row = start + vi;
                    if (row >= pat_rows) {
                        break;
                    }
                    ImGui::TableNextRow();
                    const bool cur = row == ssnap.playhead_row;
                    if (cur) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                              IM_COL32(180 + int(app.flash * 70), 50, 25, 255));
                    }
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%03d", row);
                    if (row < int(pat.cells.size())) {
                        for (int c = 0; c < chn && c < int(pat.cells[size_t(row)].size()); ++c) {
                            ImGui::TableSetColumnIndex(c + 1);
                            const auto& cell = pat.cells[size_t(row)][size_t(c)];
                            if (cur) {
                                ImGui::TextUnformatted(cell.text);
                            } else if (cell.midi > 0) {
                                ImGui::TextColored(ch_color(c), "%s", cell.text);
                            } else {
                                ImGui::TextDisabled("%s", cell.text);
                            }
                        }
                    }
                }
                ImGui::EndTable();
            }

            ImGui::Separator();
            ImGui::Text("INSTRUMENTS");
            ImGui::BeginChild("smus_ins", ImVec2(0, 48), false, ImGuiWindowFlags_HorizontalScrollbar);
            bool first = true;
            for (const auto& [reg, name] : app.smus->score().instruments) {
                if (!first) {
                    ImGui::SameLine();
                }
                first = false;
                char lab[48];
                std::snprintf(lab, sizeof(lab), "%02d:%s", reg, name.c_str());
                ImGui::SmallButton(lab);
            }
            ImGui::EndChild();

            ImGui::End();
            draw_help_window(app);
            draw_options_window(app, adev, bridge);
            ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 12, 8, 6, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
            continue;
        }

        if (ImGui::Button(snap.playing ? "Pause" : "Play")) {
            app.player->set_playing(!snap.playing);
        }
        ImGui::SameLine();
        if (ImGui::Button("Restart")) {
            app.player->restart();
        }
        ImGui::SameLine();
        if (ImGui::Button("< Ord")) {
            app.player->seek_order(-1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Ord >")) {
            app.player->seek_order(1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Play from cursor")) {
            app.player->seek_row(snap.order_pos, app.editor.row);
            app.player->set_playing(true);
        }
        ImGui::SameLine();
        ImGui::Text("%s%s   ORD %02d/%02d   PAT %02d   ROW %02d   TICK %d/%d   TEMPO %d",
                    app.editor.dirty ? "*" : "", snap.title.c_str(), snap.order_pos, snap.song_length,
                    snap.pattern_index, snap.row, snap.tick, snap.speed, snap.tempo);

        {
            const int rows = snap.pattern.empty() ? mod::kRows : int(snap.pattern.size());
            const float total = float(std::max(1, snap.song_length * rows));
            const float cur = float(snap.order_pos * rows + snap.row);
            ImGui::ProgressBar(cur / total, ImVec2(-1, 10), "");
        }

        // Editor toolbar
        {
            auto& ed = app.editor;
            ImGui::SetNextItemWidth(160);
            if (ImGui::InputText("Title", ed.title_buf, sizeof(ed.title_buf))) {
                std::lock_guard lock(app.player->mutex());
                app.player->module().title = ed.title_buf;
                ed.mark_dirty();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Edit", &ed.edit_mode);
            ImGui::SameLine();
            if (ImGui::RadioButton("Pattern", ed.view == mod::EditorView::Pattern)) {
                ed.view = mod::EditorView::Pattern;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Sample", ed.view == mod::EditorView::Sample)) {
                ed.view = mod::EditorView::Sample;
                app.sync_sample_name_buf();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50);
            ImGui::DragInt("Oct", &ed.octave, 1, 1, 3);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50);
            ImGui::DragInt("Ins", &ed.instrument, 1, 1, 31);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50);
            ImGui::DragInt("Step", &ed.step, 1, 0, 16);
            ImGui::SameLine();
            if (ImGui::SmallButton("Undo") && ed.undo(app.player->module(), app.player->mutex())) {
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Redo") && ed.redo(app.player->module(), app.player->mutex())) {
            }
        }

        ImGui::TextDisabled(
            "F9 Pattern  ·  F10 Sample  ·  Ctrl+N/S  ·  Z/Q piano  ·  Del clear  ·  Ctrl+Z/Y  ·  "
            "Space  ·  H help  ·  P options");
        ImGui::TextWrapped("%s", app.status.c_str());

        const int chn = snap.channels;
        if (ImGui::BeginTable("chs", std::max(1, chn), ImGuiTableFlags_SizingStretchSame)) {
            for (int ci = 0; ci < chn; ++ci) {
                ImGui::TableNextColumn();
                ImGui::PushID(ci);
                const auto& ch = snap.channels_state[size_t(ci)];
                ImGui::PushStyleColor(ImGuiCol_Text, ch_color(ci));
                ImGui::Text("CH%d %s", ci, ch.muted ? "MUTE" : "");
                ImGui::PopStyleColor();
                ImGui::Text("%s  %s", ch.last_note, ch.last_fx);
                std::string sname = "—";
                if (ch.instrument >= 1 && ch.instrument <= int(app.player->module().samples.size())) {
                    sname = app.player->module().samples[size_t(ch.instrument - 1)].name;
                    if (sname.empty()) {
                        sname = "#" + std::to_string(ch.instrument);
                    }
                }
                ImGui::TextDisabled("%02d %s", ch.instrument, sname.c_str());
                if (app.view.show_scopes || app.view.show_vu) {
                    const float scope_h = app.view.meter_height;
                    const float avail = ImGui::GetContentRegionAvail().x;
                    const float vu_w = app.view.show_vu ? 16.f : 0.f;
                    if (app.view.show_scopes) {
                        ImGui::PlotLines("##scope", ch.scope.data(), mod::kScopeSamples, 0, nullptr,
                                         -1.f, 1.f,
                                         ImVec2(std::max(40.f, avail - vu_w), scope_h));
                        if (app.view.show_vu) {
                            ImGui::SameLine(0.f, 4.f);
                        }
                    }
                    if (app.view.show_vu) {
                        draw_vu("vu", std::min(1.f, ch.peak * 2.4f),
                                std::min(1.f, ch.peak_hold * 2.4f), ch.muted, scope_h);
                    }
                }
                if (ImGui::SmallButton(ch.muted ? "Unmute" : "Mute")) {
                    app.player->toggle_mute(ci);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (app.view.show_spectrum) {
            std::array<float, mod::kScopeSamples> mix{};
            for (int ci = 0; ci < chn; ++ci) {
                const auto& ch = snap.channels_state[size_t(ci)];
                if (ch.muted) {
                    continue;
                }
                for (int i = 0; i < mod::kScopeSamples; ++i) {
                    mix[size_t(i)] += ch.scope[size_t(i)];
                }
            }
            ImGui::TextDisabled("SPECTRUM");
            draw_spectrum("spec", mix.data(), mod::kScopeSamples, app.view.spectrum_height);
        }

        ImGui::Separator();

        if (app.editor.view == mod::EditorView::Sample) {
            ImGui::Text("SAMPLE EDITOR");
            draw_sample_editor(app);
        } else {
            auto& ed = app.editor;
            // Sync edit pattern index with playhead when not editing
            if (!ed.edit_mode) {
                ed.pat = snap.pattern_index;
                ed.row = snap.row;
            }

            std::vector<std::vector<mod::Note>> pat_view;
            {
                std::lock_guard lock(app.player->mutex());
                ed.ensure_pattern(app.player->module(), ed.pat);
                if (ed.pat >= 0 && ed.pat < app.player->module().pattern_count()) {
                    pat_view = app.player->module().patterns[size_t(ed.pat)];
                }
            }

            ImGui::Text("PATTERN #%02d  cursor R%02d C%d", ed.pat, ed.row, ed.ch);
            ImGui::SameLine();
            if (ImGui::SmallButton("New Pat")) {
                ed.pat = ed.add_pattern(app.player->module(), app.player->mutex());
            }

            const int pat_rows = pat_view.empty() ? 0 : int(pat_view.size());
            const int focus_row = ed.edit_mode ? ed.row : snap.row;
            const int visible = std::min(app.zoom_rows, std::max(1, pat_rows));
            const int start =
                std::clamp(focus_row - visible / 2, 0, std::max(0, pat_rows - visible));

            // Selection bounds
            int sr0 = ed.sel_row0, sr1 = ed.sel_row1, sc0 = ed.sel_ch0, sc1 = ed.sel_ch1;
            if (ed.has_sel) {
                if (sr1 < sr0) {
                    std::swap(sr0, sr1);
                }
                if (sc1 < sc0) {
                    std::swap(sc0, sc1);
                }
            }

            if (ImGui::BeginTable("pat", 1 + chn,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY,
                                  ImVec2(-1, 280))) {
                ImGui::TableSetupColumn("ROW", ImGuiTableColumnFlags_WidthFixed, 40);
                for (int c = 0; c < chn; ++c) {
                    char hd[8];
                    std::snprintf(hd, sizeof(hd), "CH%d", c);
                    ImGui::TableSetupColumn(hd);
                }
                ImGui::TableHeadersRow();
                for (int vi = 0; vi < visible; ++vi) {
                    const int row = start + vi;
                    if (row >= pat_rows) {
                        break;
                    }
                    ImGui::TableNextRow();
                    const bool play_row = row == snap.row && ed.pat == snap.pattern_index;
                    if (play_row) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                              IM_COL32(180 + int(app.flash * 70), 50, 25, 255));
                    }
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%02d", row);
                    if (row < int(pat_view.size())) {
                        for (int c = 0; c < chn && c < int(pat_view[size_t(row)].size()); ++c) {
                            ImGui::TableSetColumnIndex(c + 1);
                            const auto& note = pat_view[size_t(row)][size_t(c)];
                            const bool cur = ed.edit_mode && row == ed.row && c == ed.ch;
                            const bool in_sel =
                                ed.has_sel && row >= sr0 && row <= sr1 && c >= sc0 && c <= sc1;
                            if (in_sel) {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                                      IM_COL32(90, 50, 20, 180));
                            }
                            if (cur) {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                                      IM_COL32(200, 110, 30, 220));
                            }
                            char label[32];
                            std::snprintf(label, sizeof(label), "%s##r%dc%d", note.text().c_str(),
                                          row, c);
                            if (ImGui::Selectable(label, cur, ImGuiSelectableFlags_AllowDoubleClick)) {
                                ed.row = row;
                                ed.ch = c;
                                ed.pat = ed.pat;
                                if (ImGui::GetIO().KeyShift) {
                                    ed.update_sel_to_cursor();
                                } else {
                                    ed.has_sel = false;
                                }
                            }
                            if (!cur) {
                                // Selectable already drew text; colour via style when not current
                            }
                        }
                    }
                }
                ImGui::EndTable();
            }
        }

        if (app.view.show_orders) {
            ImGui::Separator();
            ImGui::Text("ORDERS");
            ImGui::BeginChild("orders", ImVec2(0, 52), false, ImGuiWindowFlags_HorizontalScrollbar);
            auto& ed = app.editor;
            for (int i = 0; i < snap.song_length; ++i) {
                if (i) {
                    ImGui::SameLine();
                }
                const int pat = (i < int(snap.orders.size())) ? snap.orders[size_t(i)] : 0;
                char lab[8];
                std::snprintf(lab, sizeof(lab), "%02d", pat);
                if (i == snap.order_pos) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.92f, 0.46f, 0.12f, 1.f));
                } else if (i < snap.order_pos) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.24f, 0.12f, 1.f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.12f, 0.08f, 1.f));
                }
                ImGui::PushID(i);
                if (ImGui::SmallButton(lab)) {
                    app.player->seek_order(i - snap.order_pos);
                    ed.pat = pat;
                }
                if (ImGui::BeginPopupContextItem("ordctx")) {
                    int p = pat;
                    if (ImGui::InputInt("Pattern", &p)) {
                        ed.set_order_pattern(app.player->module(), app.player->mutex(), i,
                                             std::max(0, p));
                    }
                    if (ImGui::MenuItem("Insert after")) {
                        ed.insert_order(app.player->module(), app.player->mutex(), i + 1);
                    }
                    if (ImGui::MenuItem("Delete")) {
                        ed.delete_order(app.player->module(), app.player->mutex(), i);
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImGui::PopStyleColor();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+Ord")) {
                ed.insert_order(app.player->module(), app.player->mutex(), snap.song_length);
            }
            ImGui::EndChild();
        }

        ImGui::End();
        draw_help_window(app);
        draw_options_window(app, adev, bridge);
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 12, 8, 6, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    SDL_CloseAudioDevice(adev);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
