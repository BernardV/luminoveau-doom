#version 450

// Doom sky as a 3D dome: reconstruct the true per-pixel view ray (perspective-
// correct), map it spherically onto the sky texture (one texture width per 90° of
// azimuth, matching Doom), and add a subtle atmospheric haze near the horizon.
// Unlike a flat screen-space map, the sky now curves naturally and converges
// toward the zenith when you look up, so it reads as a dome rather than a wall.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D sky_tex;

layout(set = 3, binding = 0) uniform SkyParams {
    float yaw;     // camera yaw (radians, Doom angle)
    float pitch;   // camera pitch (radians, + = up)
    float tanH;    // tan(hFov/2)
    float tanV;    // tan(vFov/2)
    float vScale;  // sky texture-V per radian of elevation
    float vBias;   // sky texture-V at the horizon (elevation 0)
    float haze;    // atmospheric horizon-haze amount (0 = off)
} sky;

void main() {
    // View ray through this pixel, in world space. Screen right maps to -azimuth
    // to match Doom's angle convention (turning right scrolls the sky left).
    float sx = -(v_uv.x - 0.5) * 2.0 * sky.tanH;
    float sy =  (v_uv.y - 0.5) * 2.0 * sky.tanV;
    float cy = cos(sky.yaw),   sya = sin(sky.yaw);
    float cp = cos(sky.pitch), spa = sin(sky.pitch);
    vec3 fwd   = vec3(cy * cp, spa, sya * cp);
    vec3 right = vec3(-sya, 0.0, cy);
    vec3 up    = cross(right, fwd);
    vec3 ray   = normalize(fwd + sx * right + sy * up);

    // Spherical mapping: azimuth → U (4 wraps per 360°), elevation → V.
    float az = atan(ray.z, ray.x);
    float el = asin(clamp(ray.y, -1.0, 1.0));
    float u  = az / 1.5707963;
    float v  = sky.vBias - el * sky.vScale;
    vec3  col = texture(sky_tex, vec2(u, v)).rgb;

    // Atmospheric haze: blend toward a pale sky tint near the horizon (el≈0),
    // fading out toward the zenith — a cheap aerial-perspective depth cue.
    if (sky.haze > 0.0) {
        float h = sky.haze * (1.0 - smoothstep(0.0, 0.45, abs(el)));
        col = mix(col, vec3(0.60, 0.65, 0.72), h * 0.35);
    }
    out_color = vec4(col, 1.0);
}
