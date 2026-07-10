#include "DoomRenderPass.h"

#include <cstring>
#include <string>
#include <unordered_map>
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

// Wall textures uploaded to the GPU, cached by Doom texture id.
static std::unordered_map<int, GpuTextureHandle> g_wallTextures;
static GpuSamplerHandle g_wallSampler = 0;

static GpuTextureHandle wallTexture(int texid) {
    auto it = g_wallTextures.find(texid);
    if (it != g_wallTextures.end()) return it->second;

    int w = 0, h = 0;
    const unsigned char* rgba = DG_WallTextureRGBA(texid, &w, &h);
    GpuTextureHandle tex = 0;
    if (rgba && w > 0 && h > 0) {
        TextureAsset ta = AssetHandler::LoadFromPixelData(
            {(float)w, (float)h}, (void*)rgba, "walltex_" + std::to_string(texid));
        tex = ta.gpuTexture;
    }
    g_wallTextures[texid] = tex;   // cache even null, to avoid retrying
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

    // Repeat + nearest sampler: walls tile, and nearest keeps the crisp Doom look.
    GpuSamplerCreateInfo ss{};
    ss.minFilter = GpuFilter::Nearest; ss.magFilter = GpuFilter::Nearest; ss.mipFilter = GpuFilter::Nearest;
    ss.addressU = GpuSamplerAddressMode::Repeat;
    ss.addressV = GpuSamplerAddressMode::Repeat;
    ss.addressW = GpuSamplerAddressMode::Repeat;
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

    // Pre-upload every group's wall texture NOW, before any render pass begins.
    // Texture upload (LoadFromPixelData) acquires+submits its own command buffer,
    // which is illegal inside an active render pass — so it must happen here.
    int groups = DG_WallGroupCount();
    for (int g = 0; g < groups; g++) {
        int texid, first, count;
        DG_WallGroup(g, &texid, &first, &count);
        wallTexture(texid);
    }
}

void DoomRenderPass::render(GpuCmdBufferHandle cmdBuffer,
                            GpuTextureHandle   targetTexture,
                            const glm::mat4&   /*camera2d*/) {
    if (!m_pipeline || !DG_WorldReady()) return;
    ensureGeometry();
    if (!m_vertexCount || !m_vertexBuffer) return;

    IGpu& gpu = Renderer::GetGpu();

    float eye[3], yaw, pitch;
    DG_GetView(eye, &yaw, &pitch);
    glm::vec3 pos(eye[0], eye[1], eye[2]);
    glm::vec3 fwd(std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch));
    glm::mat4 view = glm::lookAtLH(pos, pos + fwd, glm::vec3(0, 1, 0));

    const float aspect = (float)Window::GetPhysicalWidth() / (float)Window::GetPhysicalHeight();
    const float hFov = glm::radians(90.0f);
    const float vFov = 2.0f * std::atan(std::tan(hFov * 0.5f) / (aspect > 0 ? aspect : 1.0f));
    glm::mat4 proj = glm::perspectiveLH_ZO(vFov, aspect, 1.0f, 20000.0f);
    glm::mat4 mvp  = proj * view;

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
    gpu.setViewport(rp, 0.0f, 0.0f,
                    (float)Window::GetPhysicalWidth(), (float)Window::GetPhysicalHeight(), 0.0f, 1.0f);

    gpu.bindGraphicsPipeline(rp, m_pipeline);
    gpu.pushVertexUniformData(cmdBuffer, 0, &mvp, sizeof(mvp));
    GpuBufferBinding vb{ m_vertexBuffer, 0 };
    gpu.bindVertexBuffers(rp, 0, &vb, 1);

    // One draw per wall-texture group.
    int groups = DG_WallGroupCount();
    for (int g = 0; g < groups; g++) {
        int texid, first, count;
        DG_WallGroup(g, &texid, &first, &count);
        if (count <= 0) continue;
        GpuTextureHandle tex = wallTexture(texid);
        if (!tex) continue;
        GpuTextureSamplerBinding tsb{ tex, g_wallSampler };
        gpu.bindFragmentSamplers(rp, 0, &tsb, 1);
        gpu.drawPrimitives(rp, (uint32_t)count, 1, (uint32_t)first, 0);
    }

    gpu.endRenderPass(rp);
}

void DoomRenderPass::release(bool /*logRelease*/) {
    IGpu& gpu = Renderer::GetGpu();
    if (m_vertexBuffer)             { gpu.releaseBuffer(m_vertexBuffer); m_vertexBuffer = 0; }
    if (m_depth_texture.gpuTexture) { gpu.releaseTexture(m_depth_texture.gpuTexture); m_depth_texture.gpuTexture = 0; }
    m_pipeline = 0;
}
