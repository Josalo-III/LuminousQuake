#version 450
// Palette-indexed sampling preserves Quake's fullbrights and palette shading.
// Sky/water conventions: GLQuake, Copyright (C) 1996-1997 Id Software, Inc.
// GPL-2.0-or-later.
layout(set=2,binding=0) uniform sampler2D indices;
layout(set=2,binding=1) uniform sampler2D colormap;
layout(set=2,binding=2) uniform sampler2D lightmap;
layout(set=3,binding=0,std140) uniform Surface {
    vec4 light_weights;
    vec4 parameters; // time, surface flags, smooth filter, mip mode
    vec4 camera_origin;
    vec4 dynamic_lights[32];
    uvec4 light_bits;
} surface;
layout(location=0) in vec2 uv;
layout(location=1) in vec2 light_uv;
layout(location=2) in vec3 world_position;
layout(location=3) in float shade;
layout(location=0) out vec4 color;
vec4 paletteColor(ivec2 p, int level, float grade) {
    ivec2 size = textureSize(indices, level);
    p = (p % size + size) % size;
    float entry = floor(texelFetch(indices, p, level).r * 255.0 + 0.5);
    return texture(colormap, vec2((entry + 0.5) / 256.0, (grade + 0.5) / 64.0));
}
vec4 levelColor(vec2 coord, int level, float grade) {
    vec2 pixel = coord * vec2(textureSize(indices, level));
    if (surface.parameters.z < 0.5 && surface.parameters.w < 1.5) return paletteColor(ivec2(floor(pixel)), level, grade);
    pixel -= 0.5;
    ivec2 base = ivec2(floor(pixel)); vec2 weight = fract(pixel);
    return mix(mix(paletteColor(base,level,grade), paletteColor(base+ivec2(1,0),level,grade),weight.x),
               mix(paletteColor(base+ivec2(0,1),level,grade), paletteColor(base+ivec2(1,1),level,grade),weight.x),weight.y);
}
vec4 mipColor(vec2 coord, float lod, float grade, int maxLevel) {
    if (surface.parameters.w < 1.5) return levelColor(coord,int(floor(lod+0.5)),grade);
    int lower = int(floor(lod));
    return mix(levelColor(coord,lower,grade),levelColor(coord,min(lower+1,maxLevel),grade),fract(lod));
}
vec4 filteredColor(vec2 coord, vec2 dx, vec2 dy, float grade) {
    if (surface.parameters.w < 0.5) return levelColor(coord,0,grade);
    vec2 size = vec2(textureSize(indices,0));
    dx *= size; dy *= size;
    // Principal axes of the pixel footprint in texture space.
    float a = dx.x*dx.x+dy.x*dy.x, b = dx.x*dx.y+dy.x*dy.y;
    float c = dx.y*dx.y+dy.y*dy.y;
    float disc = sqrt(max((a-c)*(a-c)+4.0*b*b,0.0));
    float major = sqrt(max((a+c+disc)*0.5,0.0));
    float minor = sqrt(max((a+c-disc)*0.5,0.0));
    vec2 axis = abs(b) > 0.00001 ? normalize(vec2(b,(a+c+disc)*0.5-a)) : (a >= c ? vec2(1,0) : vec2(0,1));
    float footprint = max(max(minor,major/clamp(surface.camera_origin.w,1.0,16.0)),1.0);
    int taps = int(clamp(ceil(major/footprint),1.0,16.0));
    int maxLevel = int(floor(log2(max(size.x,size.y))));
    float lod = clamp(log2(footprint),0.0,float(maxLevel));
    vec4 result = vec4(0);
    for (int i = 0; i < taps; ++i) {
        vec2 offset = axis*major*((float(i)+0.5)/float(taps)-0.5)/size;
        result += mipColor(coord+offset,lod,grade,maxLevel);
    }
    return result/float(taps);
}
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
    vec2 dx = dFdx(sample_uv), dy = dFdy(sample_uv);
    float index = floor(index_value * 255.0 + 0.5);
    if ((flags & 8) != 0 && index > 254.5) discard;
    if ((flags & 4) != 0) grade = clamp(shade, 0.0, 63.0);
    color = texture(colormap, vec2((index + 0.5) / 256.0, (grade + 0.5) / 64.0));
    // Resolve colors before interpolation. Sky, masks and UI keep level zero.
    if ((flags & 9) == 0)
        color = filteredColor(sample_uv, dx, dy, grade);
}
