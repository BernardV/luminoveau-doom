#version 450

// Doom sky background: one sky-texture width per 90° of yaw (matches the
// software renderer's ANGLETOSKYSHIFT mapping), full-bright. Sampled by view
// direction; drawn behind the world so it shows only through sky openings.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D sky_tex;

layout(set = 3, binding = 0) uniform SkyParams {
    float yawTurns;   // yaw / (pi/2): texture-widths of horizontal offset
    float uSpan;      // horizontal FOV / (pi/2): texture-widths across the screen
    float vScale;     // maps screen-y (0..1) into sky-texture v
    float vBias;
} sky;

void main() {
    // Left of screen = higher Doom angle, so subtract the screen-x contribution.
    float u = sky.yawTurns - (v_uv.x - 0.5) * sky.uSpan;
    float v = v_uv.y * sky.vScale + sky.vBias;
    out_color = vec4(texture(sky_tex, vec2(u, v)).rgb, 1.0);
}
