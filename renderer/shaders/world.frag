#version 450
// Palette-indexed sampling preserves Quake's fullbrights and palette shading.
// Sky/water conventions: GLQuake, Copyright (C) 1996-1997 Id Software, Inc.
// GPL-2.0-or-later.
layout(set=2,binding=0) uniform sampler2D indices;
layout(set=2,binding=1) uniform sampler2D colormap;
layout(set=2,binding=2) uniform sampler2D lightmap;
layout(set=3,binding=0,std140) uniform Surface {
    vec4 light_weights;
    vec4 parameters; // time, surface flags, reserved, reserved
    vec4 camera_origin;
    vec4 dynamic_lights[32];
    uvec4 light_bits;
} surface;
layout(location=0) in vec2 uv;
layout(location=1) in vec2 light_uv;
layout(location=2) in vec3 world_position;
layout(location=3) in float shade;
layout(location=0) out vec4 color;
void main() {
    int flags = int(surface.parameters.y);
    vec2 sample_uv = uv;
    float index_value, grade = 0;
    if ((flags & 1) != 0) {
        vec3 direction = world_position - surface.camera_origin.xyz;
        direction.z *= 3.0;
        direction *= 378.0 / max(length(direction), 0.001);
        vec2 back_uv = fract((direction.xy + surface.parameters.x * 8.0) / 128.0);
        vec2 front_uv = fract((direction.xy + surface.parameters.x * 16.0) / 128.0);
        float back = texture(indices, vec2(back_uv.x * 0.5 + 0.5, back_uv.y)).r;
        float front = texture(indices, vec2(front_uv.x * 0.5, front_uv.y)).r;
        index_value = front > 0.001 ? front : back;
    } else {
        if ((flags & 2) != 0)
            sample_uv += sin(uv.yx * 8.0 + surface.parameters.x) * 0.125;
        index_value = texture(indices, sample_uv).r;
        if ((flags & 2) == 0) {
            float brightness = dot(texture(lightmap, light_uv), surface.light_weights) * 255.0;
            for (int i = 0; i < 32; ++i)
                if ((surface.light_bits.x & (1u << i)) != 0u)
                    brightness += max(surface.dynamic_lights[i].w - length(world_position - surface.dynamic_lights[i].xyz), 0.0);
            grade = clamp(floor((255.0 - brightness) * 0.25), 0.0, 63.0);
        }
    }
    float index = floor(index_value * 255.0 + 0.5);
    if ((flags & 8) != 0 && index > 254.5) discard;
    if ((flags & 4) != 0) grade = clamp(shade, 0.0, 63.0);
    color = texture(colormap, vec2((index + 0.5) / 256.0, (grade + 0.5) / 64.0));
    // Filter palette colors, never the numeric palette indices. UI/masked
    // sprites remain crisp and their transparent palette entry stays exact.
    if (surface.parameters.z > 0.5 && (flags & 9) == 0) {
        ivec2 size = textureSize(indices, 0);
        vec2 pixel = sample_uv * vec2(size) - 0.5;
        ivec2 base = ivec2(floor(pixel));
        vec2 weight = fract(pixel);
        vec4 samples[4];
        for (int i = 0; i < 4; ++i) {
            ivec2 p = base + ivec2(i & 1, i >> 1);
            p = (p % size + size) % size;
            float entry = floor(texelFetch(indices, p, 0).r * 255.0 + 0.5);
            samples[i] = texture(colormap, vec2((entry + 0.5) / 256.0, (grade + 0.5) / 64.0));
        }
        color = mix(mix(samples[0], samples[1], weight.x), mix(samples[2], samples[3], weight.x), weight.y);
    }
}
