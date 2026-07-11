// Plain-C bridge between the Doom core (C) and the Luminoveau host (C++).
// Deliberately contains NO Doom types so the C++ side never has to include
// doomdef.h/doomtype.h (whose `boolean`/`false`/`true` clash with C++ keywords).
#ifndef DG_BRIDGE_H
#define DG_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

// Doom's logical framebuffer size (320x200).
#define DG_WIDTH  320
#define DG_HEIGHT 200

// Fase 7 (GPU is the sole 3D renderer): the software renderer skips the 3D view
// and clears that region to this palette index; the host maps it to a fully
// transparent pixel so the GPU 3D shows through and the HUD/menu (drawn after,
// opaque) composites on top. The 3D view occupies the top DG_VIEW_H rows.
#define DG_SENTINEL_INDEX 0xFF
#define DG_VIEW_H         168

// ── Called by the host (main.cpp) ──────────────────────────────────────────

// Run all of Doom's one-time startup (WAD load, renderer init, start title/demo).
// Must be called once, after the window/renderer exist.
void DG_Init(int argc, char** argv);

// Advance the game by one host frame (one D_DoomTic).
void DG_Tic(void);

// Feed a keyboard edge to Doom. `pressed` = 1 for down, 0 for up.
// `doomKey` is a Doom keycode (see DG_KEY_* below).
void DG_KeyEvent(int pressed, int doomKey);

// Feed relative mouse motion + button bitmask (bit0=left, bit1=right, bit2=mid).
void DG_MouseEvent(int buttons, int dx, int dy);

// Pointer to the current 320x200 RGBA8888 framebuffer, refreshed by I_FinishUpdate.
const unsigned int* DG_GetFramebuffer(void);

// ── Implemented by the host, called from the Doom core (i_lumi.c) ───────────

// Monotonic milliseconds since start; used to derive Doom's 35Hz tic clock.
unsigned long DG_HostTicksMs(void);

// ── Audio ───────────────────────────────────────────────────────────────────

// Output sample rate of the PCM mixer (miniaudio resamples to the device).
#define DG_AUDIO_RATE 44100

// Doom's 8-channel SFX mixer, run on the audio thread by the host's PCM
// generator. Fills `frameCount` interleaved float frames of `channels` each.
// Matches Luminoveau's PCMGenerateCallback signature.
void DG_MixAudio(float* output, unsigned int frameCount,
                 unsigned int channels, void* userData);

// Music (MUS via GM soundfont, dg_music.c). DG_MusicInit loads the soundfont;
// DG_MusicMix is called by DG_MixAudio to add music to the SFX output.
void DG_MusicInit(void);
void DG_MusicMix(float* output, unsigned int frameCount, unsigned int channels);

// ── GPU renderer geometry/view bridge (dg_render.c, plan.md Fase 1) ─────────

// True once a level is loaded and its geometry is available.
int DG_WorldReady(void);

// A fullscreen software UI is active (menu / pause / automap / non-level state).
// Host: show the software render (not GPU 3D) and release the mouse.
int DG_UIActive(void);

// GPU-sole-renderer mode (Fase 7): the software 3D render is skipped and the GPU
// is the only 3D renderer, with the software HUD/menu composited on top.
void DG_SetSoleRenderer(int on);   // host enables/disables
int  DG_SoleRenderer(void);        // current state

// True when a 3D level view should be drawn by the GPU: in a level and not in the
// automap. Unlike DG_UIActive this stays true while an in-game menu is up, so the
// menu gets the GPU 3D as its background (sole mode).
int DG_Show3D(void);

// Wall geometry as interleaved floats: {x,y,z, u,v, shade} per vertex, triangles
// (3 verts each), engine space, grouped by texture. *outFloatCount = total floats
// (verts*6). *outVersion bumps on rebuild (level change) so the host re-uploads
// only when it changes. NULL/0 when no level.
const float* DG_WorldVertices(int* outFloatCount, unsigned* outVersion);

// Draw groups: contiguous vertex ranges sharing one (kind, texid). kind: 0=wall
// (texture via DG_WallTextureRGBA), 1=flat floor/ceiling (via DG_FlatTextureRGBA).
enum { DG_KIND_WALL = 0, DG_KIND_FLAT = 1 };
int  DG_DrawGroupCount(void);
void DG_DrawGroup(int i, int* kind, int* texid, int* firstVert, int* vertCount);

// Wall texture (composite) as RGBA8 via PLAYPAL, cached. NULL/0 if invalid.
const unsigned char* DG_WallTextureRGBA(int texid, int* w, int* h);

// Flat (floor/ceiling) as RGBA8 via PLAYPAL, cached. Always 64x64. NULL if invalid.
const unsigned char* DG_FlatTextureRGBA(int picnum, int* w, int* h);

// Current sky texture id (a composite texture id; fetch pixels via DG_WallTextureRGBA).
int DG_SkyTextureId(void);

// ── Sprites (things) ─────────────────────────────────────────────────────────
// DG_SpriteCount rebuilds the per-frame visible-thing list and returns its size.
// DG_Sprite fills out8 = {x, y_feet, z, halfWidth, topY, lump, flip, shade} in
// engine space. DG_SpriteTextureRGBA gives the patch as RGBA8 with alpha (0 in
// transparent gaps), cached by lump.
int  DG_SpriteCount(void);
void DG_Sprite(int i, float* out8);
const unsigned char* DG_SpriteTextureRGBA(int lump, int* w, int* h);

// Player weapon sprite (idx 0=weapon, 1=flash). Returns 1 if active; fills
// out7 = {lump, xLeft, yTop, w, h (all in Doom 320x200 px), flip, fullbright}.
int DG_WeaponSprite(int idx, float* out7);

// Current camera: pos3 = engine-space eye {x, y_up, z}; yaw in radians (Doom
// angle, CCW from +X); pitch in radians (renderer-only mouselook).
void DG_GetView(float* pos3, float* yawRad, float* pitchRad);

// Set the renderer-only pitch (look up/down), radians, + = up. Doom has no pitch.
void DG_SetPitch(float pitchRad);

// Muzzle-flash / light-amp brightness (0..1). Applied by the world/sprite frag
// shaders as a localized point light at the eye with distance falloff.
float DG_FlashLevel(void);

// ── Dynamic colored point lights ────────────────────────────────────────────
// Per-frame lights from projectiles (colored by type) + fullbright decorations.
// DG_LightCount rebuilds the nearest-N list; DG_Light fills out7 = {x,y,z (engine
// space), r,g,b (colour), radius}. The world/sprite shaders add a colored,
// distance-attenuated contribution.
#define DG_MAX_LIGHTS 16
int  DG_LightCount(void);
void DG_Light(int i, float* out7);

// ── HUD message (drawn by the GPU overlay, over the 3D) ─────────────────────
// Current top-of-screen message text ("" if none). Advance width for a space
// or non-drawable char when laying out DG_FontGlyph glyphs.
#define DG_FONT_SPACE 4
const char* DG_HudMessage(void);
// Font glyph for a char: 1 + fills *lump/*w/*h (RGBA via DG_SpriteTextureRGBA),
// or 0 for space/non-printable. Doom's HUD font, drawn at 320x200 scale.
int DG_FontGlyph(int ch, int* lump, int* w, int* h);

// ── Doom keycodes (mirror doomdef.h; kept here so C++ needn't include it) ───
#define DG_KEY_RIGHTARROW 0xae
#define DG_KEY_LEFTARROW  0xac
#define DG_KEY_UPARROW    0xad
#define DG_KEY_DOWNARROW  0xaf
#define DG_KEY_ESCAPE     27
#define DG_KEY_ENTER      13
#define DG_KEY_TAB        9
#define DG_KEY_F1         (0x80+0x3b)
#define DG_KEY_F2         (0x80+0x3c)
#define DG_KEY_F3         (0x80+0x3d)
#define DG_KEY_F4         (0x80+0x3e)
#define DG_KEY_F5         (0x80+0x3f)
#define DG_KEY_F6         (0x80+0x40)
#define DG_KEY_F7         (0x80+0x41)
#define DG_KEY_F8         (0x80+0x42)
#define DG_KEY_F9         (0x80+0x43)
#define DG_KEY_F10        (0x80+0x44)
#define DG_KEY_F11        (0x80+0x57)
#define DG_KEY_F12        (0x80+0x58)
#define DG_KEY_BACKSPACE  127
#define DG_KEY_PAUSE      0xff
#define DG_KEY_EQUALS     0x3d
#define DG_KEY_MINUS      0x2d
#define DG_KEY_RSHIFT     (0x80+0x36)
#define DG_KEY_RCTRL      (0x80+0x1d)
#define DG_KEY_RALT       (0x80+0x38)

#ifdef __cplusplus
}
#endif

#endif // DG_BRIDGE_H
