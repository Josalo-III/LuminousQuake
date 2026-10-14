#version 450
// LuminousQuake GPU world vertex shader. GPL-2.0-or-later.
layout(location=0) in vec3 in_position;
layout(location=1) in vec2 in_uv;
layout(location=2) in vec2 in_light_uv;
layout(location=3) in float in_shade;
layout(set=1,binding=0,std140) uniform Camera { mat4 view_projection; } camera;
layout(location=0) out vec2 uv;
layout(location=1) out vec2 light_uv;
layout(location=2) out vec3 world_position;
layout(location=3) out float shade;
void main() {
    gl_Position = camera.view_projection * vec4(in_position, 1.0);
    uv = in_uv; light_uv = in_light_uv; world_position = in_position;
    shade = in_shade;
}
