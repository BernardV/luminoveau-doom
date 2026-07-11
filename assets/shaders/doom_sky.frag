#version 450

// Doom sky background: one sky-texture width per 90° of yaw (matches the
// software renderer's ANGLETOSKYSHIFT mapping), full-bright. Vertically it maps
// each pixel's view angle (pitch + screen position) into the sky texture so the
// sky pans correctly with mouselook — texture top = up, bottom = horizon/mtns.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D sky_tex;

layout(set = 3, binding = 0) uniform SkyParams {
    float yawTurns;   // yaw / (pi/2): texture-widths of horizontal offset
    float uSpan;      // horizontal FOV / (pi/2): texture-widths across the screen
    float pitch;      // view pitch, radians (+ = looking up)
    float vFov;       // vertical FOV, radians
    float vScale;     // sky-texture-V per radian of vertical view angle
    float vBias;      // sky-texture-V at the horizon (view angle 0)
} sky;

void main() {
    // Left of screen = higher Doom angle, so subtract the screen-x contribution.
    float u = sky.yawTurns - (v_uv.x - 0.5) * sky.uSpan;
    // v_uv.y = 0 at the bottom of the view, 1 at the top. Vertical view angle of
    // this pixel (+ = up); looking up moves toward the top of the sky texture.
    float ang = sky.pitch + (v_uv.y - 0.5) * sky.vFov;
    float v   = sky.vBias - ang * sky.vScale;
    out_color = vec4(texture(sky_tex, vec2(u, v)).rgb, 1.0);
}
