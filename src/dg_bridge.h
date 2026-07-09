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

// Returns the wall geometry as interleaved floats: {x,y,z,shade} per vertex,
// triangles (3 verts each), in engine space. *outFloatCount = total floats
// (verts*4). *outVersion bumps whenever the geometry is rebuilt (level change),
// so the host can re-upload only when it changes. NULL/0 when no level.
const float* DG_WorldVertices(int* outFloatCount, unsigned* outVersion);

// Current camera: pos3 = engine-space eye {x, y_up, z}; yaw in radians (Doom
// angle, CCW from +X); pitch in radians (0 until mouselook, Fase 5).
void DG_GetView(float* pos3, float* yawRad, float* pitchRad);

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
