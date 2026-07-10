// DoomRenderPass — game-side custom Luminoveau RenderPass for the GPU Doom
// renderer (see plan.md). Fase 1: renders the level's walls as camera-projected
// quads (untextured, shaded by sector light) with a depth buffer, from geometry
// + view params bridged out of the Doom core (dg_render.c). Textures, flats,
// sprites, lighting come in later phases.
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
    void onResize(uint32_t surfaceWidth, uint32_t surfaceHeight) override;

    void render(GpuCmdBufferHandle cmdBuffer,
                GpuTextureHandle   targetTexture,
                const glm::mat4&   camera) override;

    void addToRenderQueue(const Renderable& /*renderable*/) override {}
    void resetRenderQueue() override {}
    UniformBuffer& getUniformBuffer() override { static UniformBuffer dummy; return dummy; }

private:
    void ensureGeometry();          // (re)upload wall geometry when the level changes
    void createDepth(uint32_t w, uint32_t h);

    GpuBufferHandle m_vertexBuffer = 0;
    uint32_t        m_vertexBytes  = 0;   // current buffer capacity in bytes
    uint32_t        m_vertexCount  = 0;   // vertices to draw
    unsigned        m_geomVersion  = 0xffffffffu;
    GpuSampleCount  m_sampleCount  = GpuSampleCount::x1;
    uint32_t        m_depthW = 0, m_depthH = 0;

    GpuGraphicsPipelineHandle m_skyPipeline = 0;    // fullscreen sky background

    // Sprites (billboards): rebuilt + re-uploaded each frame.
    GpuGraphicsPipelineHandle m_spritePipeline = 0;
    GpuBufferHandle m_spriteVB = 0;
    uint32_t        m_spriteVBBytes = 0;
    GpuSamplerHandle m_spriteSampler = 0;
    // Build billboards + upload their textures/VB (call BEFORE beginRenderPass).
    void prepareSprites(float camRightX, float camRightZ);
    // Issue sprite draws (call INSIDE the render pass).
    void drawSprites(GpuCmdBufferHandle cmd, GpuRenderPassHandle rp,
                     const void* vpu, uint32_t vpuSize);
    uint32_t m_spriteDrawCount = 0;   // sprites ready to draw this frame
};
