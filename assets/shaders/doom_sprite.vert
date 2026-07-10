#version 450

// Billboard sprite vertices. CPU builds camera-facing quads (pos already in
// world space); we just transform by the MVP. UV + shade passed through.

layout(location = 0) in vec3  in_pos;
layout(location = 1) in vec2  in_uv;
layout(location = 2) in float in_shade;

layout(location = 0) out vec2  v_uv;
layout(location = 1) out float v_shade;

layout(set = 1, binding = 0) uniform ViewProj {
    mat4 mvp;
} vp;

void main() {
    gl_Position = vp.mvp * vec4(in_pos, 1.0);
    v_uv    = in_uv;
    v_shade = in_shade;
}
