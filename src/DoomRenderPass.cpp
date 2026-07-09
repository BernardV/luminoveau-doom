#include "DoomRenderPass.h"

#include <cstring>

#include "core/log/log.h"
#include "gpu/IGpu.h"
#include "assets/assethandler.h"
#include "renderer/renderer.h"
#include "platform/window/window.h"

namespace {
// Interleaved pos(vec2) + color(vec3); a centered triangle in NDC.
struct TestVertex { float x, y, r, g, b; };
constexpr TestVertex kTriangle[] = {
    { 0.0f,  0.6f, 1.0f, 0.2f, 0.2f },
    {-0.6f, -0.5f, 0.2f, 1.0f, 0.3f },
    { 0.6f, -0.5f, 0.3f, 0.4f, 1.0f },
};
}

bool DoomRenderPass::init(GpuTextureFormat swapchainFormat,
                          uint32_t /*surfaceWidth*/, uint32_t /*surfaceHeight*/,
                          std::string name, bool logInit,
                          size_t /*capacity*/, bool /*forceNoMSAA*/) {
    passname      = std::move(name);
    m_sampleCount = Renderer::GetSampleCount();
    IGpu& gpu     = Renderer::GetGpu();

    // GLSL shaders → backend blobs, handled by the engine. GetShader returns a
    // ShaderAsset with a ready gpuShader handle + reflected bind counts.
    // PhysFS mounts the cwd (the dir containing assets/), so engine assets are
    // referenced with the "assets/" prefix — matches the engine's own examples.
    Shader vs = AssetHandler::GetShader("assets/shaders/doom_test.vert");
    Shader fs = AssetHandler::GetShader("assets/shaders/doom_test.frag");
    if (!vs.gpuShader || !fs.gpuShader) {
        LOG_ERROR("DoomRenderPass: shader load failed");
        return false;
    }

    static GpuVertexAttribute attrs[2] = {
        { .location = 0, .binding = 0, .format = GpuVertexElementFormat::Float2, .offset = 0 },
        { .location = 1, .binding = 0, .format = GpuVertexElementFormat::Float3, .offset = 8 },
    };
    static GpuVertexBinding vbind = { .binding = 0, .stride = sizeof(TestVertex), .instanceStepping = false };

    GpuGraphicsPipelineCreateInfo pci{};
    pci.vertexShader      = vs.gpuShader;
    pci.fragmentShader    = fs.gpuShader;
    pci.attributes        = attrs;
    pci.attributeCount    = 2;
    pci.bindings          = &vbind;
    pci.bindingCount      = 1;
    pci.fillMode          = GpuFillMode::Fill;
    pci.cullMode          = GpuCullMode::None;
    pci.frontFace         = GpuFrontFace::CounterClockwise;
    pci.colorTargetFormat = swapchainFormat;
    pci.hasDepthTarget    = false;
    pci.sampleCount       = m_sampleCount;
    m_pipeline = gpu.createGraphicsPipeline(pci);
    if (!m_pipeline) {
        LOG_ERROR("DoomRenderPass: pipeline creation failed");
        return false;
    }

    // Upload the triangle vertex buffer once.
    const uint32_t bytes = (uint32_t)sizeof(kTriangle);
    m_vertexBuffer = gpu.createBuffer({ bytes, GpuBufferUsage::Vertex });
    GpuTransferBufferHandle tb = gpu.createTransferBuffer({ bytes, GpuTransferUsage::Upload });
    if (!m_vertexBuffer || !tb) {
        LOG_ERROR("DoomRenderPass: vertex buffer creation failed");
        return false;
    }
    void* ptr = gpu.mapTransferBuffer(tb, false);
    std::memcpy(ptr, kTriangle, bytes);
    gpu.unmapTransferBuffer(tb);
    GpuCmdBufferHandle cmd = gpu.acquireCommandBuffer();
    gpu.uploadToBuffer(cmd, tb, 0, m_vertexBuffer, 0, bytes, false);
    gpu.submitCommandBuffer(cmd);
    gpu.releaseTransferBuffer(tb);
    m_vertexCount = 3;

    if (logInit) LOG_INFO("DoomRenderPass: initialized ({})", passname.c_str());
    return true;
}

void DoomRenderPass::release(bool /*logRelease*/) {
    IGpu& gpu = Renderer::GetGpu();
    if (m_vertexBuffer) { gpu.releaseBuffer(m_vertexBuffer); m_vertexBuffer = 0; }
    // Pipeline lifetime is managed by the backend/device teardown.
    m_pipeline = 0;
}

void DoomRenderPass::render(GpuCmdBufferHandle cmdBuffer,
                            GpuTextureHandle   targetTexture,
                            const glm::mat4&   /*camera*/) {
    if (!m_pipeline || !m_vertexBuffer) return;
    IGpu& gpu = Renderer::GetGpu();

    const bool shouldResolve = (renderTargetResolve != 0);

    GpuColorTargetInfo ct{};
    ct.texture        = targetTexture;
    ct.resolveTexture = renderTargetResolve;
    ct.loadOp         = color_target_info_loadop;   // Load: composite over prior passes
    ct.storeOp        = shouldResolve ? GpuStoreOp::Resolve : GpuStoreOp::Store;
    ct.clearR = color_target_clear_r; ct.clearG = color_target_clear_g;
    ct.clearB = color_target_clear_b; ct.clearA = color_target_clear_a;

    GpuRenderPassHandle rp = gpu.beginRenderPass(cmdBuffer, &ct, 1, nullptr);
    render_pass = rp;
    gpu.setViewport(rp, 0.0f, 0.0f,
                    (float)Window::GetPhysicalWidth(), (float)Window::GetPhysicalHeight(),
                    0.0f, 1.0f);

    gpu.bindGraphicsPipeline(rp, m_pipeline);
    GpuBufferBinding vb{ m_vertexBuffer, 0 };
    gpu.bindVertexBuffers(rp, 0, &vb, 1);
    gpu.drawPrimitives(rp, m_vertexCount, 1, 0, 0);

    gpu.endRenderPass(rp);
}
