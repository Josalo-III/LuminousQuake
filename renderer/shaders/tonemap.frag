#version 450
// Display transfer for the floating-point frame. GPL-2.0-or-later.
// Luminous frames hold linear light (1 = Quake's palette colour at full white
// on an SDR display); Classic frames hold palette colours as they display.
// Gamma (lq_gamma, 2.2 normally) sets how display values become light. Luminous
// highlights roll off on a shoulder that leaves everything below `shoulder`
// alone and reaches the ceiling (1 on SDR, the display's headroom on HDR) at
// `white`, keeping each colour's hue.
layout(set=2,binding=0) uniform sampler2D scene;
// The result is display values (light to the 1/2.2), as an SDR frame holds;
// on HDR they continue past 1, so the UI still blends as it was drawn to. A
// final pass (tone.x = 2) turns them into linear light for the HDR swapchain.
layout(set=3,binding=0,std140) uniform Display {
    vec4 tone;   // x: 1 for Luminous, 2 for the final HDR decode; y: gamma / 2.2, z: ceiling, w: white point
    vec4 target; // x: 1 to let values pass SDR white (HDR), y: gamma, z: shoulder, w: Quake's gamma (Luminous)
    vec4 ramp;   // x: 1 shows the tone ramp (lq_tone_ramp); y, z: its band's top and bottom (0-1 down the frame)
} display;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
float shoulder(float x, float s, float ceiling, float white) {
    if (x <= s) return x;
    if (x >= white) return ceiling;
    float u = (x - s) / (ceiling - s), w = (white - s) / (ceiling - s);
    return s + (ceiling - s) * u * (1.0 + u / (w * w)) / (1.0 + u);
}
void main() {
    vec3 c = max(texture(scene, uv).rgb, vec3(0.0));
    if (display.tone.x > 2.5) {
        // lq_hdr_test: SDR white along the top; below, bands at fixed multiples
        // of it, in display values the final HDR pass turns back into exactly
        // these light levels. Where the display stops, the bands stop changing.
        // Up to 16, the most headroom the game uses: a MacBook XDR showed every band
        // distinct when they stopped at 10 (October 31).
        const float levels[10] = float[10](0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0);
        float level = uv.y < 0.15 ? 1.0 : (uv.y < 0.2 || fract(uv.x * 10.0) < 0.04) ? 0.0
            : levels[min(int(uv.x * 10.0), 9)];
        if (display.target.x < 0.5) level = min(level, 1.0);
        color = vec4(vec3(pow(level, 1.0 / 2.2)), 1.0);
        return;
    }
    if (display.tone.x > 1.5) { color = vec4(pow(c, vec3(2.2)), 1.0); return; }
    vec3 light;
    // The tone ramp: a band of known light levels in place of the scene, put
    // through the same curve, so the shoulder's effect can be judged while it
    // is adjusted. Left to right, 1/32 to 16 times the ceiling on a log scale:
    // the display's range, which the settings don't move, so the shoulder and
    // white point markers travel across it; the upper half grey, the
    // lower half lava orange to show the hue
    // kept as highlights roll off. Markers: green at the shoulder, blue at
    // the ceiling (SDR white, or the HDR headroom), red at the white point.
    float ramp_lo = log2(max(display.tone.z, 1.0)) - 5.0, ramp_hi = ramp_lo + 9.0;
    bool ramp = display.tone.x > 0.5 && display.ramp.x > 0.5 && uv.y >= display.ramp.y && uv.y < display.ramp.z;
    if (ramp) {
        float half_band = (uv.y - display.ramp.y) / (display.ramp.z - display.ramp.y);
        vec3 hue = half_band < 0.5 ? vec3(1.0) : vec3(1.0, 0.45, 0.12);
        // Linear light at the level for this column, before the curve.
        light = hue * exp2(mix(ramp_lo, ramp_hi, uv.x));
    }
    if (display.tone.x > 0.5) {
        if (!ramp) light = pow(c, vec3(display.tone.y));
        float peak = max(light.r, max(light.g, light.b));
        if (peak > display.target.z)
            light *= shoulder(peak, display.target.z, display.tone.z, display.tone.w) / peak;
        // Quake's gamma (the brightness slider) bends display values up to SDR
        // white as software Quake's gammatable does: display^g, so light^g.
        // HDR highlights above white keep their level.
        if (display.target.w > 0.0 && display.target.w != 1.0)
            light = mix(light, pow(max(light, vec3(0.0)), vec3(display.target.w)), lessThanEqual(light, vec3(1.0)));
    } else {
        light = pow(min(c, vec3(1.0)), vec3(display.target.y));
    }
    light = display.target.x > 0.5 ? max(light, vec3(0.0)) : clamp(light, 0.0, 1.0);
    if (ramp) {
        float width = 2.0 * fwidth(uv.x);
        float at_shoulder = (log2(max(display.target.z, 1e-6)) - ramp_lo) / (ramp_hi - ramp_lo);
        float at_ceiling = (log2(max(display.tone.z, 1e-6)) - ramp_lo) / (ramp_hi - ramp_lo);
        float at_white = (log2(max(display.tone.w, 1e-6)) - ramp_lo) / (ramp_hi - ramp_lo);
        if (abs(uv.x - at_shoulder) < width) light = vec3(0.0, 1.0, 0.0);
        else if (abs(uv.x - at_ceiling) < width) light = vec3(0.0, 0.35, 1.0);
        else if (abs(uv.x - at_white) < width) light = vec3(1.0, 0.0, 0.0);
    }
    color = vec4(pow(light, vec3(1.0 / 2.2)), 1.0);
}
