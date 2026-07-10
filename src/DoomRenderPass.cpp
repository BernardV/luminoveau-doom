#include "DoomRenderPass.h"

#include <cstring>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/log/log.h"
#include "gpu/IGpu.h"
#include "assets/assethandler.h"
#include "renderer/renderer.h"
#include "platform/window/window.h"

extern "C" {
#include "dg_bridge.h"
}

// Vertex from dg_render.c: pos(vec3) + uv(vec2) + shade(float) = 24 bytes.
struct WorldVertex { float x, y, z, u, v, shade; };

// Doom psprites: weapon + muzzle flash (mirrors NUMPSPRITES in p_pspr.h).
static constexpr int NUMPSPRITES_LOCAL = 2;

// Wall + flat textures uploaded to the GPU, cached by id (separate namespaces).
static std::unordered_map<int, GpuTextureHandle> g_wallTextures;
static std::unordered_map<int, GpuTextureHandle> g_flatTextures;
static GpuSamplerHandle g_wallSampler = 0;

// Box-filter one RGBA mip level (w×h) down to (w/2 × h/2), averaging 2×2 blocks.
static void downsampleRGBA(const unsigned char* src, int w, int h,
                           std::vector<unsigned char>& dst, int& dw, int& dh) {
    dw = w > 1 ? w / 2 : 1;
    dh = h > 1 ? h / 2 : 1;
    dst.resize((size_t)dw * dh * 4);
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            int sx = x * 2, sy = y * 2;
            int sx1 = (sx + 1 < w) ? sx + 1 : sx;
            int sy1 = (sy + 1 < h) ? sy + 1 : sy;
            const unsigned char* a = &src[((size_t)sy  * w + sx ) * 4];
            const unsigned char* b = &src[((size_t)sy  * w + sx1) * 4];
            const unsigned char* c = &src[((size_t)sy1 * w + sx ) * 4];
            const unsigned char* d = &src[((size_t)sy1 * w + sx1) * 4];
            unsigned char* o = &dst[((size_t)y * dw + x) * 4];
            for (int k = 0; k < 4; k++) o[k] = (a[k] + b[k] + c[k] + d[k] + 2) / 4;
        }
    }
}

// Upload an RGBA texture with a full mip chain (generated on the CPU) so the
// wall/flat sampler can do trilinear + anisotropic filtering — kills the distant
// shimmer/aliasing the fixed-res nearest sampler produced. Must be called OUTSIDE
// a render pass. kind selects wall vs flat.
static GpuTextureHandle uploadTexture(int kind, int id) {
    auto& cache = (kind == DG_KIND_FLAT) ? g_flatTextures : g_wallTextures;
    auto it = cache.find(id);
    if (it != cache.end()) return it->second;

    int w = 0, h = 0;
    const unsigned char* rgba = (kind == DG_KIND_FLAT)
        ? DG_FlatTextureRGBA(id, &w, &h) : DG_WallTextureRGBA(id, &w, &h);
    GpuTextureHandle tex = 0;
    if (rgba && w > 0 && h > 0) {
        IGpu& gpu = Renderer::GetGpu();
        // Full mip chain: floor(log2(max(w,h))) + 1 levels.
        uint32_t levels = 1; { int m = w > h ? w : h; while (m > 1) { m >>= 1; levels++; } }

        GpuTextureCreateInfo ci{};
        ci.width = (uint32_t)w; ci.height = (uint32_t)h;
        ci.depthOrLayers = 1; ci.numLevels = levels;
        ci.format = GpuTextureFormat::R8G8B8A8_Unorm;
        ci.sampleCount = GpuSampleCount::x1;
        ci.usage = GpuTextureUsage::Sampler | GpuTextureUsage::Transfer;
        tex = gpu.createTexture(ci);
        if (tex) {
            // Upload level 0, then each generated level.
            const unsigned char* lvlData = rgba;
            std::vector<unsigned char> cur, prev;
            int lw = w, lh = h;
            bool ok = true;
            for (uint32_t lvl = 0; lvl < levels && ok; lvl++) {
                if (lvl > 0) {
                    // Downsample the previous level (lw,lh hold its dims; updated to the new dims).
                    downsampleRGBA(lvlData, lw, lh, cur, lw, lh);
                    prev.swap(cur);
                    lvlData = prev.data();
                }
                uint32_t bytes = (uint32_t)lw * lh * 4u;
                GpuTransferBufferHandle tb = gpu.createTransferBuffer({ bytes, GpuTransferUsage::Upload });
                void* ptr = tb ? gpu.mapTransferBuffer(tb, false) : nullptr;
                if (!ptr) { if (tb) gpu.releaseTransferBuffer(tb); ok = false; break; }
                std::memcpy(ptr, lvlData, bytes);
                gpu.unmapTransferBuffer(tb);
                GpuCmdBufferHandle cmd = gpu.acquireCommandBuffer();
                GpuTransferBufferRegion src{}; src.transferBuffer = tb;
                GpuTextureRegion dst{}; dst.texture = tex; dst.mipLevel = lvl;
                dst.width = (uint32_t)lw; dst.height = (uint32_t)lh; dst.depth = 1;
                gpu.uploadToTexture(cmd, src, dst, false);
                gpu.submitCommandBuffer(cmd);
                gpu.releaseTransferBuffer(tb);
            }
            if (!ok) { gpu.releaseTexture(tex); tex = 0; }
        }
    }
    cache[id] = tex;   // cache even null, to avoid retrying
    return tex;
}

void DoomRenderPass::createDepth(uint32_t w, uint32_t h) {
    IGpu& gpu = Renderer::GetGpu();
    if (m_depth_texture.gpuTexture) { gpu.releaseTexture(m_depth_texture.gpuTexture); m_depth_texture.gpuTexture = 0; }
    GpuTextureCreateInfo d{};
    d.width = w ? w : 1; d.height = h ? h : 1;
    d.depthOrLayers = 1; d.numLevels = 1;
    d.format = GpuTextureFormat::D32_Float;
    d.sampleCount = GpuSampleCount::x1;
    d.usage = GpuTextureUsage::DepthStencilTarget;
    m_depth_texture.gpuTexture = gpu.createTexture(d);
    m_depthW = w; m_depthH = h;
}

bool DoomRenderPass::init(GpuTextureFormat swapchainFormat,
                          uint32_t surfaceWidth, uint32_t surfaceHeight,
                          std::string name, bool logInit,
                          size_t /*capacity*/, bool /*forceNoMSAA*/) {
    passname      = std::move(name);
    m_sampleCount = Renderer::GetSampleCount();
    IGpu& gpu     = Renderer::GetGpu();

    createDepth(surfaceWidth, surfaceHeight);
    if (!m_depth_texture.gpuTexture) { LOG_ERROR("DoomRenderPass: depth creation failed"); return false; }

    // Walls/flats tile (Repeat) with trilinear + anisotropic filtering over the
    // CPU-generated mip chain: smooths minification and kills distant shimmer
    // (the "Modern" look). Magnification stays Linear (mag Nearest would re-alias
    // up close). LOD bias slightly negative to keep textures from going too soft.
    GpuSamplerCreateInfo ss{};
    ss.minFilter = GpuFilter::Linear; ss.magFilter = GpuFilter::Linear; ss.mipFilter = GpuFilter::Linear;
    ss.addressU = GpuSamplerAddressMode::Repeat;
    ss.addressV = GpuSamplerAddressMode::Repeat;
    ss.addressW = GpuSamplerAddressMode::Repeat;
    ss.maxAniso = 16.0f;
    ss.mipLodBias = -0.5f;
    ss.maxLod = 16.0f;
    g_wallSampler = gpu.createSampler(ss);

    Shader vs = AssetHandler::GetShader("assets/shaders/doom_world.vert");
    Shader fs = AssetHandler::GetShader("assets/shaders/doom_world.frag");
    if (!vs.gpuShader || !fs.gpuShader) { LOG_ERROR("DoomRenderPass: shader load failed"); return false; }

    static GpuVertexAttribute attrs[3] = {
        { .location = 0, .binding = 0, .format = GpuVertexElementFormat::Float3, .offset = 0  },
        { .location = 1, .binding = 0, .format = GpuVertexElementFormat::Float2, .offset = 12 },
        { .location = 2, .binding = 0, .format = GpuVertexElementFormat::Float,  .offset = 20 },
    };
    static GpuVertexBinding vbind = { .binding = 0, .stride = sizeof(WorldVertex), .instanceStepping = false };

    GpuGraphicsPipelineCreateInfo pci{};
    pci.vertexShader      = vs.gpuShader;
    pci.fragmentShader    = fs.gpuShader;
    pci.attributes        = attrs;
    pci.attributeCount    = 3;
    pci.bindings          = &vbind;
    pci.bindingCount      = 1;
    pci.fillMode          = GpuFillMode::Fill;
    pci.cullMode          = GpuCullMode::None;
    pci.frontFace         = GpuFrontFace::CounterClockwise;
    pci.colorTargetFormat = swapchainFormat;
    pci.hasDepthTarget    = true;
    pci.depthTargetFormat = GpuTextureFormat::D32_Float;
    pci.sampleCount       = m_sampleCount;
    m_pipeline = gpu.createGraphicsPipeline(pci);
    if (!m_pipeline) { LOG_ERROR("DoomRenderPass: pipeline creation failed"); return false; }

    // Sky pipeline: fullscreen triangle (no vertex buffer), no depth. Drawn first
    // each frame; the world then draws over it, so sky shows only through openings.
    Shader skyv = AssetHandler::GetShader("assets/shaders/doom_sky.vert");
    Shader skyf = AssetHandler::GetShader("assets/shaders/doom_sky.frag");
    if (skyv.gpuShader && skyf.gpuShader) {
        GpuGraphicsPipelineCreateInfo spci{};
        spci.vertexShader      = skyv.gpuShader;
        spci.fragmentShader    = skyf.gpuShader;
        spci.attributeCount    = 0;   // positions generated from gl_VertexIndex
        spci.bindingCount      = 0;
        spci.fillMode          = GpuFillMode::Fill;
        spci.cullMode          = GpuCullMode::None;
        spci.colorTargetFormat = swapchainFormat;
        spci.hasDepthTarget    = true;   // must match the pass's depth attachment
        spci.depthTargetFormat = GpuTextureFormat::D32_Float;
        spci.sampleCount       = m_sampleCount;
        m_skyPipeline = gpu.createGraphicsPipeline(spci);
    }
    if (!m_skyPipeline) LOG_WARNING("DoomRenderPass: sky pipeline unavailable (sky disabled)");

    // Sprite pipeline: same vertex layout as walls (pos+uv+shade), alpha-tested
    // (discard in shader), depth test+write on, no culling (billboards).
    Shader spv = AssetHandler::GetShader("assets/shaders/doom_sprite.vert");
    Shader spf = AssetHandler::GetShader("assets/shaders/doom_sprite.frag");
    if (spv.gpuShader && spf.gpuShader) {
        GpuGraphicsPipelineCreateInfo pp{};
        pp.vertexShader      = spv.gpuShader;
        pp.fragmentShader    = spf.gpuShader;
        pp.attributes        = attrs;      // reuse world layout (Float3,Float2,Float)
        pp.attributeCount    = 3;
        pp.bindings          = &vbind;
        pp.bindingCount      = 1;
        pp.fillMode          = GpuFillMode::Fill;
        pp.cullMode          = GpuCullMode::None;
        pp.frontFace         = GpuFrontFace::CounterClockwise;
        pp.colorTargetFormat = swapchainFormat;
        pp.hasDepthTarget    = true;
        pp.depthTargetFormat = GpuTextureFormat::D32_Float;
        pp.sampleCount       = m_sampleCount;
        m_spritePipeline = gpu.createGraphicsPipeline(pp);
    }
    if (!m_spritePipeline) LOG_WARNING("DoomRenderPass: sprite pipeline unavailable");
    // Sprites: nearest, clamp (no bleeding across the patch edge).
    {
        GpuSamplerCreateInfo ss2{};
        ss2.minFilter = GpuFilter::Nearest; ss2.magFilter = GpuFilter::Nearest; ss2.mipFilter = GpuFilter::Nearest;
        ss2.addressU = GpuSamplerAddressMode::ClampToEdge;
        ss2.addressV = GpuSamplerAddressMode::ClampToEdge;
        ss2.addressW = GpuSamplerAddressMode::ClampToEdge;
        m_spriteSampler = gpu.createSampler(ss2);
    }

    // Overlay pipeline: screen-space (NDC positions), alpha-tested, NO depth so it
    // always sits on top of the 3D view. Layout: vec2 ndc + vec2 uv (16 bytes).
    Shader ov = AssetHandler::GetShader("assets/shaders/doom_overlay.vert");
    Shader of = AssetHandler::GetShader("assets/shaders/doom_overlay.frag");
    if (ov.gpuShader && of.gpuShader) {
        static GpuVertexAttribute oa[2] = {
            { .location = 0, .binding = 0, .format = GpuVertexElementFormat::Float2, .offset = 0 },
            { .location = 1, .binding = 0, .format = GpuVertexElementFormat::Float2, .offset = 8 },
        };
        static GpuVertexBinding ob = { .binding = 0, .stride = 16, .instanceStepping = false };
        GpuGraphicsPipelineCreateInfo op{};
        op.vertexShader = ov.gpuShader; op.fragmentShader = of.gpuShader;
        op.attributes = oa; op.attributeCount = 2; op.bindings = &ob; op.bindingCount = 1;
        op.fillMode = GpuFillMode::Fill; op.cullMode = GpuCullMode::None;
        op.colorTargetFormat = swapchainFormat;
        // Must match the pass's depth attachment. Vertices output z=0 (near) so
        // they pass the LESS test vs any world depth → weapon always on top.
        op.hasDepthTarget = true;
        op.depthTargetFormat = GpuTextureFormat::D32_Float;
        op.sampleCount = m_sampleCount;
        m_overlayPipeline = gpu.createGraphicsPipeline(op);
    }
    if (!m_overlayPipeline) LOG_WARNING("DoomRenderPass: overlay pipeline unavailable (no weapon sprite)");

    if (logInit) LOG_INFO("DoomRenderPass: initialized ({})", passname.c_str());
    return true;
}

void DoomRenderPass::onResize(uint32_t w, uint32_t h) { createDepth(w, h); }

void DoomRenderPass::ensureGeometry() {
    int floatCount = 0; unsigned version = 0;
    const float* data = DG_WorldVertices(&floatCount, &version);
    if (!data || floatCount == 0) { m_vertexCount = 0; return; }
    if (version == m_geomVersion && m_vertexBuffer) return;

    IGpu& gpu = Renderer::GetGpu();
    const uint32_t bytes = (uint32_t)(floatCount * sizeof(float));
    if (bytes > m_vertexBytes) {
        if (m_vertexBuffer) gpu.releaseBuffer(m_vertexBuffer);
        m_vertexBuffer = gpu.createBuffer({ bytes, GpuBufferUsage::Vertex });
        m_vertexBytes  = bytes;
    }
    if (!m_vertexBuffer) { m_vertexCount = 0; return; }

    GpuTransferBufferHandle tb = gpu.createTransferBuffer({ bytes, GpuTransferUsage::Upload });
    if (!tb) { m_vertexCount = 0; return; }
    void* ptr = gpu.mapTransferBuffer(tb, false);
    std::memcpy(ptr, data, bytes);
    gpu.unmapTransferBuffer(tb);
    GpuCmdBufferHandle cmd = gpu.acquireCommandBuffer();
    gpu.uploadToBuffer(cmd, tb, 0, m_vertexBuffer, 0, bytes, false);
    gpu.submitCommandBuffer(cmd);
    gpu.releaseTransferBuffer(tb);

    m_vertexCount = (uint32_t)(floatCount / 6);
    m_geomVersion = version;

    // Pre-upload every group's wall texture NOW, before any render pass begins:
    // texture upload acquires+submits its own command buffer, illegal inside an
    // active render pass.
    int groups = DG_DrawGroupCount();
    for (int g = 0; g < groups; g++) {
        int kind, texid, first, count;
        DG_DrawGroup(g, &kind, &texid, &first, &count);
        uploadTexture(kind, texid);
    }
    uploadTexture(DG_KIND_WALL, DG_SkyTextureId());   // sky is a composite texture
}

// Sprite texture cache (by lump). Kept separate: RGBA patches with alpha.
static std::unordered_map<int, GpuTextureHandle> g_spriteTextures;
static GpuTextureHandle spriteTexture(int lump) {
    auto it = g_spriteTextures.find(lump);
    if (it != g_spriteTextures.end()) return it->second;
    int w = 0, h = 0;
    const unsigned char* rgba = DG_SpriteTextureRGBA(lump, &w, &h);
    GpuTextureHandle tex = 0;
    if (rgba && w > 0 && h > 0) {
        IGpu& gpu = Renderer::GetGpu();
        GpuTextureCreateInfo ci{};
        ci.width=(uint32_t)w; ci.height=(uint32_t)h; ci.depthOrLayers=1; ci.numLevels=1;
        ci.format=GpuTextureFormat::R8G8B8A8_Unorm; ci.sampleCount=GpuSampleCount::x1;
        ci.usage=GpuTextureUsage::Sampler|GpuTextureUsage::Transfer;
        tex = gpu.createTexture(ci);
        if (tex) {
            const uint32_t bytes=(uint32_t)w*h*4u;
            GpuTransferBufferHandle tb = gpu.createTransferBuffer({bytes, GpuTransferUsage::Upload});
            void* ptr = tb ? gpu.mapTransferBuffer(tb, false) : nullptr;
            if (ptr) {
                std::memcpy(ptr, rgba, bytes); gpu.unmapTransferBuffer(tb);
                GpuCmdBufferHandle cmd = gpu.acquireCommandBuffer();
                GpuTransferBufferRegion src{}; src.transferBuffer=tb;
                GpuTextureRegion dst{}; dst.texture=tex; dst.width=(uint32_t)w; dst.height=(uint32_t)h; dst.depth=1;
                gpu.uploadToTexture(cmd, src, dst, false);
                gpu.submitCommandBuffer(cmd); gpu.releaseTransferBuffer(tb);
            } else if (tex) { gpu.releaseTexture(tex); tex = 0; }
        }
    }
    g_spriteTextures[lump] = tex;
    return tex;
}

void DoomRenderPass::render(GpuCmdBufferHandle cmdBuffer,
                            GpuTextureHandle   targetTexture,
                            const glm::mat4&   /*camera2d*/) {
    if (!m_pipeline || !DG_WorldReady()) return;
    // When a menu/pause/automap is up, skip the GPU 3D so the software render
    // (which draws those overlays) shows through the base blit instead.
    if (DG_UIActive()) return;
    ensureGeometry();
    if (!m_vertexCount || !m_vertexBuffer) return;

    IGpu& gpu = Renderer::GetGpu();

    float eye[3], yaw, pitch;
    DG_GetView(eye, &yaw, &pitch);
    glm::vec3 pos(eye[0], eye[1], eye[2]);
    glm::vec3 fwd(std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch));
    glm::mat4 view = glm::lookAtLH(pos, pos + fwd, glm::vec3(0, 1, 0));

    // Match the software view region so the GPU 3D lines up with (and covers)
    // Doom's own 4:3-letterboxed view — main.cpp blits the 320x200 image into a
    // 4:3 box centred in the window, with the 32px status bar in its bottom
    // 32/200. The 3D view is the top 168/200 of that box.
    const float W = (float)Window::GetPhysicalWidth();
    const float H = (float)Window::GetPhysicalHeight();
    const float target = 4.0f / 3.0f;
    float boxW, boxH;
    if (W / H > target) { boxH = H; boxW = H * target; }
    else                { boxW = W; boxH = W / target; }
    const float ox = (W - boxW) * 0.5f;
    const float oy = (H - boxH) * 0.5f;
    const float viewH = boxH * (168.0f / 200.0f);   // 3D view height (excl. status bar)

    const float aspect = boxW / viewH;
    const float hFov = glm::radians(90.0f);
    const float vFov = 2.0f * std::atan(std::tan(hFov * 0.5f) / (aspect > 0 ? aspect : 1.0f));
    glm::mat4 proj = glm::perspectiveLH_ZO(vFov, aspect, 1.0f, 20000.0f);
    // Vertex uniform: MVP + camera eye (for distance-based lighting in the shader).
    struct { glm::mat4 mvp; glm::vec4 eye; } vpu;
    vpu.mvp = proj * view;
    vpu.eye = glm::vec4(pos, 1.0f);

    // Build + upload sprite billboards + weapon overlay BEFORE the render pass
    // (uploads acquire their own command buffers). Right vector ⟂ view fwd in XZ.
    prepareSprites(-std::sin(yaw), std::cos(yaw));
    prepareOverlay(ox, oy, boxW, boxH, viewH, W, H);

    const bool shouldResolve = (renderTargetResolve != 0);
    GpuColorTargetInfo ct{};
    ct.texture        = targetTexture;
    ct.resolveTexture = renderTargetResolve;
    ct.loadOp         = color_target_info_loadop;
    ct.storeOp        = shouldResolve ? GpuStoreOp::Resolve : GpuStoreOp::Store;
    ct.clearR = color_target_clear_r; ct.clearG = color_target_clear_g;
    ct.clearB = color_target_clear_b; ct.clearA = color_target_clear_a;

    GpuDepthStencilTargetInfo dt{};
    dt.texture    = renderTargetDepth ? renderTargetDepth : m_depth_texture.gpuTexture;
    dt.loadOp     = GpuLoadOp::Clear;
    dt.storeOp    = GpuStoreOp::Store;
    dt.clearDepth = 1.0f;

    GpuRenderPassHandle rp = gpu.beginRenderPass(cmdBuffer, &ct, 1, &dt);
    render_pass = rp;
    gpu.setViewport(rp, ox, oy, boxW, viewH, 0.0f, 1.0f);

    // Sky background first (fullscreen, no depth interaction); world draws over it.
    GpuTextureHandle skyTex = m_skyPipeline ? uploadTexture(DG_KIND_WALL, DG_SkyTextureId()) : 0;
    if (skyTex) {
        struct { float yawTurns, uSpan, vScale, vBias; } sp;
        sp.yawTurns = yaw / (float)(M_PI * 0.5);   // one sky width per 90°
        sp.uSpan    = hFov / (float)(M_PI * 0.5);  // texture-widths across the screen
        sp.vScale   = 1.0f;                          // maps view-y 0..1 into sky v (tune later)
        sp.vBias    = 0.0f;
        gpu.bindGraphicsPipeline(rp, m_skyPipeline);
        GpuTextureSamplerBinding sky{ skyTex, g_wallSampler };
        gpu.bindFragmentSamplers(rp, 0, &sky, 1);
        gpu.pushFragmentUniformData(cmdBuffer, 0, &sp, sizeof(sp));
        gpu.drawPrimitives(rp, 3, 1, 0, 0);
    }

    gpu.bindGraphicsPipeline(rp, m_pipeline);
    gpu.pushVertexUniformData(cmdBuffer, 0, &vpu, sizeof(vpu));
    GpuBufferBinding vb{ m_vertexBuffer, 0 };
    gpu.bindVertexBuffers(rp, 0, &vb, 1);

    // One draw per (kind, texture) group — walls and flats share the pipeline.
    int groups = DG_DrawGroupCount();
    for (int g = 0; g < groups; g++) {
        int kind, texid, first, count;
        DG_DrawGroup(g, &kind, &texid, &first, &count);
        if (count <= 0) continue;
        GpuTextureHandle tex = uploadTexture(kind, texid);
        if (!tex) continue;
        GpuTextureSamplerBinding tsb{ tex, g_wallSampler };
        gpu.bindFragmentSamplers(rp, 0, &tsb, 1);
        gpu.drawPrimitives(rp, (uint32_t)count, 1, (uint32_t)first, 0);
    }

    // Sprites (billboards) after the world so they depth-test against it.
    drawSprites(cmdBuffer, rp, &vpu, sizeof(vpu));

    // Player weapon sprite last, as a screen-space overlay on top of everything.
    drawOverlay(rp);

    gpu.endRenderPass(rp);
}

namespace {
struct SV { float x,y,z,u,v,s; };
std::vector<SV>  g_spriteVerts;
std::vector<int> g_spriteLump;    // texture lump per sprite
std::vector<int> g_spriteFirst;   // first vertex per sprite
}

// Called BEFORE beginRenderPass: builds billboards + uploads textures and the VB
// (all of which acquire/submit command buffers, illegal inside a render pass).
void DoomRenderPass::prepareSprites(float camRightX, float camRightZ) {
    m_spriteDrawCount = 0;
    if (!m_spritePipeline) return;
    int n = DG_SpriteCount();
    if (n <= 0) return;
    IGpu& gpu = Renderer::GetGpu();
    glm::vec3 right(camRightX, 0.0f, camRightZ);

    g_spriteVerts.clear(); g_spriteLump.clear(); g_spriteFirst.clear();
    for (int i = 0; i < n; i++) {
        float o[8]; DG_Sprite(i, o);
        glm::vec3 pos(o[0], o[1], o[2]);
        float halfW = o[3], top = o[4];
        int lump = (int)o[5]; int flip = (int)o[6]; float shade = o[7];
        if (!spriteTexture(lump)) continue;   // upload now (before the pass)
        glm::vec3 L = pos - right * halfW, R = pos + right * halfW;
        float y0 = pos.y, y1 = top;
        float uL = flip ? 1.f : 0.f, uR = flip ? 0.f : 1.f;
        g_spriteFirst.push_back((int)g_spriteVerts.size()); g_spriteLump.push_back(lump);
        g_spriteVerts.push_back({L.x,y0,L.z, uL,1.f, shade});
        g_spriteVerts.push_back({R.x,y0,R.z, uR,1.f, shade});
        g_spriteVerts.push_back({R.x,y1,R.z, uR,0.f, shade});
        g_spriteVerts.push_back({L.x,y0,L.z, uL,1.f, shade});
        g_spriteVerts.push_back({R.x,y1,R.z, uR,0.f, shade});
        g_spriteVerts.push_back({L.x,y1,L.z, uL,0.f, shade});
    }
    if (g_spriteVerts.empty()) return;

    uint32_t bytes = (uint32_t)(g_spriteVerts.size() * sizeof(SV));
    if (bytes > m_spriteVBBytes) {
        if (m_spriteVB) gpu.releaseBuffer(m_spriteVB);
        m_spriteVB = gpu.createBuffer({ bytes, GpuBufferUsage::Vertex });
        m_spriteVBBytes = bytes;
    }
    if (!m_spriteVB) return;
    GpuTransferBufferHandle tb = gpu.createTransferBuffer({ bytes, GpuTransferUsage::Upload });
    void* ptr = tb ? gpu.mapTransferBuffer(tb, false) : nullptr;
    if (!ptr) { if (tb) gpu.releaseTransferBuffer(tb); return; }
    std::memcpy(ptr, g_spriteVerts.data(), bytes);
    gpu.unmapTransferBuffer(tb);
    GpuCmdBufferHandle ucmd = gpu.acquireCommandBuffer();
    gpu.uploadToBuffer(ucmd, tb, 0, m_spriteVB, 0, bytes, false);
    gpu.submitCommandBuffer(ucmd);
    gpu.releaseTransferBuffer(tb);

    m_spriteDrawCount = (uint32_t)g_spriteLump.size();
}

// Called INSIDE the render pass: draw only (no uploads).
void DoomRenderPass::drawSprites(GpuCmdBufferHandle cmd, GpuRenderPassHandle rp,
                                 const void* vpu, uint32_t vpuSize) {
    if (!m_spriteDrawCount || !m_spriteVB) return;
    IGpu& gpu = Renderer::GetGpu();
    gpu.bindGraphicsPipeline(rp, m_spritePipeline);
    gpu.pushVertexUniformData(cmd, 0, vpu, vpuSize);
    GpuBufferBinding vb{ m_spriteVB, 0 };
    gpu.bindVertexBuffers(rp, 0, &vb, 1);
    for (uint32_t i = 0; i < m_spriteDrawCount; i++) {
        GpuTextureHandle tex = spriteTexture(g_spriteLump[i]);   // cached (uploaded in prepare)
        if (!tex) continue;
        GpuTextureSamplerBinding tsb{ tex, m_spriteSampler };
        gpu.bindFragmentSamplers(rp, 0, &tsb, 1);
        gpu.drawPrimitives(rp, 6, 1, (uint32_t)g_spriteFirst[i], 0);
    }
}

namespace {
struct OV { float x,y,u,v; };
std::vector<OV>  g_ovlVerts;
std::vector<int> g_ovlLump;
std::vector<int> g_ovlFirst;
}

// Build the player weapon sprite(s) as screen-space NDC quads mapped into the
// 4:3 box (320x200 virtual → box), and upload. Call BEFORE beginRenderPass.
void DoomRenderPass::prepareOverlay(float ox, float oy, float boxW, float boxH,
                                    float viewH, float W, float H) {
    m_overlayDrawCount = 0;
    if (!m_overlayPipeline) return;
    IGpu& gpu = Renderer::GetGpu();
    g_ovlVerts.clear(); g_ovlLump.clear(); g_ovlFirst.clear();

    // Map a Doom 320x200 pixel to NDC within the box.
    auto toNdc = [&](float px, float py, float& nx, float& ny) {
        float bx = ox + (px / 320.0f) * boxW;
        float by = oy + (py / 200.0f) * boxH;
        nx = bx / W * 2.0f - 1.0f;
        ny = 1.0f - by / H * 2.0f;
    };

    // Crosshair: a small white "+" at NDC (0,0). The overlay is drawn through the
    // 3D viewport, whose centre (NDC origin) is exactly the view direction — i.e.
    // where a level shot goes — so this always sits on the aim point regardless of
    // letterbox/aspect. (Earlier bug: computed in full-window pixels, which the
    // 3D viewport then remapped, placing it ~80px too high.)
    {
        const float armX = 0.012f, armY = armX * (boxW / viewH);  // square on screen
        const float thX = armX * 0.18f, thY = armY * 0.18f;
        struct { float x0,y0,x1,y1; } bars[2] = {
            { -armX, -thY,  armX,  thY },   // horizontal
            { -thX,  -armY, thX,   armY },  // vertical
        };
        for (int b = 0; b < 2; b++) {
            float x0=bars[b].x0, y0=bars[b].y0, x1=bars[b].x1, y1=bars[b].y1;
            g_ovlFirst.push_back((int)g_ovlVerts.size()); g_ovlLump.push_back(-1);
            g_ovlVerts.push_back({x0,y0, 0,0}); g_ovlVerts.push_back({x1,y0, 0,0});
            g_ovlVerts.push_back({x1,y1, 0,0}); g_ovlVerts.push_back({x0,y0, 0,0});
            g_ovlVerts.push_back({x1,y1, 0,0}); g_ovlVerts.push_back({x0,y1, 0,0});
        }
    }

    for (int idx = NUMPSPRITES_LOCAL - 1; idx >= 0; idx--) {   // flash over weapon? draw weapon then flash
        float o[7];
        if (!DG_WeaponSprite(idx, o)) continue;
        int lump = (int)o[0];
        float xL = o[1], yT = o[2], w = o[3], h = o[4];
        int flip = (int)o[5];
        if (!spriteTexture(lump)) continue;
        float nx0,ny0,nx1,ny1;
        toNdc(xL, yT, nx0, ny0);            // top-left
        toNdc(xL + w, yT + h, nx1, ny1);    // bottom-right
        float uL = flip ? 1.f : 0.f, uR = flip ? 0.f : 1.f;
        g_ovlFirst.push_back((int)g_ovlVerts.size()); g_ovlLump.push_back(lump);
        g_ovlVerts.push_back({nx0,ny0, uL,0.f});
        g_ovlVerts.push_back({nx1,ny0, uR,0.f});
        g_ovlVerts.push_back({nx1,ny1, uR,1.f});
        g_ovlVerts.push_back({nx0,ny0, uL,0.f});
        g_ovlVerts.push_back({nx1,ny1, uR,1.f});
        g_ovlVerts.push_back({nx0,ny1, uL,1.f});
    }
    if (g_ovlVerts.empty()) return;

    uint32_t bytes = (uint32_t)(g_ovlVerts.size() * sizeof(OV));
    if (bytes > m_overlayVBBytes) {
        if (m_overlayVB) gpu.releaseBuffer(m_overlayVB);
        m_overlayVB = gpu.createBuffer({ bytes, GpuBufferUsage::Vertex });
        m_overlayVBBytes = bytes;
    }
    if (!m_overlayVB) return;
    GpuTransferBufferHandle tb = gpu.createTransferBuffer({ bytes, GpuTransferUsage::Upload });
    void* ptr = tb ? gpu.mapTransferBuffer(tb, false) : nullptr;
    if (!ptr) { if (tb) gpu.releaseTransferBuffer(tb); return; }
    std::memcpy(ptr, g_ovlVerts.data(), bytes);
    gpu.unmapTransferBuffer(tb);
    GpuCmdBufferHandle ucmd = gpu.acquireCommandBuffer();
    gpu.uploadToBuffer(ucmd, tb, 0, m_overlayVB, 0, bytes, false);
    gpu.submitCommandBuffer(ucmd);
    gpu.releaseTransferBuffer(tb);
    m_overlayDrawCount = (uint32_t)g_ovlLump.size();
}

void DoomRenderPass::drawOverlay(GpuRenderPassHandle rp) {
    if (!m_overlayDrawCount || !m_overlayVB) return;
    IGpu& gpu = Renderer::GetGpu();
    gpu.bindGraphicsPipeline(rp, m_overlayPipeline);
    GpuBufferBinding vb{ m_overlayVB, 0 };
    gpu.bindVertexBuffers(rp, 0, &vb, 1);
    for (uint32_t i = 0; i < m_overlayDrawCount; i++) {
        GpuTextureHandle tex = (g_ovlLump[i] == -1)
            ? Renderer::WhitePixel().gpuTexture      // crosshair
            : spriteTexture(g_ovlLump[i]);
        if (!tex) continue;
        GpuTextureSamplerBinding tsb{ tex, m_spriteSampler };
        gpu.bindFragmentSamplers(rp, 0, &tsb, 1);
        gpu.drawPrimitives(rp, 6, 1, (uint32_t)g_ovlFirst[i], 0);
    }
}

void DoomRenderPass::release(bool /*logRelease*/) {
    IGpu& gpu = Renderer::GetGpu();
    if (m_vertexBuffer)             { gpu.releaseBuffer(m_vertexBuffer); m_vertexBuffer = 0; }
    if (m_depth_texture.gpuTexture) { gpu.releaseTexture(m_depth_texture.gpuTexture); m_depth_texture.gpuTexture = 0; }
    m_pipeline = 0;
}
