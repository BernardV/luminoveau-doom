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
static bool  g_touch   = false;   // on-screen touch controls active (mobile/web)
static bool  g_sole    = false;   // GPU is the sole 3D renderer (Fase 7)

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
        // Host hotkey: F5 (and ` as alt) toggles the texture filter — NOT forwarded
        // to Doom. GPU mode: Authentic+ (crisp) ↔ Modern (smooth). Else: software blit.
        if (sc == SDL_SCANCODE_F5 || sc == SDL_SCANCODE_GRAVE) {
            if (ks[sc]) {
                if (g_gpuMode) {
                    int smooth = DoomRenderPass_ToggleFilter();
                    LOG_INFO("Texture filter: {}", smooth ? "Modern (smooth)" : "Authentic+ (crisp)");
                } else { g_linearFilter = !g_linearFilter; ApplyFilter(); }
            }
            continue;
        }
        // Forward the plain key. WASD walk (W/S forward-back, A/D strafe) is handled
        // inside Doom's G_BuildTiccmd, which reads these letters as movement too —
        // so the letters still flow cleanly for cheats (iddqd…) and menus.
        SDL_Keycode kc = SDL_GetKeyFromScancode((SDL_Scancode)sc, SDL_GetModState(), false);
        int dk = SdlKeyToDoom(kc);
        if (dk) DG_KeyEvent(ks[sc] ? 1 : 0, dk);
    }
}

// ── Gamepad ───────────────────────────────────────────────────────────────
// Driven entirely through Lumi's engine Input API (Input::Get*), keyed off a
// gamepad the engine has detected. If none is connected, PollGamepad() returns
// immediately and no gamepad code runs. All actions are synthesized into Doom's
// existing key/mouse event stream, so Doom's own binding logic handles them.

// Runtime detection: the engine tracks connect/disconnect (SDL_EVENT_GAMEPAD_*),
// so we just look for a GAMEPAD device. Returns its id, or -1 if none.
static int ActiveGamepadId()
{
    for (InputDevice* d : Input::GetAllInputs())
        if (d && d->getType() == InputType::GAMEPAD) return d->getGamepadID();
    return -1;
}

// Emit a Doom key edge only when the pressed state changes.
static void EdgeKey(bool now, bool& prev, int doomKey)
{
    if (now == prev) return;
    prev = now;
    DG_KeyEvent(now ? 1 : 0, doomKey);
}
// Momentary tap (down+up) for stepped actions like weapon select.
static void TapKey(int doomKey) { DG_KeyEvent(1, doomKey); DG_KeyEvent(0, doomKey); }

// Mouse buttons via the engine Input API (polled SDL_GetMouseState — reliable),
// not SDL_AppEvent, which drops events on macOS (same reason keyboard is polled).
// Doom maps the bitmask natively: left=fire, right=strafe, middle=forward.
static void PollMouseButtons()
{
    int b = 0;
    if (Input::MouseButtonDown(SDL_BUTTON_LEFT))   b |= 1;
    if (Input::MouseButtonDown(SDL_BUTTON_RIGHT))  b |= 2;
    if (Input::MouseButtonDown(SDL_BUTTON_MIDDLE)) b |= 4;
    if (b != g_mouseButtons) { g_mouseButtons = b; DG_MouseEvent(g_mouseButtons, 0, 0); }
}

static void PollGamepad()
{
    int id = ActiveGamepadId();
    if (id < 0) return;                       // no pad → nothing runs

    using GA = SDL_GamepadAxis;
    using GB = SDL_GamepadButton;
    auto axis = [&](GA a){ return Input::GetGamepadAxisMovement(id, a); };
    auto btn  = [&](GB b){ return Input::GamepadButtonDown(id, (int)b); };

    // Persistent edge state across frames.
    static bool eFwd=false, eBack=false, eStrL=false, eStrR=false;
    static bool eFire=false, eUse=false, eRun=false, eEsc=false;
    static bool eMUp=false, eMDn=false, eMLt=false, eMRt=false, eEnter=false;
    static bool eNext=false, ePrev=false;
    static int  weapon = 1;

    // Sensitivities (env-overridable so they can be dialed in without rebuilding):
    //   DOOM_GP_TURN  right-stick turn speed   (default 16)
    //   DOOM_GP_LOOK  right-stick look/pitch    (default 0.04)
    //   DOOM_GP_MOVET move stick threshold      (default 0.30)
    static const float TURN = getenv("DOOM_GP_TURN")  ? (float)atof(getenv("DOOM_GP_TURN"))  : 16.0f;
    static const float LOOK = getenv("DOOM_GP_LOOK")  ? (float)atof(getenv("DOOM_GP_LOOK"))  : 0.04f;
    static const float T    = getenv("DOOM_GP_MOVET") ? (float)atof(getenv("DOOM_GP_MOVET")) : 0.30f;

    const bool ui = DG_UIActive();
    const float lx = axis(SDL_GAMEPAD_AXIS_LEFTX),  ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    const float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    const float lt = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    const float rt = axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

    if (ui) {
        // Menu navigation: dpad or left stick step through items; A confirms, B/Start exit.
        EdgeKey(ly < -T || btn(SDL_GAMEPAD_BUTTON_DPAD_UP),    eMUp,  DG_KEY_UPARROW);
        EdgeKey(ly >  T || btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN),  eMDn,  DG_KEY_DOWNARROW);
        EdgeKey(lx < -T || btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT),  eMLt,  DG_KEY_LEFTARROW);
        EdgeKey(lx >  T || btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), eMRt,  DG_KEY_RIGHTARROW);
        EdgeKey(btn(SDL_GAMEPAD_BUTTON_SOUTH),  eEnter, DG_KEY_ENTER);
        EdgeKey(btn(SDL_GAMEPAD_BUTTON_EAST) || btn(SDL_GAMEPAD_BUTTON_START), eEsc, DG_KEY_ESCAPE);
        // Clear any held movement so the player doesn't drift while paused.
        EdgeKey(false, eFwd,  DG_KEY_UPARROW);
        EdgeKey(false, eBack, DG_KEY_DOWNARROW);
        EdgeKey(false, eStrL, ',');  EdgeKey(false, eStrR, '.');
        EdgeKey(false, eFire, DG_KEY_RCTRL);
        return;
    }
    // Release the menu-nav edges when leaving a menu.
    EdgeKey(false, eMUp, DG_KEY_UPARROW);  EdgeKey(false, eMDn, DG_KEY_DOWNARROW);
    EdgeKey(false, eMLt, DG_KEY_LEFTARROW); EdgeKey(false, eMRt, DG_KEY_RIGHTARROW);
    EdgeKey(false, eEnter, DG_KEY_ENTER);

    // Movement: left stick or dpad. Doom moves via arrow keys + strafe keys (',' '.').
    EdgeKey(ly < -T || btn(SDL_GAMEPAD_BUTTON_DPAD_UP),    eFwd,  DG_KEY_UPARROW);
    EdgeKey(ly >  T || btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN),  eBack, DG_KEY_DOWNARROW);
    EdgeKey(lx < -T, eStrL, ',');
    EdgeKey(lx >  T, eStrR, '.');

    // Look: right stick → analog turn (Doom's mouse-turn) + renderer pitch.
    if (rx != 0.0f) DG_MouseEvent(g_mouseButtons, (int)(rx * TURN), 0);
    if (ry != 0.0f && g_gpuMode) {
        g_pitch -= ry * LOOK;
        const float lim = 1.30f;
        if (g_pitch >  lim) g_pitch =  lim;
        if (g_pitch < -lim) g_pitch = -lim;
        DG_SetPitch(g_pitch);
    }

    // Actions: RT fire, A use, LT run, B/Start menu.
    EdgeKey(rt > T,                         eFire, DG_KEY_RCTRL);
    EdgeKey(btn(SDL_GAMEPAD_BUTTON_SOUTH),  eUse,  ' ');
    EdgeKey(lt > T,                         eRun,  DG_KEY_RSHIFT);
    EdgeKey(btn(SDL_GAMEPAD_BUTTON_EAST) || btn(SDL_GAMEPAD_BUTTON_START), eEsc, DG_KEY_ESCAPE);

    // Weapon cycle on shoulders → tap number keys 1..7.
    bool next = btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    bool prev = btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    if (next && !eNext) { weapon = weapon % 7 + 1;       TapKey('0' + weapon); }
    if (prev && !ePrev) { weapon = (weapon + 5) % 7 + 1; TapKey('0' + weapon); }
    eNext = next; ePrev = prev;
}

// ── Touch (on-screen virtual controls) ──────────────────────────────────────
// The engine's VirtualControls (a left joystick + up to 4 right buttons) is fed
// by auto-routed SDL finger events. Enabled only when a touch device is present
// (or DOOM_TOUCH=1 to test on desktop); otherwise none of this runs.

static bool TouchAvailable()
{
    if (getenv("DOOM_TOUCH")) return true;
#ifdef __EMSCRIPTEN__
    // Web: auto-enable only for a REAL hardware touchscreen. SDL also exposes a
    // synthetic "pen"/"mouse" touch device (negative id) for pointer emulation —
    // skip it, else desktop browsers get stray on-screen controls.
    int count = 0;
    SDL_TouchID* devs = SDL_GetTouchDevices(&count);
    bool hasScreen = false;
    for (int i = 0; i < count; i++) {
        if ((int64_t)devs[i] < 0) continue;                          // synthetic mouse/pen device
        if (SDL_GetTouchDeviceType(devs[i]) == SDL_TOUCH_DEVICE_DIRECT) { hasScreen = true; break; }
    }
    if (devs) SDL_free(devs);
    return hasScreen;
#else
    // Native desktop always has a mouse + keyboard, and SDL reports a synthetic
    // pen/mouse "touch" device there — so never auto-enable; use DOOM_TOUCH=1 to test.
    return false;
#endif
}

static void PollTouch()
{
    if (!g_touch) return;
    auto& vc = Input::GetVirtualControls();

    static bool tFwd=false, tBack=false, tStrL=false, tStrR=false;
    static bool tFire=false, tUse=false, tEsc=false;
    static bool tMUp=false, tMDn=false, tEnter=false;
    static bool bPrev[4] = {};

    const bool ui = DG_UIActive();
    vf2d dir = vc.GetJoystickDirection();     // screen space: +x right, +y down
    float mag = vc.GetJoystickMagnitude();
    const float T = 0.4f;
    bool up = mag > T && dir.y < -0.5f, down = mag > T && dir.y > 0.5f;
    bool left = mag > T && dir.x < -0.5f, right = mag > T && dir.x > 0.5f;

    // Button semantics: 0=fire, 1=use, 2=weapon-next, 3=menu. Edge via JustPressed.
    bool b0 = vc.IsButtonPressed(0), b1 = vc.IsButtonPressed(1);
    bool b2 = vc.IsButtonPressed(2), b3 = vc.IsButtonPressed(3);

    if (ui) {
        EdgeKey(up,   tMUp,  DG_KEY_UPARROW);
        EdgeKey(down, tMDn,  DG_KEY_DOWNARROW);
        EdgeKey(b0,   tEnter, DG_KEY_ENTER);
        EdgeKey(b3,   tEsc,  DG_KEY_ESCAPE);
        EdgeKey(false, tFwd, DG_KEY_UPARROW);   EdgeKey(false, tBack, DG_KEY_DOWNARROW);
        EdgeKey(false, tStrL, ','); EdgeKey(false, tStrR, '.'); EdgeKey(false, tFire, DG_KEY_RCTRL);
        for (int i = 0; i < 4; i++) bPrev[i] = false;
        return;
    }
    EdgeKey(false, tMUp, DG_KEY_UPARROW); EdgeKey(false, tMDn, DG_KEY_DOWNARROW);
    EdgeKey(false, tEnter, DG_KEY_ENTER);

    // Move with the stick; strafe is handled by A/D-style side keys.
    EdgeKey(up,    tFwd,  DG_KEY_UPARROW);
    EdgeKey(down,  tBack, DG_KEY_DOWNARROW);
    EdgeKey(left,  tStrL, ',');
    EdgeKey(right, tStrR, '.');

    EdgeKey(b0, tFire, DG_KEY_RCTRL);
    EdgeKey(b1, tUse,  ' ');
    EdgeKey(b3, tEsc,  DG_KEY_ESCAPE);
    if (b2 && !bPrev[2]) { static int w = 1; w = w % 7 + 1; TapKey('0' + w); }
    bPrev[0]=b0; bPrev[1]=b1; bPrev[2]=b2; bPrev[3]=b3;
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

    // GPU renderer (see plan.md): on by default; set DOOM_GPU=0 to fall back to
    // the pure software renderer.
    const char* gpuEnv = getenv("DOOM_GPU");
    bool gpuEnabled = !(gpuEnv && gpuEnv[0] == '0' && gpuEnv[1] == '\0');
    if (gpuEnabled) {
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

    // Fase 7 — GPU is the sole 3D renderer (opt-in via DOOM_SOLE=1 for now). Doom
    // skips its software 3D and clears that region to a transparent sentinel; the
    // software HUD/menu is composited OVER the GPU 3D via a "hud" render target
    // (renderToScreen), so the menu background becomes the GPU 3D and the wasted
    // software 3D render is reclaimed.
    if (g_gpuMode && getenv("DOOM_SOLE")) {
        SpriteRenderTargetConfig hudCfg;
        hudCfg.renderToScreen = true;             // composite over the primary (GPU) framebuffer
        hudCfg.clearOnLoad    = true;
        hudCfg.clearColor     = {0, 0, 0, 0};     // transparent — only opaque HUD/menu pixels show
        hudCfg.blendMode      = BlendMode::SrcAlpha;
        Renderer::CreateSpriteRenderTarget("hud", hudCfg);
        DG_SetSoleRenderer(1);
        g_sole = true;
        LOG_INFO("GPU sole-renderer mode (Fase 7) enabled");
    }

    // Relative mouse mode (for mouselook) is toggled per-frame in AppIterate based
    // on whether a menu/UI is up, so it's released when the user needs the cursor.

    // On-screen touch controls: only when a touch device is present (mobile/web),
    // or forced with DOOM_TOUCH=1. Left joystick + 4 buttons (fire/use/weapon/menu).
    if (TouchAvailable()) {
        auto& vc = Input::GetVirtualControls();
        vc.SetButtonCount(4);
        vc.SetJoystickMode(VirtualControls::JoystickMode::RELATIVE);
        vc.SetEnabled(true);
        g_touch = true;
        LOG_INFO("Touch controls enabled");
    }

    return Lumi::Result::Continue;
}

Lumi::Result AppIterate(void* /*appstate*/)
{
    PollKeyboard();                    // feed keyboard edges into Doom
    PollMouseButtons();                // mouse buttons (fire = left)
    PollGamepad();                     // + gamepad, if one is connected
    PollTouch();                       // + on-screen touch controls, if enabled

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

    // Blit the 320x200 software image. In sole mode it carries the HUD/menu with a
    // transparent 3D-view region, and must composite OVER the GPU 3D — so route it
    // to the "hud" render target (renderToScreen). In legacy mode it's the full
    // frame drawn under the GPU 3D via the default layer.
    if (g_sole) {
        Draw::SetTargetRenderPass("hud");
        Draw::Texture(g_screen, {x, y}, {dstW, dstH});
        Draw::SetTargetRenderPass("2dsprites");
    } else {
        Draw::Texture(g_screen, {x, y}, {dstW, dstH});
    }

    // On-screen touch controls. VirtualControls renders AND hit-tests in the
    // engine's logical coordinate space (both use Window::GetWidth/Height), so
    // they stay mutually consistent. On a HiDPI desktop where this app otherwise
    // draws in physical pixels they appear in the logical (top-left) quadrant —
    // a cosmetic quirk of the DOOM_TOUCH desktop test; on real touch targets
    // (mobile/web, logical == physical) they anchor to the corners as intended.
    if (g_touch) Input::GetVirtualControls().Render();

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
        // Mouse buttons are polled in PollMouseButtons() (SDL_AppEvent drops
        // them on macOS, same as keyboard), so nothing to do here.
        default: break;
    }
    return Lumi::Result::Continue;
}

void AppQuit(void* /*appstate*/, Lumi::Result /*result*/)
{
    Window::Close();
}
