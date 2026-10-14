#version 450
// Software actor/UI compositing bridge. GPL-2.0-or-later.
layout(set=2,binding=0) uniform sampler2D overlay;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
void main() { color = texture(overlay, uv); }
