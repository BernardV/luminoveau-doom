// i_lumi.c — Luminoveau platform layer for linuxdoom-1.10.
//
// Replaces the original i_video.c / i_system.c / i_sound.c / i_net.c / i_main.c.
// Compiled as C alongside the Doom core; talks to the C++ host through dg_bridge.h.
//
// Responsibilities:
//   * video   — palette-index 320x200 screens[0]  ->  RGBA8888 framebuffer
//   * system  — timing, zone base, I_Error/I_Quit
//   * sound    — stubbed (silent) for this port
//   * net      — single-player stub

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "doomdef.h"
#include "doomtype.h"
#include "d_event.h"
#include "d_main.h"
#include "i_system.h"
#include "i_video.h"
#include "i_sound.h"
#include "i_net.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_net.h"
#include "doomstat.h"

#include "dg_bridge.h"

// ── Video ───────────────────────────────────────────────────────────────────

static unsigned int g_framebuffer[DG_WIDTH * DG_HEIGHT];
static unsigned int g_palette[256];   // pre-multiplied by the active gamma table

const unsigned int* DG_GetFramebuffer(void)
{
    return g_framebuffer;
}

void I_InitGraphics(void)
{
    // screens[0] is already a 320x200 buffer allocated by V_Init(); nothing to do.
    // A default (identity-ish) palette until Doom uploads PLAYPAL.
    int i;
    for (i = 0; i < 256; i++)
        g_palette[i] = 0xff000000u;
}

void I_ShutdownGraphics(void) {}

void I_SetPalette(byte* palette)
{
    int i;
    for (i = 0; i < 256; i++)
    {
        unsigned int r = gammatable[usegamma][*palette++];
        unsigned int g = gammatable[usegamma][*palette++];
        unsigned int b = gammatable[usegamma][*palette++];
        // R8G8B8A8_Unorm, little-endian: bytes R,G,B,A -> 0xAABBGGRR
        g_palette[i] = r | (g << 8) | (b << 16) | (0xffu << 24);
    }
}

void I_UpdateNoBlit(void) {}

void I_FinishUpdate(void)
{
    int i;
    const byte* src = screens[0];
    for (i = 0; i < DG_WIDTH * DG_HEIGHT; i++)
        g_framebuffer[i] = g_palette[src[i]];
}

void I_ReadScreen(byte* scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

void I_StartFrame(void) {}

// Input is pushed from the host via DG_KeyEvent/DG_MouseEvent, so there is
// nothing to poll here.
void I_StartTic(void) {}

// ── Input bridge ──────────────────────────────────────────────────────────────

void DG_KeyEvent(int pressed, int doomKey)
{
    event_t ev;
    ev.type  = pressed ? ev_keydown : ev_keyup;
    ev.data1 = doomKey;
    ev.data2 = ev.data3 = 0;
    D_PostEvent(&ev);
}

void DG_MouseEvent(int buttons, int dx, int dy)
{
    event_t ev;
    ev.type  = ev_mouse;
    ev.data1 = buttons;
    ev.data2 = dx << 2;
    ev.data3 = (-dy) << 2;
    D_PostEvent(&ev);
}

// ── System ────────────────────────────────────────────────────────────────────

int mb_used = 16;   // zone heap size in MB (bumped from the original 6MB)

void I_Tactile(int on, int off, int total) { (void)on; (void)off; (void)total; }

static ticcmd_t emptycmd;
ticcmd_t* I_BaseTiccmd(void) { return &emptycmd; }

int I_GetHeapSize(void) { return mb_used * 1024 * 1024; }

byte* I_ZoneBase(int* size)
{
    *size = mb_used * 1024 * 1024;
    return (byte*)malloc(*size);
}

int I_GetTime(void)
{
    // Doom wants 1/TICRATE-second tics; DG_HostTicksMs() is a monotonic ms clock.
    return (int)((DG_HostTicksMs() * TICRATE) / 1000UL);
}

void I_Init(void)
{
    I_InitSound();
    DG_MusicInit();   // load the GM soundfont for MUS playback
}

void I_Quit(void)
{
    D_QuitNetGame();
    M_SaveDefaults();
    I_ShutdownGraphics();
    exit(0);
}

void I_WaitVBL(int count) { (void)count; }
void I_BeginRead(void) {}
void I_EndRead(void) {}

byte* I_AllocLow(int length)
{
    byte* mem = (byte*)malloc(length);
    memset(mem, 0, length);
    return mem;
}

void I_Error(char* error, ...)
{
    va_list argptr;
    va_start(argptr, error);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, error, argptr);
    fprintf(stderr, "\n");
    va_end(argptr);
    fflush(stderr);
    D_QuitNetGame();
    I_ShutdownGraphics();
    exit(-1);
}

// ── Sound effects ────────────────────────────────────────────────────────────
//
// Doom's SFX are DMX "DS" lumps: 8-bit unsigned mono PCM at 11025 Hz with an
// 8-byte header. We precache them (getsfx, as in the original) and mix up to 8
// channels ourselves — but on the audio thread, via DG_MixAudio, which the host
// drives through Luminoveau's PCM generator. Each channel resamples 11025 Hz ->
// DG_AUDIO_RATE and applies per-channel left/right volume.

#include <math.h>
#include "sounds.h"
#include "z_zone.h"
#include "w_wad.h"   // W_CacheLumpNum returns void* — without this it's an
                     // implicit int, truncating the pointer on 64-bit (crash).

#define DG_SFX_CHANNELS 8
#define DG_SFXRATE      11025
#define SAMPLECOUNT     512

typedef struct {
    volatile int         active;    // written last when starting; read on audio thread
    const unsigned char* data;      // 8-bit unsigned PCM
    int                  length;    // sample count
    double               pos;       // playback cursor (input samples) — audio thread
    double               rate;      // input samples advanced per output frame
    float                lvol, rvol;// 0..1
    int                  handle;
    int                  sfxid;
    int                  start;     // gametic when started (oldest = lowest priority)
} dg_chan_t;

static dg_chan_t g_chan[DG_SFX_CHANNELS];
static int       g_lengths[NUMSFX];
static int       g_handleCounter = 0;

// Load one SFX lump into zone memory, padded to a mixing-buffer multiple.
static void* getsfx(char* sfxname, int* len)
{
    unsigned char* sfx;
    unsigned char* paddedsfx;
    int i, size, paddedsize, sfxlump;
    char name[20];

    sprintf(name, "ds%s", sfxname);
    if (W_CheckNumForName(name) == -1)
        sfxlump = W_GetNumForName("dspistol");
    else
        sfxlump = W_GetNumForName(name);

    size = W_LumpLength(sfxlump);
    sfx = (unsigned char*)W_CacheLumpNum(sfxlump, PU_STATIC);

    paddedsize = ((size - 8 + (SAMPLECOUNT - 1)) / SAMPLECOUNT) * SAMPLECOUNT;
    paddedsfx = (unsigned char*)Z_Malloc(paddedsize + 8, PU_STATIC, 0);
    memcpy(paddedsfx, sfx, size);
    for (i = size; i < paddedsize + 8; i++)
        paddedsfx[i] = 128;   // 8-bit unsigned silence

    Z_Free(sfx);
    *len = paddedsize;
    return (void*)(paddedsfx + 8);   // skip the 8-byte header
}

void I_InitSound(void)
{
    int i;
    for (i = 1; i < NUMSFX; i++) {
        if (!S_sfx[i].link) {
            S_sfx[i].data = getsfx(S_sfx[i].name, &g_lengths[i]);
        } else {
            S_sfx[i].data = S_sfx[i].link->data;
            g_lengths[i]  = g_lengths[S_sfx[i].link - S_sfx];
        }
    }
}

void I_UpdateSound(void) {}   // mixing happens on the audio thread (DG_MixAudio)
void I_SubmitSound(void) {}
void I_ShutdownSound(void) {}
void I_SetChannels(void) {}
void I_SetSfxVolume(int volume) { snd_SfxVolume = volume; }

int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    int i, slot = -1, oldest = 0x7fffffff, oldestslot = 0;
    int sepp, leftvol, rightvol;
    double pf;
    (void)priority;

    if (id <= 0 || id >= NUMSFX || !S_sfx[id].data) return -1;

    for (i = 0; i < DG_SFX_CHANNELS; i++) {
        if (!g_chan[i].active) { slot = i; break; }
        if (g_chan[i].start < oldest) { oldest = g_chan[i].start; oldestslot = i; }
    }
    if (slot < 0) slot = oldestslot;   // steal the oldest channel

    // Doom's stereo separation formula (0=left .. 255=right), volumes in 0..127.
    sepp     = sep + 1;
    leftvol  = vol - ((vol * sepp * sepp) >> 16);
    sepp     = sepp - 257;
    rightvol = vol - ((vol * sepp * sepp) >> 16);
    if (leftvol  < 0) leftvol  = 0; if (leftvol  > 127) leftvol  = 127;
    if (rightvol < 0) rightvol = 0; if (rightvol > 127) rightvol = 127;

    pf = pow(2.0, (pitch - 128) / 64.0);   // Doom pitch: 128 = normal

    g_chan[slot].active = 0;                // stop the audio thread using stale data
    g_chan[slot].data   = (const unsigned char*)S_sfx[id].data;
    g_chan[slot].length = g_lengths[id];
    g_chan[slot].pos    = 0.0;
    g_chan[slot].rate   = ((double)DG_SFXRATE / DG_AUDIO_RATE) * pf;
    g_chan[slot].lvol   = leftvol  / 127.0f;
    g_chan[slot].rvol   = rightvol / 127.0f;
    g_chan[slot].sfxid  = id;
    g_chan[slot].start  = gametic;
    g_chan[slot].handle = ++g_handleCounter;
    g_chan[slot].active = 1;                // publish
    return g_chan[slot].handle;
}

void I_StopSound(int handle)
{
    int i;
    for (i = 0; i < DG_SFX_CHANNELS; i++)
        if (g_chan[i].active && g_chan[i].handle == handle) g_chan[i].active = 0;
}

int I_SoundIsPlaying(int handle)
{
    int i;
    for (i = 0; i < DG_SFX_CHANNELS; i++)
        if (g_chan[i].active && g_chan[i].handle == handle) return 1;
    return 0;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{ (void)handle; (void)vol; (void)sep; (void)pitch; }

// Audio-thread mixer. Sums the active channels, resampling each from 11025 Hz.
void DG_MixAudio(float* out, unsigned int frames, unsigned int channels, void* user)
{
    unsigned int f, ch;
    int c;
    (void)user;

    for (f = 0; f < frames; f++) {
        float l = 0.0f, r = 0.0f;
        for (c = 0; c < DG_SFX_CHANNELS; c++) {
            int idx;
            float s;
            if (!g_chan[c].active) continue;
            idx = (int)g_chan[c].pos;
            if (idx >= g_chan[c].length) { g_chan[c].active = 0; continue; }
            s = ((float)g_chan[c].data[idx] - 128.0f) * (1.0f / 128.0f);
            l += s * g_chan[c].lvol;
            r += s * g_chan[c].rvol;
            g_chan[c].pos += g_chan[c].rate;
        }
        l *= 0.5f; r *= 0.5f;   // headroom for up to 8 summed channels
        if (l >  1.0f) l =  1.0f; if (l < -1.0f) l = -1.0f;
        if (r >  1.0f) r =  1.0f; if (r < -1.0f) r = -1.0f;

        if (channels >= 2) {
            out[f * channels + 0] = l;
            out[f * channels + 1] = r;
            for (ch = 2; ch < channels; ch++) out[f * channels + ch] = 0.0f;
        } else {
            out[f] = (l + r) * 0.5f;
        }
    }

    DG_MusicMix(out, frames, channels);   // mix music on top of the SFX
}

// Music (MUS via GM soundfont) lives in dg_music.c: I_InitMusic, I_ShutdownMusic,
// I_SetMusicVolume, I_RegisterSong, I_PlaySong, I_StopSong, I_PauseSong,
// I_ResumeSong, I_UnRegisterSong, I_QrySongPlaying.

// ── Networking (single-player stub) ─────────────────────────────────────────

void I_InitNetwork(void)
{
    doomcom = malloc(sizeof(*doomcom));
    memset(doomcom, 0, sizeof(*doomcom));

    netgame = false;
    doomcom->id = DOOMCOM_ID;
    doomcom->numplayers = doomcom->numnodes = 1;
    doomcom->deathmatch = false;
    doomcom->consoleplayer = 0;
    doomcom->ticdup = 1;
    doomcom->extratics = 0;
}

void I_NetCmd(void) {}

// ── Top-level bridge ────────────────────────────────────────────────────────

void DG_Init(int argc, char** argv)
{
    myargc = argc;
    myargv = argv;
    D_DoomMain();   // returns after one-time init (loop is driven by DG_Tic)
}

void DG_Tic(void)
{
    D_DoomTic();
}
