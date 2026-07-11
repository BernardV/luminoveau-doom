#version 450

// Bloom composite: sample the offscreen 3D scene, add a soft glow extracted from
// its bright regions (lights, sky, lava, muzzle flash), and write to the target.
// strength == 0 → exact passthrough (Authentic+ / crisp mode, no bloom).

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D scene_tex;

layout(set = 3, binding = 0) uniform BloomParams {
    vec2  texel;      // 1/width, 1/height of scene_tex (blur step size)
    float strength;   // glow amount; 0 disables (crisp mode)
    float threshold;  // brightness cutoff for the bright-pass
    float vignette;   // corner darkening amount (0 = off)
    float tonemap;    // filmic highlight rolloff amount (0 = off)
} p;

// ACES-ish filmic curve — rolls off bright (post-bloom) highlights smoothly
// instead of hard-clipping to white, for a more cinematic Modern look.
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Keep only the portion of a texel's colour above the brightness threshold.
vec3 brightPass(vec2 uv) {
    vec3 c = texture(scene_tex, uv).rgb;
    float l = max(max(c.r, c.g), c.b);
    float k = max(l - p.threshold, 0.0) / max(l, 1e-4);
    return c * k;
}

void main() {
    // scene_tex was rendered as a colour target; sampling it flips vertically vs
    // the direct-to-screen path, so flip V here (the world was upside-down).
    vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);

    vec3 base = texture(scene_tex, uv).rgb;
    if (p.strength <= 0.0) { out_color = vec4(base, 1.0); return; }

    // Cheap two-ring radial blur of the bright-pass — enough for a soft halo
    // without a separable/downsampled chain.
    vec3  sum  = brightPass(uv) * 4.0;
    float wsum = 4.0;
    const int N = 12;
    for (int i = 0; i < N; i++) {
        float a = 6.2831853 * float(i) / float(N);
        vec2 dir = vec2(cos(a), sin(a));
        sum += brightPass(uv + dir * p.texel * 3.0) * 2.0; wsum += 2.0;   // inner
        sum += brightPass(uv + dir * p.texel * 6.0) * 1.0; wsum += 1.0;   // outer
    }
    vec3 bloom = sum / wsum;
    vec3 col = base + bloom * p.strength;

    // Filmic rolloff (blend toward the tonemapped curve so base LDR stays close
    // but post-bloom highlights don't harshly clip).
    if (p.tonemap > 0.0) col = mix(col, aces(col), p.tonemap);

    // Vignette: a subtle darkening toward the extreme corners only. Kept gentle so
    // it doesn't frame the (now full-window, wide) view with dark side bands.
    if (p.vignette > 0.0) {
        vec2  d  = v_uv - 0.5;
        float r2 = dot(d, d);                          // 0 centre .. 0.5 corner
        float v  = smoothstep(0.5, 0.18, r2 * p.vignette);
        col *= mix(1.0, v, 0.3);
    }

    out_color = vec4(col, 1.0);
}
