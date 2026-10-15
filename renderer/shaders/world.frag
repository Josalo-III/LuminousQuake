#version 450
// Palette-indexed sampling preserves Quake's fullbrights and palette shading.
// Sky/water conventions: GLQuake, Copyright (C) 1996-1997 Id Software, Inc.
// GPL-2.0-or-later.
layout(set=2,binding=0) uniform sampler2D indices;
layout(set=2,binding=1) uniform sampler2D colormap;
layout(set=2,binding=2) uniform sampler2D lightmap;
layout(set=2,binding=3) uniform samplerCubeArrayShadow shadow_maps;
layout(set=2,binding=4) uniform samplerCubeArray shadow_depths; // same cubes, raw depth
layout(set=2,binding=5) uniform sampler2D bounce; // four stacked style layers, brightness/255
layout(set=3,binding=0,std140) uniform Surface {
    vec4 light_weights;
    vec4 parameters; // time, surface flags, smooth filter, mip mode
    vec4 camera_origin;
    vec4 dynamic_lights[32];
    uvec4 light_bits;
    vec4 bounce_weights; // live style value per bounce layer
} surface;
// Enhanced runtime lights, pushed once per frame. Each slot owns a shadow cube.
layout(set=3,binding=1,std140) uniform Lights {
    vec4 position_range[32];
    vec4 color_strength[32];
    vec4 falloff[32]; // light value, attenuation, angle scale, formula
    vec4 spot[32]; // direction, outer cosine (< -1: omnidirectional)
    vec4 shape[32]; // inner cosine, emitter radius
    uvec4 parameters; // slot count, Enhanced enabled, shadow resolution, shadows enabled
    vec4 tuning; // exposure, lightmap-to-layer row ratio, bounce enabled, bounce gain
} lights;
const float SHADOW_NEAR = 2.0;
layout(location=0) in vec2 uv;
layout(location=1) in vec2 light_uv;
layout(location=2) in vec3 world_position;
layout(location=3) in float shade;
layout(location=4) in vec3 world_normal;
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
// LIGHT/ericw-tools "delay" formulas; see LQ_LightFalloff.
float lightValue(vec4 f, float distance) {
    float scaled = f.y * distance;
    int formula = int(f.w + 0.5);
    if (formula == 0) return max(f.x - scaled, 0.0);
    if (formula == 1) return f.x * 128.0 / max(scaled, 1.0);
    if (formula == 2) return f.x * 16384.0 / max(scaled * scaled, 1.0);
    if (formula == 3) return f.x;
    if (formula == 5) return f.x * 16384.0 / ((scaled + 128.0) * (scaled + 128.0));
    return 0.0;
}
const vec2 DISK[16] = vec2[](
    vec2(-0.94201624,-0.39906216), vec2(0.94558609,-0.76890725), vec2(-0.09418410,-0.92938870),
    vec2(0.34495938,0.29387760), vec2(-0.91588581,0.45771432), vec2(-0.81544232,-0.87912464),
    vec2(-0.38277543,0.27676845), vec2(0.97484398,0.75648379), vec2(0.44323325,-0.97511554),
    vec2(0.53742981,-0.47373420), vec2(-0.26496911,-0.41893023), vec2(0.79197514,0.19090188),
    vec2(-0.24188840,0.99706507), vec2(-0.81409955,0.91437590), vec2(0.19984126,0.78641367),
    vec2(0.14383161,-0.14100790));
float majorAxis(vec3 v) { return max(max(abs(v.x), abs(v.y)), abs(v.z)); }
// Reversed depth the receiving plane would have along a tap direction. Wide
// filters then compare like with like on tilted receivers instead of meeting
// the far side of the receiver's own wall. Negative: the tap leaves the plane
// toward the light, so it cannot be shadowed by anything behind the receiver.
float planeReference(vec3 tap, vec3 origin, vec3 normal, vec3 point) {
    float toward = dot(normal, tap);
    if (toward >= -1e-4) return -1.0;
    return SHADOW_NEAR / majorAxis(tap * (dot(normal, point - origin) / toward));
}
// Reversed-depth cube shadows from an emitter of the given radius (PCSS): the
// mean depth of blockers found inside the largest possible penumbra sets the
// filter width radius * (receiver - blocker) / blocker, so contact shadows stay
// sharp and distant occluders soften. Comparisons are hardware filtered.
float shadowFactor(uint slot, vec3 origin, vec3 normal, float radius) {
    if (lights.parameters.w == 0u) return 1.0;
    float texel = 2.0 * majorAxis(world_position - origin) / float(lights.parameters.z);
    vec3 point = world_position + normal * texel;
    vec3 ray = point - origin;
    float distance = length(ray), major = majorAxis(ray);
    vec3 side = normalize(cross(ray, abs(ray.z) < 0.9 * distance ? vec3(0,0,1) : vec3(1,0,0)));
    vec3 up = normalize(cross(ray, side));
    // A world-anchored rotation turns sampling bands into stable fine grain.
    float angle = fract(sin(dot(floor(world_position), vec3(12.9898,78.233,37.719))) * 43758.5453) * 6.2831853;
    mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
    float search = max(min(radius * 3.0, distance * 0.5), texel), width = texel;
    if (radius > 0.0) {
        float blockers = 0.0, count = 0.0;
        for (int i = 0; i < 16; ++i) {
            vec2 offset = rotation * DISK[i] * search;
            vec3 tap = ray + side * offset.x + up * offset.y;
            float reference = planeReference(tap, origin, normal, point);
            if (reference < 0.0) continue;
            float stored = texture(shadow_depths, vec4(tap, float(slot))).r;
            if (stored > reference * 1.0005) { blockers += SHADOW_NEAR / stored; count += 1.0; }
        }
        if (count == 0.0) return 1.0;
        float blocker = blockers / count;
        width = clamp(radius * max(major - blocker, 0.0) / max(blocker, SHADOW_NEAR), texel, search);
    }
    float sum = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 offset = rotation * DISK[i] * width;
        vec3 tap = ray + side * offset.x + up * offset.y;
        float reference = planeReference(tap, origin, normal, point);
        sum += reference < 0.0 ? 1.0 : texture(shadow_maps, vec4(tap, float(slot)), reference * 1.0005);
    }
    return sum / 16.0;
}
void main() {
    int flags = int(surface.parameters.y);
    vec2 sample_uv = uv;
    float index_value, grade = 0;
    vec3 direct_tint = vec3(1.0);
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
            float brightness = 0.0;
            if (lights.parameters.y != 0u) {
                vec3 normal = normalize(world_normal);
                vec3 illumination = vec3(0.0);
                for (uint i = 0u; i < min(lights.parameters.x,32u); ++i) {
                    if ((surface.light_bits.x & (1u << i)) == 0u) continue;
                    vec4 origin = lights.position_range[i];
                    vec3 direction = origin.xyz-world_position;
                    float distance = length(direction);
                    if (distance >= origin.w) continue;
                    direction = distance > 0.001 ? direction/distance : normal;
                    float facing = dot(normal,direction);
                    if (facing <= 0.0) continue; // LIGHT rejects sources behind a face
                    vec4 f = lights.falloff[i];
                    /* LIGHT's softened incidence: half the value at grazing. */
                    float direct = lightValue(f,distance) * ((1.0 - f.z) + f.z * facing);
                    vec4 cone = lights.spot[i];
                    if (cone.w >= -1.0) {
                        float along = dot(-direction,cone.xyz);
                        if (along <= cone.w) continue;
                        float inner = lights.shape[i].x;
                        if (inner > cone.w) direct *= clamp((along-cone.w)/(inner-cone.w),0.0,1.0);
                    }
                    direct *= lights.color_strength[i].w;
                    if (direct < 0.01) continue;
                    direct *= shadowFactor(i,origin.xyz,normal,lights.shape[i].y);
                    illumination += lights.color_strength[i].rgb * direct;
                }
                // Static-world bounce from those lights, driven by live styles.
                if (lights.tuning.z != 0.0) {
                    float row = light_uv.y * lights.tuning.y;
                    for (int k = 0; k < 4; ++k) if (surface.bounce_weights[k] > 0.0)
                        illumination += texture(bounce, vec2(light_uv.x, (row + float(k)) * 0.25)).rgb *
                            (255.0 * surface.bounce_weights[k] * lights.tuning.w);
                }
                illumination *= lights.tuning.x;
                brightness = max(illumination.r,max(illumination.g,illumination.b));
                if (brightness > 0.001) direct_tint = illumination/brightness;
            } else {
                brightness = dot(texture(lightmap, light_uv), surface.light_weights) * 255.0;
                for (int i = 0; i < 32; ++i)
                    if ((surface.light_bits.x & (1u << i)) != 0u)
                        brightness += max(surface.dynamic_lights[i].w - length(world_position - surface.dynamic_lights[i].xyz), 0.0);
            }
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
    if (lights.parameters.y != 0u && index < 224.0) color.rgb *= direct_tint;
}
