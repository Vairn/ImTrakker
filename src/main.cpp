#include "mod/module.hpp"
#include "mod/player.hpp"
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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
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

struct App {
    fs::path data_dir;
    std::unique_ptr<mod::Player> player;
    std::unique_ptr<smus::Engine> smus;
    std::string status = "Open a module to begin";
    char open_path[512]{};
    int zoom_rows = 14;
    float flash = 0.f;
    std::shared_ptr<pfd::open_file> pending_open;
    std::string pending_load;

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

static void draw_vu(const char* id, float level, float hold, ImVec4 col) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 18.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(36, 24, 16, 255));
    const int segs = 18;
    const float gap = 2.f;
    const float seg_w = (w - (segs - 1) * gap) / segs;
    const int lit = int(std::lround(std::clamp(level, 0.f, 1.f) * segs));
    const int hold_seg = int(std::lround(std::clamp(hold, 0.f, 1.f) * segs));
    for (int i = 0; i < segs; ++i) {
        const float x = p.x + i * (seg_w + gap);
        ImU32 c = ImGui::ColorConvertFloat4ToU32(col);
        if (i >= int(segs * 0.8f)) {
            c = IM_COL32(255, 90, 40, 255);
        } else if (i >= int(segs * 0.55f)) {
            c = IM_COL32(255, 170, 60, 255);
        }
        if (i < lit) {
            dl->AddRectFilled(ImVec2(x, p.y + 2), ImVec2(x + seg_w, p.y + h - 2), c);
        } else if (i == hold_seg - 1 && hold_seg > 0) {
            dl->AddRectFilled(ImVec2(x, p.y + 2), ImVec2(x + seg_w, p.y + h - 2), IM_COL32(220, 176, 96, 255));
        }
    }
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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
            if (e.type == SDL_KEYDOWN && !io.WantCaptureKeyboard) {
                const SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) {
                    running = false;
                } else if (k == SDLK_o) {
                    app.begin_browse();
                } else if (app.smus) {
                    if (k == SDLK_SPACE) {
                        app.smus->set_playing(!app.smus->playing());
                    } else if (k == SDLK_r) {
                        app.smus->restart();
                        app.flash = 0.4f;
                    }
                } else if (app.player) {
                    if (k == SDLK_SPACE) {
                        app.player->set_playing(!app.player->playing());
                    } else if (k == SDLK_r) {
                        app.player->restart();
                        app.flash = 0.4f;
                    } else if (k == SDLK_LEFT) {
                        app.player->seek_order(-1);
                        app.flash = 0.3f;
                    } else if (k == SDLK_RIGHT) {
                        app.player->seek_order(1);
                        app.flash = 0.3f;
                    } else if (k >= SDLK_F1 && k <= SDLK_F8) {
                        app.player->toggle_mute(k - SDLK_F1);
                    } else if (k == SDLK_u) {
                        app.player->unmute_all();
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

        ImGui::Separator();

        ImGui::SetNextItemWidth(520);
        ImGui::InputText("##open", app.open_path, sizeof(app.open_path));
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            app.begin_browse();
        }
        ImGui::SameLine();
        if (ImGui::Button("Open") && app.open_path[0]) {
            app.request_open_typed();
        }

        ImGui::Separator();

        if (!app.has_song()) {
            ImGui::TextWrapped("%s", app.status.c_str());
            ImGui::TextDisabled("O / Browse...  ·  Esc quit");
            ImGui::End();
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
                float prog = 0.f;
                int n = 0;
                for (int ci = 0; ci < ssnap.tracks && ci < 4; ++ci) {
                    if (ssnap.track_length[size_t(ci)] > 0) {
                        prog += float(ssnap.track_index[size_t(ci)]) /
                                float(ssnap.track_length[size_t(ci)]);
                        ++n;
                    }
                }
                ImGui::ProgressBar(n ? prog / float(n) : 0.f, ImVec2(-1, 10), "");
            }

            ImGui::TextDisabled("Space  ·  R restart  ·  O open");
            ImGui::TextWrapped("%s", app.status.c_str());

            const int chn = std::max(1, std::min(4, ssnap.tracks));
            if (ImGui::BeginTable("smus_chs", chn, ImGuiTableFlags_SizingStretchSame)) {
                for (int ci = 0; ci < chn; ++ci) {
                    ImGui::TableNextColumn();
                    const auto& ch = ssnap.channels[size_t(ci)];
                    ImGui::PushStyleColor(ImGuiCol_Text, ch_color(ci));
                    ImGui::Text("AUD%d %s", ci, ch.active ? "" : "·");
                    ImGui::PopStyleColor();
                    ImGui::Text("%s", ch.last_note);
                    ImGui::TextDisabled("%02d %s", ch.instrument_reg, ch.instrument_name);
                    ImGui::TextDisabled("evt %d / %d%s", ssnap.track_index[size_t(ci)],
                                        ssnap.track_length[size_t(ci)],
                                        ssnap.track_done[size_t(ci)] ? " done" : "");
                    draw_vu("vu", std::min(1.f, ch.peak * 2.4f), std::min(1.f, ch.peak_hold * 2.4f),
                            ch_color(ci));
                }
                ImGui::EndTable();
            }

            ImGui::Separator();
            ImGui::Text("TRACKS");
            const auto& score = app.smus->score();
            int max_len = 1;
            for (int ci = 0; ci < chn && ci < int(score.tracks.size()); ++ci) {
                max_len = std::max(max_len, int(score.tracks[size_t(ci)].size()));
            }
            // Follow the furthest-ahead live cursor so the window stays useful.
            int focus = 0;
            for (int ci = 0; ci < chn; ++ci) {
                focus = std::max(focus, ssnap.track_index[size_t(ci)]);
            }
            const int visible = std::min(app.zoom_rows, max_len);
            const int start = std::clamp(focus - visible / 2, 0, std::max(0, max_len - visible));
            if (ImGui::BeginTable("smus_pat", 1 + chn,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY,
                                  ImVec2(-1, 320))) {
                ImGui::TableSetupColumn("EVT", ImGuiTableColumnFlags_WidthFixed, 40);
                for (int c = 0; c < chn; ++c) {
                    char hd[8];
                    std::snprintf(hd, sizeof(hd), "AUD%d", c);
                    ImGui::TableSetupColumn(hd);
                }
                ImGui::TableHeadersRow();
                for (int vi = 0; vi < visible; ++vi) {
                    const int row = start + vi;
                    if (row >= max_len) {
                        break;
                    }
                    ImGui::TableNextRow();
                    bool any_cur = false;
                    for (int c = 0; c < chn; ++c) {
                        if (row == ssnap.track_index[size_t(c)]) {
                            any_cur = true;
                            break;
                        }
                    }
                    if (any_cur) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                              IM_COL32(180 + int(app.flash * 70), 50, 25, 255));
                    }
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%03d", row);
                    for (int c = 0; c < chn; ++c) {
                        ImGui::TableSetColumnIndex(c + 1);
                        if (c >= int(score.tracks.size()) || row >= int(score.tracks[size_t(c)].size())) {
                            ImGui::TextDisabled("...");
                            continue;
                        }
                        const auto& ev = score.tracks[size_t(c)][size_t(row)];
                        const std::string text = format_smus_event(ev);
                        const bool cur = row == ssnap.track_index[size_t(c)];
                        if (cur) {
                            ImGui::TextUnformatted(text.c_str());
                        } else {
                            ImGui::TextColored(ch_color(c), "%s", text.c_str());
                        }
                    }
                }
                ImGui::EndTable();
            }

            ImGui::Separator();
            ImGui::Text("INSTRUMENTS");
            ImGui::BeginChild("smus_ins", ImVec2(0, 48), false, ImGuiWindowFlags_HorizontalScrollbar);
            bool first = true;
            for (const auto& [reg, name] : score.instruments) {
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
        ImGui::Text("%s   ORD %02d/%02d   PAT %02d   ROW %02d   TICK %d/%d   TEMPO %d",
                    snap.title.c_str(), snap.order_pos, snap.song_length, snap.pattern_index,
                    snap.row, snap.tick, snap.speed, snap.tempo);

        {
            const int rows = snap.pattern.empty() ? mod::kRows : int(snap.pattern.size());
            const float total = float(std::max(1, snap.song_length * rows));
            const float cur = float(snap.order_pos * rows + snap.row);
            ImGui::ProgressBar(cur / total, ImVec2(-1, 10), "");
        }

        ImGui::TextDisabled(
            "O open  ·  Space  ·  Left/Right order  ·  F1-F8 mute  ·  U unmute  ·  R restart");
        ImGui::TextWrapped("%s", app.status.c_str());

        const int chn = snap.channels;
        if (ImGui::BeginTable("chs", std::max(1, chn), ImGuiTableFlags_SizingStretchSame)) {
            for (int ci = 0; ci < chn; ++ci) {
                ImGui::TableNextColumn();
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
                ImGui::PlotLines("##scope", ch.scope.data(), mod::kScopeSamples, 0, nullptr, -1.f, 1.f,
                                 ImVec2(-1, 40));
                draw_vu("vu", std::min(1.f, ch.peak * 2.4f), std::min(1.f, ch.peak_hold * 2.4f),
                        ch.muted ? ImVec4(0.3f, 0.22f, 0.16f, 1.f) : ch_color(ci));
                if (ImGui::SmallButton(ch.muted ? "Unmute" : "Mute")) {
                    app.player->toggle_mute(ci);
                }
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        ImGui::Text("PATTERN #%02d", snap.pattern_index);
        const int pat_rows = snap.pattern.empty() ? 0 : int(snap.pattern.size());
        const int visible = std::min(app.zoom_rows, std::max(1, pat_rows));
        const int start =
            std::clamp(snap.row - visible / 2, 0, std::max(0, pat_rows - visible));
        if (ImGui::BeginTable("pat", 1 + chn,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY,
                              ImVec2(-1, 320))) {
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
                const bool cur = row == snap.row;
                if (cur) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                          IM_COL32(180 + int(app.flash * 70), 50, 25, 255));
                }
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%02d", row);
                if (row < int(snap.pattern.size())) {
                    for (int c = 0; c < chn && c < int(snap.pattern[size_t(row)].size()); ++c) {
                        ImGui::TableSetColumnIndex(c + 1);
                        const auto& note = snap.pattern[size_t(row)][size_t(c)];
                        if (cur) {
                            ImGui::TextUnformatted(note.text().c_str());
                        } else {
                            ImGui::TextColored(ch_color(c), "%s", note.text().c_str());
                        }
                    }
                }
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        ImGui::Text("ORDERS");
        ImGui::BeginChild("orders", ImVec2(0, 40), false, ImGuiWindowFlags_HorizontalScrollbar);
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
            }
            ImGui::PopID();
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        ImGui::End();
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
