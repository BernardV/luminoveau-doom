// DoomRenderPass — game-side custom Luminoveau RenderPass for the GPU Doom
// renderer (see plan.md). Fase 0: scaffolding only — draws a single colored
// triangle from its own graphics pipeline, built from GLSL shaders loaded via
// AssetHandler::GetShader (auto-transpiled to SPIRV/Metal/WGSL per backend).
// Later phases grow this into the real world renderer (walls/flats/sprites).
#pragma once

#include "gpu/renderpass.h"
#include "gpu/types.h"

class DoomRenderPass : public RenderPass {
public:
    bool init(GpuTextureFormat swapchainFormat,
              uint32_t surfaceWidth, uint32_t surfaceHeight,
              std::string name, bool logInit = true,
              size_t capacity = 0, bool forceNoMSAA = false) override;

    void release(bool logRelease = true) override;

    void render(GpuCmdBufferHandle cmdBuffer,
                GpuTextureHandle   targetTexture,
                const glm::mat4&   camera) override;

    // Unused base-class hooks (this pass pulls its data directly, like Model3DRenderPass).
    void addToRenderQueue(const Renderable& /*renderable*/) override {}
    void resetRenderQueue() override {}
    UniformBuffer& getUniformBuffer() override { static UniformBuffer dummy; return dummy; }

private:
    GpuBufferHandle m_vertexBuffer = 0;
    uint32_t        m_vertexCount  = 0;
    GpuSampleCount  m_sampleCount  = GpuSampleCount::x1;
};
