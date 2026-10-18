#version 450
// Software actor/UI compositing bridge. GPL-2.0-or-later.
layout(set=2,binding=0) uniform sampler2D overlay;
// x: gamma / 2.2, the exponent from the UI's display values to the frame's
// (the frame holds display values on SDR and HDR alike).
layout(set=3,binding=0,std140) uniform Overlay { vec4 transfer; } overlay_out;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
void main() {
    color = texture(overlay, uv);
    color.rgb = pow(max(color.rgb, vec3(0.0)), vec3(overlay_out.transfer.x));
}
