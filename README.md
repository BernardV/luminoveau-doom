# DOOM on the Luminoveau engine

The original id Software **linuxdoom-1.10** source ported to run on the
**Luminoveau** game engine, building for macOS, Windows, and the web (WebAssembly).

Doom keeps its own renderer (the classic software renderer drawing into a
320×200 8-bit palettized framebuffer). Luminoveau supplies the window, GPU
present, input, and timing. Each frame the palette-indexed framebuffer is
converted to RGBA, streamed into a GPU texture, and blitted to the window
(letterboxed to 4:3).

## Layout

```
lumi_doom/
├── lumi/                     # the Luminoveau engine (unmodified)
├── doom/linuxdoom-1.10/      # id Software Doom source (small portability + 64-bit fixes)
└── doomgame/                 # this port
    ├── CMakeLists.txt        # builds doomcore (C) + the doom app (C++ host)
    ├── assets/doom1.wad      # shareware IWAD (freely redistributable)
    ├── build_mac.sh          # native macOS build
    ├── build_web.sh          # Emscripten/WebAssembly build
    └── src/
        ├── main.cpp          # Luminoveau host: window, present, input, timing
        ├── i_lumi.c          # Doom platform layer (video/system/sound/net)
        └── dg_bridge.h       # plain-C bridge between the two
```

## How it works

- **`i_lumi.c`** replaces Doom's original `i_video.c` / `i_system.c` /
  `i_sound.c` / `i_net.c` / `i_main.c`. It implements every `I_*` platform
  function and exposes a tiny plain-C bridge (`DG_Init` / `DG_Tic` /
  `DG_KeyEvent` / `DG_GetFramebuffer` …).
- **`main.cpp`** implements the four Luminoveau callbacks. `AppInit` runs Doom's
  full startup; `AppIterate` advances Doom one tic, uploads the framebuffer, and
  presents. Keyboard is polled from `SDL_GetKeyboardState` each frame.
- Doom's main loop was refactored: `D_DoomMain` returns after init and the old
  `while(1)` body became `D_DoomTic()`, called once per host frame — so the
  engine owns the loop (required for a single-threaded WebAssembly build).

## Building

### macOS
```
./build_mac.sh
open build-mac/bin/doom.app
```
Produces a proper `.app` bundle. **Launch it with `open`** (a bare CLI binary on
macOS never becomes a foreground app, so it wouldn't receive keyboard input).

### Web (WebAssembly)
Requires the Emscripten toolchain on PATH (`emcc`, `emcmake`).
```
./build_web.sh
(cd build-web/bin && python3 -m http.server 8000)
```
Open <http://localhost:8000/doom.html> in a **WebGPU-capable browser** (recent
Chrome/Edge; Safari Technology Preview). The WAD is preloaded into the virtual
filesystem at link time.

### Windows (cross-compiled from macOS/Linux via mingw-w64)
```
brew install mingw-w64      # or your distro's package
./build_win.sh
```
Produces `build-win/bin/` — a runnable folder: `doom.exe` + the three mingw
runtime DLLs + `assets/`. Copy the whole folder to a Windows machine and run
`doom.exe` (needs GPU drivers; SDL_GPU uses the Vulkan/D3D12 backend).

Notes:
- Built as **RelWithDebInfo** on purpose: the engine hardcodes `-march=native`
  in its Release CXX flags, which mingw's gcc rejects. `toolchain-mingw.cmake`
  pins a generic target; RelWithDebInfo avoids the native flag + LTO.
- Cross-compiled, **not yet run on real Windows here** (no Windows host) — it's a
  valid PE32+ with all deps bundled, but treat as untested until you run it.
- A native **MSVC** build should also work from a Visual Studio "x64 Native
  Tools" prompt (`cmake -S doomgame -B build -DCMAKE_BUILD_TYPE=Release &&
  cmake --build build --config Release`).

## Controls (defaults)

| Action | Key |
|---|---|
| Move | Arrow keys |
| Fire | Ctrl |
| Use / open doors | Space |
| Run | Shift |
| Strafe | Alt (or `,` / `.`) |
| Menu | Esc |
| Weapons | 1–7 |
| Toggle crisp/smooth scaling | `` ` `` (grave) |

## The WAD

`assets/doom1.wad` is the freely redistributable **shareware** IWAD (Doom v1.9,
episode 1). Drop in `doom.wad` / `doom2.wad` (registered/commercial) alongside it
to play the full games — Doom auto-detects which IWAD is present.

## Audio

**Sound effects work.** Doom's 8-channel software mixer runs on the audio thread
via Luminoveau's PCM generator (`Audio::CreatePCMGenerator`): each SFX lump
(8-bit unsigned PCM, 11025 Hz) is resampled to 44.1 kHz and panned per channel.
See `DG_MixAudio` in `i_lumi.c`.

**Music is not implemented** — the original linuxdoom left `I_PlaySong` et al. as
stubs (MUS playback needs a MUS→synth path); those remain no-ops here.

On the web, browser autoplay policy keeps the audio context suspended until the
first click/keypress, so sound starts once you interact with the page.

## Notes / limitations

- Shareware demo lumps report a different version ("Demo is from a different game
  version!") and are skipped — harmless, the title/credit screens still cycle.
- Screen wipes complete within a single host frame (no animated melt).
- A visual upscale filter (bilinear) can be toggled at runtime with `` ` ``. Real
  HD textures would require a GPU renderer (Doom's software renderer works in
  8-bit palette indices and can't sample filtered/hi-res textures).

## Fixes applied to the Doom source

The 1993 source assumes a 32-bit ILP32 platform. Minimal changes for 64-bit +
non-Linux (all clearly commented in-tree):

- `r_data.c` / `p_setup.c` — pointer arrays sized with `sizeof(*ptr)` instead of a
  hardcoded `4`.
- `r_data.c` — `maptexture_t.columndirectory` made 4 bytes (was `void**`).
- `r_data.c` / `r_draw.c` — colormap/translation-table alignment via `intptr_t`,
  not `(int)`.
- `m_misc.c` — dropped the string-valued chat-macro config rows (stored a pointer
  in an `int`).
- `w_wad.c` / `r_data.c` / `m_bbox.c` — portable headers (`<alloca.h>`/`<limits.h>`
  instead of `<malloc.h>`/`<values.h>`).
- `doomdef.h` — disabled the `SNDSERV` external-sound-server hack.
- `d_main.c` — split `D_DoomMain` init from the per-frame `D_DoomTic`.
```
