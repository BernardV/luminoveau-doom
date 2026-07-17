
# Plan: vervang Doom's software-renderer door een GPU-renderer op Luminoveau

## Context

De huidige port draait Doom's originele software-renderer (BSP → kolom/span-rasterisatie
in een 8-bit 320×200 `screens[0]`) en blit dat als texture via Lumi. Doel: de **3D-wereld-
render volledig vervangen** door een GPU-renderer bovenop Lumi, met betere graphics.

Software-renderer kan principieel geen gefilterde/hi-res/gekleurd-belichte textures — dat
vereist de wereld op de GPU tekenen vanuit Doom's geometrie. Gekozen scope (uit vragen):
- **Beide stijlen, schakelbaar**: *Authentic+* (exacte palette+colormap-look, maar crisp/
  gefilterd/hi-res) als basis, *Modern* (RGBA + smooth licht) als toggle.
- **Alle verbeteringen**: filtering+mipmaps+hi-res, dynamisch/gekleurd licht, mouselook
  (pitch), post-processing (bloom/tonemap/grade).

Alleen de **3D-viewport** wordt vervangen. HUD, statusbar, menu's, automap, intermission,
finale, titel/demo en wipe blijven Doom's software-2D (via `screens[0]`), gecomposit óver
de GPU-3D. Realistisch: meerdere weken; daarom strak gefaseerd, elke fase los verifieerbaar.

Paden (repo is gereorganiseerd): engine `doomgame/vendor/lumi`, doom
`doomgame/vendor/doom/linuxdoom-1.10`, port-code `doomgame/src`.

## Architectuur

Nieuwe `DoomRenderPass : RenderPass` (Lumi-subklasse, **game-side** in `doomgame/src`,
model: `vendor/lumi/src/renderer/passes/model3drenderpass.{h,cpp}`), aangehangen via
`Renderer::AttachRenderPassToFrameBuffer` in een eigen framebuffer die vóór de bestaande
sprite/blit-pass draait. Rendert de wereld in een `D32_Float`-depth + HDR-kleurtarget;
daarna post-fx; daarna software-UI eroverheen.

**Geen engine-edits nodig**: de custom pass subklasst `vendor/lumi/src/gpu/renderpass.h`,
gebruikt `Renderer::GetGpu()` (`IGpu`), en de shaders worden in **GLSL** geschreven die de
engine automatisch naar elke backend compileert (zie Shaders-sectie).

### Bridge (C ↔ C++)
Doom is C, de renderer C++. Nieuw `doomgame/src/dg_render.c` (C, compileert mee met de
doom-headers) verzamelt geometrie/state in **platte C-structs** die de C++-pass leest —
zo lekken Doom-headers (`boolean`/`fixed_t`) niet in C++. Exposed via `dg_bridge.h`:
- level-load: `segs/numsegs`, `sides`, `sectors/numsectors`, `subsectors/numsubsectors`,
  `nodes`, `vertexes` (uit `r_state.h`); textures/flats/patches als RGBA + R8-index
  (via `R_GetColumn`, flats 64×64, `PLAYPAL`, `COLORMAP`).
- per-frame: `viewx/viewy/viewz` (fixed), `viewangle` (BAM) uit `r_main.c` (via
  `R_SetupFrame`); per-sector `floorheight/ceilingheight/lightlevel`; `texturetranslation`/
  `flattranslation` (animaties); alle `mobj_t` (thinker-lijst) → sprite-lijst; `player`
  `extralight`/`fixedcolormap`/`psprites`; `gamestate`/`automapactive`/viewwindow-rect.

### Skippen software-3D
`R_RenderPlayerView` (`r_main.c:870`) roept `R_SetupFrame` (zet `viewx/y/z/angle`) + BSP/
draw. We houden `R_SetupFrame` (nodig voor view-params) maar slaan BSP/draw over bij
GPU-mode: kleine edit in `r_main.c` met een `extern boolean use_gpu_renderer;`-guard.
De viewport-regio van `screens[0]` wordt dan naar een **sentinel-palette-index** gecleared
zodat de composite die als transparant behandelt.

### Compositing
- `GS_LEVEL` && niet-automap → GPU-3D in de viewport; software-`screens[0]` (HUD/statusbar/
  menu, viewport = sentinel-kleur) eroverheen via color-key in de blit-shader.
- automap / andere gamestates → geen GPU-3D; blit software fullscreen (huidig gedrag).

## Input — Lumi Input API i.p.v. eigen polling
Nu doet `main.cpp` eigen `SDL_GetKeyboardState`-polling (`PollKeyboard`). Vervang door
Lumi's Input-systeem (`vendor/lumi/src/platform/input/input.h`): keyboard, muis,
**controllers** en virtual controls, in één API. Doom blijft gevoed via `DG_KeyEvent`/
`DG_MouseEvent` (ev_keydown/keyup/ev_mouse), maar de bron wordt Lumi Input:
- **Keyboard**: `Input::KeyPressed(SDLK_x)`→`DG_KeyEvent(1,doomkey)`, `Input::KeyReleased`→
  `DG_KeyEvent(0,…)` (KeyDown/Pressed nemen een SDL-**keycode**, matcht `SdlKeyToDoom`).
- **Muis**: `Input::GetMouseDelta()` (relatieve beweging) → turn/mouselook; `Input::
  MouseButtonDown` → fire. Zet relative-mouse-mode aan via de Window-API
  (`Window::_setRelativeMouseMode` / publieke wrapper; `SDL_SetWindowRelativeMouseMode`).
- **Controller** (nieuw): auto-getrackt (engine handelt `SDL_EVENT_GAMEPAD_ADDED/REMOVED`
  in `window.cpp:196`). Lees `Input::GetGamepadAxisMovement(id,axis)` (deadzone al toegepast)
  + `Input::GamepadButtonDown/Pressed(id,btn)` en map naar Doom-acties (linkerstick →
  vooruit/strafe, rechterstick → turn/look, A→use, RT→fire, enz.), gesynthetiseerd als
  ev_keydown/ev_mouse zodat Doom's bestaande binding-logica het afhandelt.
- **Virtual controls** (touch) beschikbaar voor mobiel/web-touch later.
- **Flow-herordening**: Lumi Input wordt ververst in `Window::StartFrame` (`_handleInput`→
  `Input::Update`). Lees input dus **na** `StartFrame` en **vóór** `DG_Tic` (dat de Doom-
  eventqueue leegt in `TryRunTics`). AppIterate wordt: `StartFrame` → Lumi Input lezen →
  Doom voeden → `DG_Tic` → framebuffer uploaden/tekenen → `EndFrame`.

**Let op / verificatie (belangrijk):** Lumi's *keyboard*-state is event-gedreven
(`_updateInputs` uit `Window::ProcessEvent`), dezelfde `SDL_AppEvent`-route die eerder
onbetrouwbaar bleek (daarom het `SDL_GetKeyboardState`-polling). Muis + gamepad zijn níet
afhankelijk daarvan (mouse-motion-events komen wél binnen; gamepad wordt gepolld). **Eerste
taak van deze migratie: verifiëren dat `Input::KeyDown` post-bundle betrouwbaar toetsen
ziet.** Zo niet, dan de bron robuust maken (voorkeur: engine-Input laten vullen uit
`SDL_GetKeyboardState`, of een kleine lokale shim die `Input::currentKeyboardState` bijwerkt)
i.p.v. Lumi Input te omzeilen — zodat de rest (gamepad/mouse/virtual) via één API loopt.

## Shaders — GLSL, automatisch getranspileerd (dev-tip)
Alle shaders (vertex + fragment) in **GLSL** schrijven; de engine compileert ze automatisch
naar de doel-backend — **geen** HLSL/WGSL-authoring, geen per-backend baked `.cpp`-blobs,
geen handmatige dxc/spirv-cross/metal/tint:
- **Native** (mac/win/linux): `AssetHandler::GetShader("shaders/doom.vert")` → SDL_shadercross
  compileert GLSL→SPIRV→doel (METALLIB/SPIRV/DXIL) bij runtime; stage uit `.vert`/`.frag`.
  Levert `ShaderAsset.gpuShader` (een `GpuShaderHandle`) → direct in een eigen
  `createGraphicsPipeline` (custom vertex-layout + depth). Bevestigd: het pad ondersteunt
  expliciet de **vertex**-stage (`src/renderer/shaders.cpp:371`, `src/assets/sdl/assethandler.cpp:14`).
- **Web**: build-time transpile (`lumi_transpile_shaders`) globt `*.vert`/`*.frag` en zet
  GLSL→SPIR-V→WGSL (glslang+Tint); runtime laadt de WGSL. Werkt dus ook voor vertex-shaders.
- Shaders komen in `doomgame/assets/shaders/` (worden al gekopieerd/gepreload zoals andere
  assets). Vereist toolchain zit al in de engine-build (SDL_shadercross native /
  glslangValidator web).
- **Authentic+ vs Modern**: twee GLSL-fragmentvarianten (of één shader met een uniform-
  branch/#define). Authentic+: sample R8-index → `colormap[light][idx]` → `PLAYPAL[..]`
  (light uit sectorlicht+afstand, net als `zlight`/`scalelight`). Modern: RGBA + smooth
  falloff + dynamische lichten.

## Fasering (elk los te bouwen + verifiëren via screenshot)

- **Fase 0 — scaffolding.** `DoomRenderPass` skelet + één GLSL vert/frag via `GetShader` in
  een eigen pipeline: één driehoek via de pass op mac/web/win. Bewijst custom-pass +
  auto-getranspileerde GLSL over alle backends.
- **Fase 1 — muren (statisch).** Quads uit `segs` (1-zijdig mid; 2-zijdig upper/lower/mid),
  UV uit seg-offset/`textureoffset`/`rowoffset` + pegging-flags. Doom-textures → GPU
  (RGBA + R8). Camera uit `viewx/y/z/viewangle`, FOV 90°h, `perspectiveLH_ZO`, depthbuffer.
  Coördinaten: Doom (x,y,z-up) → engine LH. Verificatie: muren kloppen t.o.v. software.
- **Fase 2 — flats + sky.** Vloeren/plafonds: **subsector-convexe-polygonen reconstrueren
  via BSP-clipping** (of earcut per sector — zie risico's), textureren met flats (64×64) op
  sectorhoogte. Sky als cilinder/screen-space uit `skytexture` per view-yaw.
- **Fase 3 — dynamiek.** Sectorhoogtes (deuren/liften) per frame via **per-sector height-
  storagebuffer** die de vertex-shader leest (alleen kleine buffer update, geen re-mesh);
  animated textures via `texturetranslation`/`flattranslation` (binding-swap).
- **Fase 4 — sprites.** Billboards uit `mobj_t` (8 rotaties/flip zoals `R_ProjectSprite`),
  masked/alpha uit patch-posts; fuzz/translucency; wapensprites (`psprites`) als overlay.
- **Fase 5 — belichting + verbeteringen.** Authentic+ colormap-pad én Modern smooth-pad
  (toggle-toets); mipmaps + trilineair/aniso + hi-res (volledige vensterresolutie);
  dynamische/gekleurde puntlichten (wapenflits via `extralight`, projectielen/lava);
  mouselook (pitch in de GPU-camera) + turn, gevoed uit **Lumi Input** `GetMouseDelta`
  (relative-mouse-mode) en gamepad-sticks (zie Input-sectie).
- **Fase 6 — post-fx.** Bloom/tonemap/colorgrade via Lumi's effect-chain
  (`Draw::CreateEffect`/`ShaderRenderPass`, GLSL onder `assets/shaders/`) op het HDR-target.
- **Fase 7 — integratie/composite.** `use_gpu_renderer`-guard in `r_main.c`; color-key-
  composite van software-UI; conditioneel per gamestate; toggle software↔GPU; alle drie
  builds (mac/web/win) opnieuw draaien + verifiëren.

## Te maken / wijzigen bestanden
Nieuw: `doomgame/src/DoomRenderPass.{h,cpp}` (C++ pass), `doomgame/src/dg_render.c` (C-bridge/
geometrie-extractie), en **GLSL-shaders** in `doomgame/assets/shaders/` (`doom.vert`/`doom.frag`
voor Authentic+ en Modern, plus post-fx `.vert`/`.frag`). Geen HLSL/WGSL/baked-blobs.
Wijzigen: `doomgame/src/main.cpp` (pass aanmaken/attachen, per-frame updates, composite,
toggle, **input via Lumi Input i.p.v. `PollKeyboard`** + flow-herordening + relative-mouse),
`doomgame/src/dg_bridge.h` (nieuwe decls), `doomgame/CMakeLists.txt` (nieuwe sources; web-
transpile pikt de GLSL in `assets/shaders/` automatisch op), en klein
`doomgame/vendor/doom/linuxdoom-1.10/r_main.c` (`use_gpu_renderer`-guard om BSP/draw over te
slaan, `R_SetupFrame` behouden).
Herbruiken (input): `Input::KeyPressed/KeyReleased/GetMouseDelta/MouseButtonDown/
GetGamepadAxisMovement/GamepadButtonDown` (`vendor/lumi/src/platform/input/input.h`),
`Window::_setRelativeMouseMode`.
Herbruiken: `Renderer::AttachRenderPassToFrameBuffer`/`CreateFrameBuffer`/
`CreateSpriteRenderTarget`, `IGpu` buffer/pipeline/draw-API (`vendor/lumi/src/gpu/IGpu.h`),
`AssetHandler::BeginUploadBatch`/`_copy_to_texture` (honderden textures), `Camera3D`,
`GpuSamplerCreateInfo` (aniso/mip), `AssetHandler::GetShader` (GLSL→backend).

## Risico's (hoog → laag)
1. **Flat-polygon-reconstructie** (fase 2): subsectors missen impliciete BSP-split-randen;
   BSP-clip of earcut-per-sector nodig. Grootste onbekende; fallback earcut.
2. **Compositing** software-UI ↔ GPU-3D (color-key + gamestate-condities + wipe/automap).
3. **Dynamische sectorhoogtes** correct in de vertex-shader (deuren/liften/crushers).
4. Coördinaten/handedness (winding/culling) tussen Doom en engine-LH.
5. **Lumi keyboard-input**: event-gedreven via dezelfde SDL-callback-route die eerder
   onbetrouwbaar was. Moet als eerste geverifieerd; anders bron robuust maken (zie Input).

## Verificatie
Per fase: bouw `build_mac.sh` en vergelijk een screenshot (`DOOM_SHOT=1`) met de software-
render op een bekende plek (E1M1-start). Fase 5+: toggle Authentic+/Modern en check beide.
Eind: alle drie targets (`build_mac.sh`/`build_web.sh`/`build_win.sh`) bouwen; mac +
web (browser) interactief spelen; input/HUD/menu's blijven werken; software-fallback-toggle
werkt. Regressietest: bestaande SFX/muziek/keyboard ongewijzigd.
