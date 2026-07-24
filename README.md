# ImTrakker

Amiga ProTracker / Startrekker module player with an **SDL2 + Dear ImGui** UI.

Built as a sibling project to the [Dune Amiga](../Amiga/Dune) reverse-engineering tree. Default playlist points at Dune’s ripped FLT4 songs (`m1` / `m2` / `m3`); you can also open arbitrary supported modules.

## Supported magics (@1080)

| Magic | Channels |
|-------|----------|
| `FLT4` | 4 (Dune / Startrekker) |
| `M.K.` / `M!K!` / `4CHN` | 4 |
| `6CHN` | 6 |
| `8CHN` / `FLT8` | 8 |

Effect subset matches `Dune/tools/play_tracker.py` (arpeggio, porta, volume, speed/tempo, break/jump).

## Build (Windows / MSYS2 UCRT64)

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH="C:/msys64/ucrt64"
cmake --build build
```

Requires **SDL2** (and a C++17 compiler). Dear ImGui is pulled via CMake FetchContent.

## Run

```bash
# Uses ../Amiga/Dune/ripped if present, or set IMTRAKKER_DATA / DUNE_DATA
./build/imtrakker.exe
./build/imtrakker.exe m2
./build/imtrakker.exe path/to/song.mod
./build/imtrakker.exe m1 --dump-wav 2 out.wav
```

Keys: `1`/`2`/`3` Dune songs · `Space` pause · `←`/`→` order · `F1`–`F8` mute · `R` restart · `O` open file · `Esc` quit

## Layout

| Path | Role |
|------|------|
| `src/mod/` | Module loader + Paula-ish mixer |
| `src/main.cpp` | SDL audio + ImGui tracker UI |
| `src/hsq.*` | Optional Cryo HSQ unpack (for still-packed `.hsq`) |
