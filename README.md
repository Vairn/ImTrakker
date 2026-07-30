# ImTrakker

Amiga ProTracker / Startrekker **player and editor** with an **SDL2 + Dear ImGui** UI.

Built as a sibling project to the [Dune Amiga](../Amiga/Dune) reverse-engineering tree. Open existing modules or **New** a blank song, edit patterns/samples, and **Save** as `.mod`.

## Supported formats

| Format | Play | Edit / Save |
|--------|------|-------------|
| ProTracker / Startrekker (`.mod`) | yes | yes (save as `.mod`) |
| SoundFX 1.3 / 2.0 (`.sfx` / `.sfx2`) | yes | edit in memory → save as `.mod` |
| OctaMED MMD0–3 | yes | edit in memory → save as `.mod` |
| Sonix SMUS | yes | play-only |
| HSQ-packed mods | yes (unpack) | — |

### Magics (@1080)

| Magic | Channels |
|-------|----------|
| `FLT4` | 4 (Dune / Startrekker) |
| `M.K.` / `M!K!` / `4CHN` | 4 |
| `6CHN` | 6 |
| `8CHN` / `FLT8` | 8 |

Effect set: full ProTracker 2.3d (0–F and E0–EF), including vibrato/tremolo,
tone porta, sample offset, volume slide, finetune tables, pattern loop/delay,
note cut/delay, retrig, invert loop, and Amiga LED filter (E0).

## Build (Windows / MSYS2 UCRT64)

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH="C:/msys64/ucrt64"
cmake --build build
```

Requires **SDL2** (and a C++17 compiler). Dear ImGui is pulled via CMake FetchContent.

## Run

```bash
./build/imtrakker.exe
./build/imtrakker.exe path/to/song.mod
./build/imtrakker.exe m1 --dump-wav 2 out.wav
```

## Editor

Press **H** or the **Help** button in the app for the full how-to (quick start, pattern keys, samples, playback, formats).

- **New** — blank 4-channel song
- **Save / Save As…** — ProTracker `.mod`
- **Pattern** (F9) — click cells, tracker piano keys, effects
- **Sample** (F10) — waveform, loop, DSP ops, load/save samples
- **Help** (H) — in-app how-to page
- **Options** (P) — display + WAV / note export

### Pattern keys

| Key | Action |
|-----|--------|
| `Z S X D C V G B H N J M` | Notes (current octave) |
| `Q 2 W 3 E R 5 T 6 Y 7 U I` | Notes (octave + 1) |
| Arrows / Tab | Move cursor / field |
| Digits / `A`–`F` | Instrument / effect / param (hex) |
| Del | Clear cell / selection |
| Shift+arrows | Block select |
| Ctrl+C / X / V | Copy / cut / paste block |
| Ctrl+Z / Y | Undo / redo |
| Ctrl+N / S | New / Save |
| Space | Play / pause |
| F1–F8 | Mute channels |
| Esc | Stop sample audition (or quit) |

### Sample formats

Load: **WAV** (PCM), **IFF 8SVX**, **RAW** signed 8-bit (optional unsigned), or steal from another `.mod`.

Save sample: WAV / 8SVX / RAW.

Ops: cut/copy/paste/clear/crop, reverse, invert, amplify, fade, boost, filter, octave resample, set/clear loop. Audition with the same piano keys while on the Sample page.

## Layout

| Path | Role |
|------|------|
| `src/mod/` | Module load/save, player, editor, sample I/O |
| `src/smus/` | Sonix SMUS engine (playback) |
| `src/main.cpp` | SDL audio + ImGui UI |
| `src/hsq.*` | Optional Cryo HSQ unpack |
