# DOOM on Luminoveau

[![Play in browser](https://img.shields.io/badge/play-in%20browser-e33)](https://bernardv.github.io/luminoveau-doom/)
[![Web deploy](https://github.com/BernardV/luminoveau-doom/actions/workflows/deploy-web.yml/badge.svg)](https://github.com/BernardV/luminoveau-doom/actions/workflows/deploy-web.yml)

A port of id Software's **DOOM** (the 1997 `linuxdoom-1.10` source release) to the
[**Luminoveau**](https://github.com/bXi/luminoveau) game engine, with the classic
software 3D renderer **replaced by a GPU renderer** and a set of modern comforts:
widescreen full-window rendering, smooth or crisp texturing, bloom / tonemap /
vignette, dynamic coloured lights, mouselook, gamepad support, and a full
**touch-control layer** that turns the web build into an installable mobile PWA.

Runs on **macOS**, **Windows**, **Linux**, and the **Web (WebAssembly + WebGPU)**.

> This is a source port for study and fun. It bundles only the **freely
> distributable shareware IWAD** (`doom1.wad`, episode 1); for the full game you
> supply your own commercial IWAD (see [Game data](#game-data)).

---

## Play & download

- **Play in your browser:** **[bernardv.github.io/luminoveau-doom](https://bernardv.github.io/luminoveau-doom/)**
  — needs a WebGPU browser (Chrome / Edge, or Safari 17.4+). Tap/click to start; on a
  phone use "Add to Home Screen" for a fullscreen, installable PWA. **Savegames persist**
  in the browser (stored in IndexedDB), so you can save, reload, and continue. You can
  also **bring your own IWAD** — the start screen lets you load your own DOOM / Ultimate
  DOOM / DOOM II WAD (kept in your browser, never uploaded) instead of the shareware.
- **Download a build:** the latest [**Releases**](https://github.com/BernardV/luminoveau-doom/releases)
  have `doom-macos.zip`, `doom-windows.zip`, `doom-linux.zip` and `doom-web.zip`.
  - **macOS** (Apple Silicon): the app is ad-hoc signed but not Apple-notarized, so the
    first time, **right-click → Open** — or run `xattr -dr com.apple.quarantine doom.app`
    — to get past Gatekeeper.
  - **Windows**: unzip and run `doom.exe` — statically linked, no DLLs needed.
  - **Linux**: needs a Vulkan-capable GPU/driver.

Every build ships shareware episode 1 and is playable out of the box; drop your own
IWAD in for the full game (see [Game data](#game-data)).

### How it's built

- **Web** deploys to GitHub Pages automatically on every push to `main`.
- **Releases** (macOS / Linux / Web zips) are built only when a version tag `vX.Y`
  is pushed, and attached to the GitHub Release. The engine is fetched at build time,
  so a fresh checkout builds without vendored engine sources.

---

## How it works

DOOM's original **game simulation** runs unchanged — physics, monster AI, the BSP,
all the game logic. What's replaced is the **rendering**. Instead of Doom's software
rasteriser drawing the 3D view into a 320×200 palettized buffer, a thin C bridge
(`src/dg_*`) hands the live world — walls, floors and ceilings, sprites and the
camera — to a custom Luminoveau `RenderPass` (`DoomRenderPass`) that draws it as
**true 3D on the GPU**, every frame, at the screen's native resolution. Luminoveau
owns the window, GPU present, input, timing and audio.

- **Full-window 3D** — the world fills the whole window in real perspective (Hor+
  widescreen: the classic vertical FOV is kept and the horizontal FOV widens with the
  screen, so you see more to the sides rather than a stretched image). Doom's software
  drawing is still used for the **2D layer** — HUD, status bar, menus, the level-end
  screens — composited on top in a 4:3-centred box.
- **Two looks**, toggled live with **F5**:
  - **Modern** (default) — smooth textures, bloom, filmic tonemap, vignette, soft dynamic coloured lights.
  - **Authentic+** — crisp nearest-filtered textures, the classic pixelated look.
- **Extras the software renderer never had** — mouselook / touch look with vertical
  aim, dynamic lights from lamps, torches and projectiles, and a 3D sky dome.
- **Audio** — sound effects plus music (Doom MUS lumps synthesised through a
  General-MIDI soundfont via TinySoundFont).

The classic software renderer is still in the tree — `DOOM_GPU=0` runs the original,
pixel-for-pixel. The engine is fetched at build time (pinned upstream commit, used
unmodified); the Doom source is vendored. See [Building](#building).

---

## Building

Requires CMake ≥ 3.20 and a C/C++ toolchain. The engine is pulled from GitHub over
**SSH** by default — if you don't have SSH keys, add
`-DLUMINOVEAU_GIT_URL=https://github.com/bXi/luminoveau.git`.

```sh
# macOS  → build-mac/bin/doom.app
./build_mac.sh

# Web    → build-web/bin/index.html  (needs Emscripten on PATH)
./build_web.sh
# then:  cd build-web/bin && python3 -m http.server 8000
#        open http://localhost:8000/   (WebGPU browser; HTTPS or localhost only)

# Windows → build-win/bin/doom.exe  (mingw-w64 cross-compile from macOS/Linux)
./build_win.sh
```

The shareware `doom1.wad` is already in `assets/`, so a fresh build is playable out
of the box (episode 1). For the full game, drop your own commercial IWAD in `assets/`
before building — whatever is there is packaged into the app / copied next to the
binary / preloaded into the web build.

---

## Game data

DOOM's levels, textures and sounds live in an **IWAD** file. The freely
distributable **shareware** `doom1.wad` (episode 1) **is included** in `assets/`, so
the game runs out of the box. For the full game, drop your own commercial IWAD into
`assets/` too. The game mode is picked from the filename:

| File | Game |
|------|------|
| `doom1.wad` | Shareware DOOM (episode 1 — freely distributable) |
| `doom.wad`  | Registered DOOM (episodes 1–3) |
| `doomu.wad` | The Ultimate DOOM (episodes 1–4) |
| `doom2.wad` | DOOM II |

The **shareware** `doom1.wad` is freely redistributable (e.g. `apt install
doom-wad-shareware`). The registered/Ultimate/DOOM II IWADs are **commercial** — buy
them (Steam, GOG, the re-releases) and keep them local; never commit them.

---

## Controls

### Keyboard & mouse (desktop)

| Action | Keys |
|--------|------|
| Forward / back | **↑** / **↓**, or **W** / **S** |
| Turn | **←** / **→**, or mouse |
| Look up / down | Mouse (GPU mouselook) |
| Strafe | **A** / **D**, or `,` / `.` (or hold **Alt** while turning) |
| Fire | **Ctrl**, or left mouse |
| Use / open | **Space** |
| Run | **Shift** |
| Weapons | **1**–**7** |
| Filter look (Modern ↔ Authentic+) | **F5** |
| Menu / back | **Esc** |
| Cheats | Just type them (e.g. `iddqd`, `idkfa`, `idclev31`) |

A gamepad is used automatically if one is connected (left stick move, right stick
look, triggers fire, etc.).

### Touch (mobile / tablet)

On a touch device the on-screen controls appear automatically:

| Region | Action |
|--------|--------|
| **Left disc** | Move — push where you want to go: up/down walk, left/right **turn** (so you walk toward your finger) |
| **Right half — drag** | Look around (turn + aim up/down) |
| **Right half — tap** | **Fire** (a tap fires; a swipe only looks) |
| **FIRE** button | Fire (hold for continuous). Also **respawns** when you're dead, and **continues** past the level-end / finale screens |
| **USE** button | Open doors / switches |
| **WPN** button | Next weapon (skips weapons you don't own) |
| **MENU** button | Open / close the menu |
| In a menu | Disc up/down = navigate, disc **left/right = adjust** sliders (sound, screen size), FIRE = select, MENU = back |

**Cheats on touch** — there's no keyboard, so a secret button combo opens one:

> Press **USE · USE · WPN · WPN · FIRE · FIRE · USE**

…and a text box pops up. Type a code (`iddqd`, `idkfa`, `idclip`, `idclev31`, …) and
tap **Send**.

Sensitivities and sizes are tunable via environment variables (see below).

---

## Web / PWA

The web build is an installable Progressive Web App:

- **Tap to play** — the start screen unlocks audio and goes fullscreen on the tap.
- **Install** — "Add to Home Screen" (iOS) / the install button (Android/desktop
  Chrome) launches it chromeless and fullscreen. Requires **HTTPS** (or `localhost`);
  WebGPU, the service worker and install all need a secure context.
- **Version** — a build id (`date-githash`) is shown bottom-right on the start screen
  and stamped onto every asset URL, so you can confirm the latest build loaded.

---

## Environment flags

Everything has sensible defaults; these override at launch (e.g.
`DOOM_WARP="1 1" ./doom`).

**Renderer:** `DOOM_GPU=0` (software renderer) · `DOOM_MODERN=1` / `DOOM_CRISP=1`
(start smooth / crisp) · `DOOM_SOLE=0` (legacy GPU-over-software) ·
`DOOM_BLOOM`, `DOOM_BLOOM_THRESH`, `DOOM_TONEMAP`, `DOOM_VIGNETTE` ·
`DOOM_COLORMAP=1` (banded Authentic+ lighting) · `DOOM_LIGHT_INTENSITY` ·
`DOOM_LIGHTTEST=1`.

**Audio:** `DOOM_SFX_GAIN` (default 1.6) · `DOOM_MUSIC_GAIN` (default 0.30).

**Touch:** `DOOM_TOUCH=1` (force controls on desktop for testing) ·
`DOOM_TOUCH_SCALE` (control size) · `DOOM_TOUCH_DISCTURN` (disc turn rate) ·
`DOOM_TOUCH_TURN` / `DOOM_TOUCH_LOOK` (right-drag look sensitivity).

**Gamepad:** `LUMI_NO_GAMEPAD=1` (skip gamepad init) · `DOOM_GP_TURN`, `DOOM_GP_LOOK`,
`DOOM_GP_MOVET`.

**Misc:** `DOOM_WARP="E M"` (jump to episode E, map M) · `DOOM_SHOT=1` (screenshot at
frame 180).

---

## Repository layout

```
src/                 host layer: window/input/audio/timing + the GPU render bridge
assets/shaders/      GLSL shaders (transpiled to SPIR-V / WGSL per backend)
web/                 PWA shell, manifest, service worker, icons, version stamper
vendor/doom/         id's linuxdoom-1.10 source (vendored, with 64-bit/port fixes)
CMakeLists.txt       Doom is vendored; Luminoveau is fetched via FetchContent
```

The Luminoveau engine is **not** vendored — CMake `FetchContent` pulls it at a pinned
upstream commit and uses it **unmodified**. (Earlier local engine fixes — touch
controls, the no-gamepad hatch, the Linux/CI shader transpile — have all been merged
upstream.)

---

## Credits & licences

- **DOOM** — created by **id Software**. The DOOM franchise and its game data are
  © id Software / ZeniMax Media, now part of **Bethesda / Microsoft**. Buy the games;
  this port ships none of their assets.
- **DOOM source code** — the `linuxdoom-1.10` release, © id Software, used under the
  **DOOM Source Code License** (the terms in the source headers). id later relicensed
  the DOOM source under the GNU GPL.
- **Luminoveau engine** — by **[bXi](https://github.com/bXi/luminoveau)**; see the
  engine's own `LICENSE.md`.
- **TinySoundFont** — MUS/MIDI synthesis, by Bernhard Schelling.
- **This port & GPU renderer** — the host layer, GPU renderer, touch controls and
  build tooling here.

DOOM® is a registered trademark of id Software LLC. This is an unofficial,
non-commercial fan port; it is not affiliated with or endorsed by id Software,
ZeniMax, Bethesda or Microsoft.
