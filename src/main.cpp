// main.cpp — Luminoveau host for DOOM.
//
// Owns the window and the frame loop. Each frame it advances Doom one tic
// (DG_Tic), streams Doom's 320x200 framebuffer into a GPU texture, and blits it
// to the window (letterboxed to 4:3). Keyboard/mouse events are translated to
// Doom keycodes and pushed into the Doom event queue.

#include "luminoveau.h"
#include "app/lumi.h"

#include <SDL3/SDL.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include "dg_bridge.h"
}

#include "DoomRenderPass.h"

// setenv() doesn't exist in the Windows CRT; use _putenv_s there.
static void Dg_SetEnv(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

// ── Streaming framebuffer texture ───────────────────────────────────────────

static TextureAsset g_screen;   // 320x200 RGBA8, re-uploaded every frame

static void CreateScreenTexture()
{
    auto& gpu = Renderer::GetGpu();
    GpuTextureCreateInfo info{};
    info.width       = DG_WIDTH;
    info.height      = DG_HEIGHT;
    info.depthOrLayers = 1;
    info.numLevels   = 1;
    info.format      = GpuTextureFormat::R8G8B8A8_Unorm;
    info.sampleCount = GpuSampleCount::x1;
    info.usage       = GpuTextureUsage::Sampler | GpuTextureUsage::Transfer;

    g_screen.gpuTexture = gpu.createTexture(info);
    g_screen.gpuSampler = Renderer::GetSampler(ScaleMode::Nearest);
    g_screen.width  = DG_WIDTH;
    g_screen.height = DG_HEIGHT;
    g_screen.filename = "doom_screen";
}

// Scaling filter for the final blit. Nearest = crisp/authentic pixels;
// Linear = smoothed upscale. Toggled at runtime with the ` (grave) key.
static bool g_linearFilter = false;
static void ApplyFilter()
{
    g_screen.gpuSampler =
        Renderer::GetSampler(g_linearFilter ? ScaleMode::Linear : ScaleMode::Nearest);
}

static void UploadScreen(const unsigned int* rgba)
{
    auto& gpu = Renderer::GetGpu();
    const uint32_t bytes = DG_WIDTH * DG_HEIGHT * 4u;

    GpuTransferBufferCreateInfo tbInfo{};
    tbInfo.size  = bytes;
    tbInfo.usage = GpuTransferUsage::Upload;
    GpuTransferBufferHandle tb = gpu.createTransferBuffer(tbInfo);
    if (!tb) return;

    void* ptr = gpu.mapTransferBuffer(tb, false);
    if (!ptr) { gpu.releaseTransferBuffer(tb); return; }
    std::memcpy(ptr, rgba, bytes);
    gpu.unmapTransferBuffer(tb);

    GpuCmdBufferHandle cmd = gpu.acquireCommandBuffer();
    if (!cmd) { gpu.releaseTransferBuffer(tb); return; }

    GpuTransferBufferRegion src{};
    src.transferBuffer = tb;
    src.offset = 0;
    GpuTextureRegion dst{};
    dst.texture = g_screen.gpuTexture;
    dst.width = DG_WIDTH; dst.height = DG_HEIGHT; dst.depth = 1;

    gpu.uploadToTexture(cmd, src, dst, false);
    gpu.submitCommandBuffer(cmd);
    gpu.releaseTransferBuffer(tb);
}

// ── Host clock for Doom's tic rate ──────────────────────────────────────────

extern "C" unsigned long DG_HostTicksMs(void)
{
    return (unsigned long)SDL_GetTicks();
}

// ── Input translation ───────────────────────────────────────────────────────

static int SdlKeyToDoom(SDL_Keycode kc)
{
    switch (kc)
    {
        case SDLK_LEFT:      return DG_KEY_LEFTARROW;
        case SDLK_RIGHT:     return DG_KEY_RIGHTARROW;
        case SDLK_UP:        return DG_KEY_UPARROW;
        case SDLK_DOWN:      return DG_KEY_DOWNARROW;
        case SDLK_ESCAPE:    return DG_KEY_ESCAPE;
        case SDLK_RETURN:    return DG_KEY_ENTER;
        case SDLK_TAB:       return DG_KEY_TAB;
        case SDLK_BACKSPACE: return DG_KEY_BACKSPACE;
        case SDLK_PAUSE:     return DG_KEY_PAUSE;
        case SDLK_LCTRL:
        case SDLK_RCTRL:     return DG_KEY_RCTRL;    // fire
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:    return DG_KEY_RSHIFT;   // run
        case SDLK_LALT:
        case SDLK_RALT:      return DG_KEY_RALT;     // strafe
        case SDLK_F1:  return DG_KEY_F1;
        case SDLK_F2:  return DG_KEY_F2;
        case SDLK_F3:  return DG_KEY_F3;
        case SDLK_F4:  return DG_KEY_F4;
        case SDLK_F5:  return DG_KEY_F5;
        case SDLK_F6:  return DG_KEY_F6;
        case SDLK_F7:  return DG_KEY_F7;
        case SDLK_F8:  return DG_KEY_F8;
        case SDLK_F9:  return DG_KEY_F9;
        case SDLK_F10: return DG_KEY_F10;
        case SDLK_F11: return DG_KEY_F11;
        case SDLK_F12: return DG_KEY_F12;
        default:
            // Printable ASCII (letters already arrive lowercase in SDL3).
            if (kc >= 0x20 && kc < 0x7f)
                return (int)std::tolower((int)kc);
            return 0;
    }
}

static int   g_mouseButtons = 0;
static bool  g_gpuMode = false;   // GPU renderer active (enables mouselook)
static float g_pitch   = 0.0f;    // look up/down angle, radians

// Keyboard is polled from SDL_GetKeyboardState each frame (edge-detected) rather
// than relying on SDL_EVENT_KEY_* callbacks, which proved unreliable on macOS
// (mouse events arrive but key events don't). Polling reads the actual key state
// and works as long as the OS delivers keyboard to the focused window.
static void PollKeyboard()
{
    static bool prev[SDL_SCANCODE_COUNT] = {};
    int n = 0;
    const bool* ks = SDL_GetKeyboardState(&n);
    if (!ks) return;
    if (n > SDL_SCANCODE_COUNT) n = SDL_SCANCODE_COUNT;

    for (int sc = 0; sc < n; ++sc) {
        if (ks[sc] == prev[sc]) continue;
        prev[sc] = ks[sc];
        // Host hotkey: ` toggles the upscale filter (not forwarded to Doom).
        if (sc == SDL_SCANCODE_GRAVE) {
            if (ks[sc]) { g_linearFilter = !g_linearFilter; ApplyFilter(); }
            continue;
        }
        SDL_Keycode kc = SDL_GetKeyFromScancode((SDL_Scancode)sc, SDL_GetModState(), false);
        int dk = SdlKeyToDoom(kc);
        if (dk) DG_KeyEvent(ks[sc] ? 1 : 0, dk);
    }
}

// ── App callbacks ───────────────────────────────────────────────────────────

Lumi::Result AppInit(void** /*appstate*/, int argc, char* argv[])
{
    Window::InitWindow("DOOM — Luminoveau", 960, 600, 1, SDL_WINDOW_RESIZABLE);
    Renderer::ClearBackground({0, 0, 0, 255});
    Audio::Init();   // not done by InitWindow; needed before the PCM mixer

    // Doom locates its IWAD with raw fopen/access, not PhysFS, and the working
    // directory can't be relied on (a macOS .app launched via `open` starts in
    // "/"). Derive an absolute assets path from the app location instead.
#ifdef __EMSCRIPTEN__
    setenv("DOOMWADDIR", "/assets", 1);
    setenv("HOME", "/", 1);
#else
    {
        const char* base = SDL_GetBasePath();          // exe dir, or .app/Contents/Resources/
        std::string dir = (base ? std::string(base) : std::string("./")) + "assets";
        Dg_SetEnv("DOOMWADDIR", dir.c_str());
        if (!getenv("HOME")) Dg_SetEnv("HOME", base ? base : ".");
        LOG_INFO("DOOM: WAD dir = {}", dir.c_str());
    }
#endif

    CreateScreenTexture();
    if (getenv("DOOM_LINEAR")) { g_linearFilter = true; ApplyFilter(); }

    DG_Init(argc, argv);   // full Doom startup; returns after init (precaches SFX)

    // Start the SFX mixer: a continuous PCM generator on the audio thread that
    // pulls from Doom's 8 channels (DG_MixAudio). Kept alive for the app's life.
    static PCMSound pcm = Audio::CreatePCMGenerator(
        PCMFormat{DG_AUDIO_RATE, 2}, &DG_MixAudio, nullptr);
    Audio::PlayPCMSound(pcm, AudioChannel::SFX);

    // GPU renderer (see plan.md). Fase 0: opt-in scaffolding — a custom RenderPass
    // that draws a triangle over the frame, proving the pass + GLSL pipeline work
    // on every backend. Enable with DOOM_GPU=1.
    if (getenv("DOOM_GPU")) {
        static DoomRenderPass doomPass;
        auto& gpu = Renderer::GetGpu();
        // Init at the primary framebuffer's size (desktop bounds), so the pass's
        // own depth target matches the color target it renders into.
        uint32_t dw = 0, dh = 0;
        Window::GetDisplayBounds(dw, dh);
        if (doomPass.init(gpu.getSwapchainFormat(), dw, dh, "doom3d")) {
            Renderer::AttachRenderPassToFrameBuffer(&doomPass, "doom3d", "primaryFramebuffer");
            g_gpuMode = true;
        }
    }

    // Relative mouse mode (for mouselook) is toggled per-frame in AppIterate based
    // on whether a menu/UI is up, so it's released when the user needs the cursor.

    return Lumi::Result::Continue;
}

Lumi::Result AppIterate(void* /*appstate*/)
{
    PollKeyboard();                    // feed keyboard edges into Doom

    // Mouselook (GPU mode): horizontal delta turns the player in the game sim
    // (via Doom's ev_mouse), vertical delta drives the renderer-only pitch.
    // Suspended while a menu/pause/automap is up — and the cursor is released
    // then so the user can move the mouse freely.
    if (g_gpuMode) {
        bool uiActive = DG_UIActive();
        static int  prevRelative = -1;  // -1 = force first apply
        static int  settle = 0;
        int wantRelative = uiActive ? 0 : 1;
        if (wantRelative != prevRelative) {
            Window::SetRelativeMouseMode(wantRelative != 0);
            prevRelative = wantRelative;
            settle = 0;                 // discard the spurious delta after (re)capture
        }
        if (!uiActive) {
            vf2d md = Input::GetMouseDelta();
            if (settle < 6) { settle++; md = {0.0f, 0.0f}; }
            if (md.x != 0.0f) DG_MouseEvent(g_mouseButtons, (int)(md.x * 2.0f), 0);
            g_pitch -= md.y * 0.003f;      // up = look up (screen-y grows downward)
            const float lim = 1.30f;        // ~74°, clamp so we don't flip over
            if (g_pitch >  lim) g_pitch =  lim;
            if (g_pitch < -lim) g_pitch = -lim;
            DG_SetPitch(g_pitch);
        }
    }

    DG_Tic();                          // advance the game one tic
    UploadScreen(DG_GetFramebuffer()); // stream the frame to the GPU

    // Optional: capture one frame a few seconds in (DOOM_SHOT=1). Off in normal play.
    if (getenv("DOOM_SHOT")) {
        static int frame = 0;
        if (++frame == 180) Window::TakeScreenshot("doom_shot.png");
    }

    Window::StartFrame();

    // Letterbox the 320x200 (displayed 4:3) image inside the window. The primary
    // framebuffer / sprite camera is in PHYSICAL pixels, so use physical size —
    // on a Retina display logical size is half of that and would shrink the image
    // to a quarter of the window.
    const float W = (float)Window::GetPhysicalWidth();
    const float H = (float)Window::GetPhysicalHeight();
    const float targetAspect = 4.0f / 3.0f;
    float dstW, dstH;
    if (W / H > targetAspect) { dstH = H; dstW = H * targetAspect; }
    else                      { dstW = W; dstH = W / targetAspect; }
    const float x = (W - dstW) * 0.5f;
    const float y = (H - dstH) * 0.5f;

    Draw::Texture(g_screen, {x, y}, {dstW, dstH});

    Window::EndFrame();
    return Lumi::Result::Continue;
}

Lumi::Result AppEvent(void* /*appstate*/, SDL_Event* event)
{
    // Keyboard is handled by PollKeyboard() in AppIterate, not here.
    switch (event->type)
    {
        case SDL_EVENT_MOUSE_MOTION:
            // In GPU mode, mouse motion is read via Input::GetMouseDelta in
            // AppIterate (turn + pitch); avoid double-feeding here.
            if (!g_gpuMode)
                DG_MouseEvent(g_mouseButtons,
                              (int)event->motion.xrel,
                              (int)event->motion.yrel);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            int bit = 0;
            if (event->button.button == SDL_BUTTON_LEFT)   bit = 1;
            else if (event->button.button == SDL_BUTTON_RIGHT)  bit = 2;
            else if (event->button.button == SDL_BUTTON_MIDDLE) bit = 4;
            if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) g_mouseButtons |= bit;
            else                                            g_mouseButtons &= ~bit;
            DG_MouseEvent(g_mouseButtons, 0, 0);
            break;
        }
        default: break;
    }
    return Lumi::Result::Continue;
}

void AppQuit(void* /*appstate*/, Lumi::Result /*result*/)
{
    Window::Close();
}
