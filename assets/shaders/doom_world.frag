#version 450

// Fase 1b: sample the wall texture and modulate by the sector-light shade.
// SDL_shadercross convention: fragment samplers live in descriptor set 2.

layout(location = 0) in vec2  v_uv;
layout(location = 1) in float v_shade;
layout(location = 2) in float v_dist;
layout(location = 3) in vec3  v_worldpos;

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D wall_tex;

// Lighting: the muzzle-flash/light-amp scalar (a point light at the eye) plus a
// small set of dynamic coloured point lights (projectiles, torches). Set 3.
#define DG_MAX_LIGHTS 16
layout(set = 3, binding = 0) uniform Lighting {
    vec4  lightPosRad[DG_MAX_LIGHTS];  // xyz = world pos, w = radius
    vec4  lightColor[DG_MAX_LIGHTS];   // rgb = colour
    float flash;
    int   count;
    float authentic;   // >0 = Doom colormap-style banded lighting
} u;

// Sum coloured contribution from the dynamic lights at a world position.
vec3 dynamicLights(vec3 p) {
    vec3 acc = vec3(0.0);
    for (int i = 0; i < u.count; i++) {
        float rad = u.lightPosRad[i].w;
        float d   = length(u.lightPosRad[i].xyz - p);
        // Smooth (C1-continuous) falloff that reaches exactly 0 at d == rad, so a
        // light popping in/out of the set at that distance causes no visible jump.
        float a   = 1.0 - clamp(d / rad, 0.0, 1.0);
        acc += u.lightColor[i].rgb * (a * a * (3.0 - 2.0 * a));   // smoothstep
    }
    return acc;
}

void main() {
    vec4 tex = texture(wall_tex, v_uv);
    // Doom-like distance diminishing: brighter up close, fading with range,
    // floored so it never goes fully black. Combined with sector-light shade.
    float dim = clamp(1.25 - v_dist / 2200.0, 0.25, 1.0);
    float flashAdd = u.flash * clamp(1.0 - v_dist / 500.0, 0.0, 1.0);
    float bright = min(v_shade * dim + flashAdd, 1.0);
    // Authentic+ colormap look: quantize into Doom-like discrete light steps.
    if (u.authentic > 0.5) bright = floor(bright * 16.0 + 0.5) / 16.0;
    vec3 lit = tex.rgb * bright + tex.rgb * dynamicLights(v_worldpos);
    out_color = vec4(min(lit, vec3(1.0)), 1.0);
}
