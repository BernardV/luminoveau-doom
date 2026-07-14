// dg_music.c — Doom MUS music playback via TinySoundFont (General MIDI).
//
// linuxdoom left all I_*Music/I_*Song as stubs. Here we parse Doom's MUS lumps
// and synthesize them with a GM soundfont (assets/soundfont.sf2) using tsf.
// Sequencing + all tsf calls happen on the audio thread (DG_MusicMix, invoked
// from DG_MixAudio); the game thread only publishes song-change requests.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TSF_IMPLEMENTATION
#include "extern/tsf.h"

#include "dg_bridge.h"

extern int snd_MusicVolume;   // doomstat (0..15)

// ── Shared state ─────────────────────────────────────────────────────────────
static tsf* g_tsf = NULL;

// Requests: game thread writes, audio thread consumes (gen bumped last).
static volatile const unsigned char* g_reqScore = NULL;
static volatile const unsigned char* g_reqEnd   = NULL;
static volatile int      g_reqLoop = 0;
static volatile unsigned  g_reqGen  = 0;
static volatile int      g_paused   = 0;
static volatile float    g_musVol   = 1.0f;

// Set by I_RegisterSong, latched by I_PlaySong.
static const unsigned char* g_regScore = NULL;
static const unsigned char* g_regEnd   = NULL;

// Audio-thread-only playback state.
static unsigned            g_ackGen = 0;
static const unsigned char* g_pos = NULL;
static const unsigned char* g_scoreStart = NULL;
static const unsigned char* g_end = NULL;
static int                 g_playing = 0;
static int                 g_loop = 0;
static double              g_delay = 0.0;   // samples until next event
static int                 g_chanVol[16];

#define MUS_TICKRATE 140                    // MUS delays are 1/140 s

// ── Init / shutdown (game thread, before playback) ───────────────────────────
// Rock remap: force every melodic channel to Distortion Guitar (GM program 30) and
// the drum channel (MUS ch 15) to the Power kit (GM drum kit 16, fall back to the
// Standard kit if the soundfont lacks it). On by default; tied to the GPU renderer —
// the classic software mode (DOOM_GPU=0) keeps the original General-MIDI voices.
static int g_musicRock = 1;

static void dg_set_preset(int chan, int val)
{
    if (g_musicRock) {
        if (chan == 15) {
            if (!tsf_channel_set_presetnumber(g_tsf, chan, 16, 1))
                tsf_channel_set_presetnumber(g_tsf, chan, 0, 1);
        } else {
            if (!tsf_channel_set_presetnumber(g_tsf, chan, 30, 0))
                tsf_channel_set_presetnumber(g_tsf, chan, val, 0);
        }
    } else {
        tsf_channel_set_presetnumber(g_tsf, chan, val, chan == 15);
    }
}

void DG_MusicInit(void)
{
    const char* dir = getenv("DOOMWADDIR");
    const char* gpuEnv = getenv("DOOM_GPU");
    char path[1024];
    int i;
    // Match main.cpp's GPU test exactly: on unless DOOM_GPU is exactly "0".
    g_musicRock = !(gpuEnv && gpuEnv[0] == '0' && gpuEnv[1] == '\0');
    snprintf(path, sizeof(path), "%s/soundfont.sf2", dir ? dir : "assets");
    g_tsf = tsf_load_filename(path);
    if (!g_tsf) {
        fprintf(stderr, "Music: could not load soundfont '%s' — music disabled\n", path);
        return;
    }
    tsf_set_output(g_tsf, TSF_STEREO_INTERLEAVED, DG_AUDIO_RATE, 0.0f);
    for (i = 0; i < 16; i++) {
        g_chanVol[i] = 127;
        dg_set_preset(i, 0);            // rock remap (or original preset 0 when off)
    }
    fprintf(stderr, "Music: soundfont loaded (%s)%s\n", path,
            g_musicRock ? " [rock instruments]" : "");
}

void I_InitMusic(void) {}          // real init is DG_MusicInit (called from I_Init)
void I_ShutdownMusic(void) {}

void I_SetMusicVolume(int volume)
{
    float g;
    snd_MusicVolume = volume;
    g = volume / 15.0f;            // Doom passes 0..15
    if (g > 1.0f) g = 1.0f;
    if (g < 0.0f) g = 0.0f;
    g_musVol = g;
}

// ── Song control (game thread) ───────────────────────────────────────────────
int I_RegisterSong(void* data)
{
    const unsigned char* d = (const unsigned char*)data;
    int scoreStart, scoreLen;
    if (!d || memcmp(d, "MUS\x1a", 4) != 0) { g_regScore = g_regEnd = NULL; return 0; }
    scoreLen   = d[4] | (d[5] << 8);
    scoreStart = d[6] | (d[7] << 8);
    g_regScore = d + scoreStart;
    g_regEnd   = d + scoreStart + scoreLen;
    return 1;
}

void I_PlaySong(int handle, int looping)
{
    (void)handle;
    g_reqScore = g_regScore;
    g_reqEnd   = g_regEnd;
    g_reqLoop  = looping;
    g_paused   = 0;
    g_reqGen++;                    // publish last
}

void I_StopSong(int handle)   { (void)handle; g_reqScore = NULL; g_reqEnd = NULL; g_reqGen++; }
void I_UnRegisterSong(int h)  { (void)h; }
void I_PauseSong(int h)        { (void)h; g_paused = 1; }
void I_ResumeSong(int h)       { (void)h; g_paused = 0; }
int  I_QrySongPlaying(int h)  { (void)h; return g_playing; }

// ── Sequencer + synth (audio thread) ─────────────────────────────────────────

// Apply MUS events until one carries a delay (or the score ends). Sets g_delay.
static void advanceEvents(void)
{
    int guard = 0;
    for (;;) {
        unsigned char ev, b;
        int last, type, chan;

        if (++guard > 100000) { g_playing = 0; g_delay = 1e18; return; }

        if (!g_pos || g_pos >= g_end) {           // end of score
            if (g_loop && g_scoreStart) { g_pos = g_scoreStart; continue; }
            g_playing = 0; tsf_note_off_all(g_tsf); g_delay = 1e18; return;
        }

        ev   = *g_pos++;
        last = ev & 0x80;
        type = (ev >> 4) & 7;
        chan = ev & 0x0f;

        switch (type) {
            case 0:  // release note
                tsf_channel_note_off(g_tsf, chan, (*g_pos++) & 0x7f);
                break;
            case 1: { // play note (optional volume byte)
                int note, vel;
                b = *g_pos++;
                note = b & 0x7f;
                if (b & 0x80) { g_chanVol[chan] = (*g_pos++) & 0x7f; }
                vel = g_chanVol[chan];
                tsf_channel_note_on(g_tsf, chan, note, vel / 127.0f);
                break;
            }
            case 2:  // pitch bend (0..255, 128=centre) -> 14-bit
                tsf_channel_set_pitchwheel(g_tsf, chan, (*g_pos++) * 64);
                break;
            case 3: { // system event
                int c = (*g_pos++) & 0x7f;
                if (c == 10 || c == 11) tsf_channel_note_off_all(g_tsf, chan);
                break;
            }
            case 4: { // change controller
                int ctrl = (*g_pos++) & 0x7f;
                int val  = (*g_pos++) & 0x7f;
                switch (ctrl) {
                    case 0: dg_set_preset(chan, val); break;          // program change (rock-remapped)
                    case 1: break;                                    // bank
                    case 2: tsf_channel_midi_control(g_tsf, chan, 1,  val); break; // modulation
                    case 3: tsf_channel_midi_control(g_tsf, chan, 7,  val); break; // volume
                    case 4: tsf_channel_midi_control(g_tsf, chan, 10, val); break; // pan
                    case 5: tsf_channel_midi_control(g_tsf, chan, 11, val); break; // expression
                    case 6: tsf_channel_midi_control(g_tsf, chan, 91, val); break; // reverb
                    case 7: tsf_channel_midi_control(g_tsf, chan, 93, val); break; // chorus
                    case 8: tsf_channel_midi_control(g_tsf, chan, 64, val); break; // sustain
                    case 9: tsf_channel_midi_control(g_tsf, chan, 67, val); break; // soft
                }
                break;
            }
            case 5:  // (unused in practice)
                break;
            case 6:  // score end
                if (g_loop && g_scoreStart) { g_pos = g_scoreStart; continue; }
                g_playing = 0; tsf_note_off_all(g_tsf); g_delay = 1e18; return;
            case 7:  // unknown
                break;
        }

        if (last) {                                // variable-length delay (ticks)
            unsigned delay = 0;
            do { b = *g_pos++; delay = (delay << 7) | (b & 0x7f); } while (b & 0x80);
            g_delay = (double)delay * (double)DG_AUDIO_RATE / (double)MUS_TICKRATE;
            return;
        }
    }
}

void DG_MusicMix(float* out, unsigned int frames, unsigned int channels)
{
    static float tmp[2048 * 2];
    unsigned int done = 0;
    float vol;

    if (!g_tsf) return;

    // Master music gain. The soundfont renders at unity (0 dB) and a full MUS
    // arrangement is very hot — far louder per volume-step than the SFX mixer — so
    // attenuate it hard to balance against SFX. Tunable: DOOM_MUSIC_GAIN.
    static float musMaster = -1.0f;
    if (musMaster < 0.0f) {
        const char* e = getenv("DOOM_MUSIC_GAIN");
        musMaster = e ? (float)atof(e) : 0.30f;
        if (musMaster < 0.0f) musMaster = 0.0f;
    }

    // Adopt a pending song change.
    if (g_reqGen != g_ackGen) {
        g_ackGen = g_reqGen;
        tsf_note_off_all(g_tsf);
        g_scoreStart = (const unsigned char*)g_reqScore;
        g_end        = (const unsigned char*)g_reqEnd;
        g_pos        = g_scoreStart;
        g_loop       = g_reqLoop;
        g_delay      = 0.0;
        g_playing    = (g_scoreStart != NULL);
    }

    vol = g_musVol * musMaster;

    while (done < frames) {
        unsigned int chunk = frames - done;
        unsigned int i;

        if (g_playing && !g_paused) {
            while (g_delay < 1.0 && g_playing) advanceEvents();
            if ((double)chunk > g_delay) chunk = (unsigned int)g_delay;
        }
        if (chunk == 0) chunk = 1;
        if (chunk > 2048) chunk = 2048;

        tsf_render_float(g_tsf, tmp, (int)chunk, 0);   // render (not mixing)

        for (i = 0; i < chunk; i++) {
            float l = tmp[i * 2 + 0] * vol;
            float r = tmp[i * 2 + 1] * vol;
            if (channels >= 2) {
                float ol = out[(done + i) * channels + 0] + l;
                float orr= out[(done + i) * channels + 1] + r;
                if (ol >  1.0f) ol =  1.0f; if (ol < -1.0f) ol = -1.0f;
                if (orr >  1.0f) orr =  1.0f; if (orr < -1.0f) orr = -1.0f;
                out[(done + i) * channels + 0] = ol;
                out[(done + i) * channels + 1] = orr;
            } else {
                float o = out[done + i] + (l + r) * 0.5f;
                if (o > 1.0f) o = 1.0f; if (o < -1.0f) o = -1.0f;
                out[done + i] = o;
            }
        }

        if (g_playing && !g_paused) g_delay -= chunk;
        done += chunk;
    }
}
