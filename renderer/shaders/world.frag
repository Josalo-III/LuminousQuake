#version 450
// Palette-indexed sampling preserves Quake's fullbrights and palette shading.
// Sky/water conventions: GLQuake, Copyright (C) 1996-1997 Id Software, Inc.
// GPL-2.0-or-later.
layout(set=2,binding=0) uniform sampler2D indices;
layout(set=2,binding=1) uniform sampler2D colormap;
layout(set=2,binding=2) uniform sampler2D lightmap;
// Shadow cubes in four arrays of 512, 256, 128 and 128 (see LQ_ShadowSlot);
// each is bound for depth comparison and for raw depth (blocker search).
layout(set=2,binding=3) uniform sampler2DArrayShadow shadow_maps0;
layout(set=2,binding=4) uniform sampler2DArray shadow_depths0;
layout(set=2,binding=5) uniform sampler2D bounce; // four stacked style layers, brightness/255
layout(set=2,binding=6) uniform sampler3D probe_static; // depth: (layer*6 + direction)*nz + z
layout(set=2,binding=7) uniform sampler3D probe_dynamic; // depth: direction*nz + z
layout(set=2,binding=8) uniform sampler2DArrayShadow shadow_maps1;
layout(set=2,binding=9) uniform sampler2DArray shadow_depths1;
layout(set=2,binding=10) uniform sampler2DArrayShadow shadow_maps2;
layout(set=2,binding=11) uniform sampler2DArray shadow_depths2;
layout(set=2,binding=12) uniform sampler2DArrayShadow shadow_maps3;
layout(set=2,binding=13) uniform sampler2DArray shadow_depths3;
// Community texture pack (Luminous): colour with its glow in alpha, and a
// DarkPlaces _norm map (tangent normal, green up the image; height in alpha).
// Sky surfaces take no pack images; with a skybox, pack_color holds its six
// faces side by side (Metal allows 16 samplers, all in use).
layout(set=2,binding=14) uniform sampler2D pack_color;
layout(set=2,binding=15) uniform sampler2D pack_normal;
layout(set=3,binding=0,std140) uniform Surface {
    vec4 light_weights;
    vec4 parameters; // time, surface flags, smooth filter, mip mode
    vec4 camera_origin;
    vec4 dynamic_lights[32];
    uvec4 light_bits; // Luminous: runtime-light slots reaching the face; Classic: dlights in x
    vec4 bounce_weights; // live style value per bounce layer
    vec4 face_light; // x: 1 + the slot brightest everywhere on this face (found on the CPU); 0 unknown
    uvec4 shadow_free; // slots whose shadow lookup here provably finds no blocker (LQ_SceneFindShadowFree)
    vec4 pack; // x: draw the pack's colour; y: shade direct light by its normal map; z: colour gain; w: gloss intensity (Luminous)
    vec4 relief; // x: parallax depth, as a fraction of the texture's size (0 off); y: height held at the face; z: relief shadows; w: cull pack detail by footprint
    vec4 entity; // x: a translucent entity's opacity (flag 4096)
} surface;
// Luminous runtime lights, pushed once per frame. Each slot owns a shadow cube.
layout(set=3,binding=1,std140) uniform Lights {
    vec4 position_range[128];
    vec4 color_strength[128];
    vec4 falloff[128]; // light value, attenuation, angle scale, formula
    vec4 spot[128]; // direction, outer cosine (< -1: omnidirectional)
    vec4 shape[128]; // inner cosine, emitter radius, submerged, shadow array*1024 + cube
    uvec4 parameters; // slot count, Luminous enabled, shadow arrays, shadows enabled
    vec4 tuning; // exposure, lightmap-to-layer row ratio, bounce enabled, bounce gain
    vec4 probe_origin; // xyz, spacing
    vec4 probe_size; // probes per axis; w: 1 static, 2 dynamic (bits)
    vec4 probe_weights; // live style value per static layer
    vec4 liquid; // x: translucent liquid opacity; zero when liquids are opaque or unlit
    vec4 sky; // rgb: sky brightness at full visibility (bounce/probe layer 0 alpha)
    vec4 sun_direction; // xyz: direction sunlight travels; w: incidence angle scale
    vec4 sun_light; // rgb: sunlight at normal incidence (bounce/probe layer 1 alpha: visibility)
    vec4 shadow_sizes; // cube face size per shadow array
    vec4 color_space; // x: exponent from summed brightness to linear light; w: fullbright emission (Luminous)
                      // y: faint level, z: dominance ratio below which lights cast no shadow (0 off)
    vec4 shadow_filter; // x: a light's brightness at which its shadow takes all 16 samples (0: always)
    vec4 limit[128]; // x: most brightness the light adds to a surface; 0 none (torch accents); yzw: shadow cube centre
    vec4 effects; // x: sprite fullbright emission (lq_sprite_emission); y: Quake's palette gamma
    vec4 palette_shift; // screen tints on pack colours: colour * (1 - w) + rgb
    vec4 fog; // rgb: fog colour 0-1; w: density per unit (FitzQuake's / 64), 0 none
    vec4 sky_fog; // x: share of fog over the sky; y: 1 with a skybox
} lights;
const float SHADOW_NEAR = 2.0;
layout(location=0) in vec2 uv;
layout(location=1) in vec2 light_uv;
layout(location=2) in vec3 world_position;
layout(location=3) in float shade;
layout(location=4) in vec3 world_normal;
layout(location=0) out vec4 color;
// Luminous shades in linear light at 32 bits. Each texel's palette colour
// (colormap row 32, Quake's normal brightness) is decoded to linear and lit
// before filtering; fullbrights emit their colour unlit. The frame is encoded
// for the display once, at the end of main. The transfer is a pure 2.2 power,
// the CRT gamma Quake's palette was drawn for. It matches sRGB except below
// about 4%, where sRGB's linear toe would crush the shadows Classic shows.
bool linear_lit = false;
bool written_linear = false; // colour holds linear light rather than display values
vec4 sky_color = vec4(0.0); // smooth-filtered sky; alpha 0 when unused
vec3 lit_radiance = vec3(1.0);
const float DISPLAY_GAMMA = 2.2;
vec3 displayDecode(vec3 c) { return pow(max(c, vec3(0.0)), vec3(DISPLAY_GAMMA)); }
vec3 displayEncode(vec3 c) { return pow(clamp(c, 0.0, 1.0), vec3(1.0 / DISPLAY_GAMMA)); }
// Palette colours to linear light. The game sends its palette through Quake's
// gamma (the brightness slider, lights.effects.y), as software Quake shows it.
// Luminous undoes that here and applies the gamma to the finished frame in
// tone mapping, where it bends what the display shows, light included, as
// software Quake's does, rather than only the textures' colour.
vec3 paletteDecode(vec3 c) { return pow(max(c, vec3(0.0)), vec3(DISPLAY_GAMMA / lights.effects.y)); }
// Fullbrights (palette 224-255: flames, lava, lit panels) emit. In Luminous
// they shine at color_space.w (3) times their palette colour in linear light:
// tone mapping rolls them back to about their colour on SDR, and HDR displays
// show them brighter than white.
float emission() { return lights.parameters.y != 0u && lights.color_space.w > 0.0 ? lights.color_space.w : 1.0; }
// Sprites (explosions, s_light) emit by their own gain, so they can reach
// further into HDR highlights than lava and lit panels.
// A pack colour in linear light, as the palette would have it: scaled by
// `gain` in linear light, then the screen tints the game mixes into its
// palette entries before they reach us (Quake's gamma comes at the end).
vec3 packAlbedo(vec3 encoded, float gain) {
    vec3 c = encoded * pow(gain, 1.0 / DISPLAY_GAMMA);
    return displayDecode(c * (1.0 - lights.palette_shift.w) + lights.palette_shift.rgb);
}
// A colour from outside the palette (skybox, fog) as the palette's colours
// reach the frame: through the screen tints, then Quake's gamma in Classic,
// where the palette carries it (Luminous applies it in tone mapping).
vec3 paletteLike(vec3 c) {
    c = c * (1.0 - lights.palette_shift.w) + lights.palette_shift.rgb;
    return lights.parameters.y != 0u ? c : pow(max(c, vec3(0.0)), vec3(lights.effects.y > 0.0 ? lights.effects.y : 1.0));
}
// A skybox face, as FitzQuake (after Quake 2) lays them out: rt +x, lf -x,
// bk +y, ft -y, up +z, dn -z, each face's s running to the viewer's right and
// t up the image.
vec3 skyboxColor(vec3 d) {
    vec3 a = abs(d);
    float face, major;
    vec2 st;
    if (a.x >= a.y && a.x >= a.z) { major = a.x; face = d.x >= 0.0 ? 0.0 : 1.0; st = vec2(d.x >= 0.0 ? -d.y : d.y, d.z); }
    else if (a.y >= a.z) { major = a.y; face = d.y >= 0.0 ? 2.0 : 3.0; st = vec2(d.y >= 0.0 ? d.x : -d.x, d.z); }
    else { major = a.z; face = d.z >= 0.0 ? 4.0 : 5.0; st = vec2(-d.y, d.z >= 0.0 ? -d.x : d.x); }
    float inset = 0.5 / float(textureSize(pack_color, 0).y);
    vec2 uv = clamp(vec2(st.x, -st.y) / max(major, 1e-6) * 0.5 + 0.5, inset, 1.0 - inset);
    return textureLod(pack_color, vec2((face + uv.x) / 6.0, uv.y), 0.0).rgb;
}
float spriteEmission() { return lights.parameters.y != 0u && lights.effects.x > 0.0 ? lights.effects.x : emission(); }
vec4 litEntry(float entry) {
    vec4 c = texture(colormap, vec2((entry + 0.5) / 256.0, 32.5 / 64.0));
    c.rgb = paletteDecode(c.rgb);
    if (entry < 224.0) c.rgb *= lit_radiance;
    else c.rgb *= emission();
    return c;
}
vec4 paletteColor(ivec2 p, int level, float grade) {
    ivec2 size = textureSize(indices, level);
    p = (p % size + size) % size;
    float entry = floor(texelFetch(indices, p, level).r * 255.0 + 0.5);
    if (linear_lit) return litEntry(entry);
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
// Luminous sky, whatever the texture filter: each 128x128 layer is filtered
// bilinearly on its own, wrapping inside its half of the texture, and the
// front layer's index-0 mask becomes fractional coverage so cloud edges
// feather instead of cutting out. Colours are the palette's own (colormap row
// 32), blended in linear light. Classic keeps the original nearest-texel sky.
vec4 skyEntry(float entry) {
    vec4 c = texture(colormap, vec2((entry + 0.5) / 256.0, 32.5 / 64.0));
    c.rgb = paletteDecode(c.rgb);
    return c;
}
vec4 skyLayer(vec2 layer_uv, int left, bool masked) {
    int half_width = textureSize(indices, 0).x / 2, height = textureSize(indices, 0).y;
    vec2 pixel = layer_uv * vec2(half_width, height) - 0.5;
    ivec2 base = ivec2(floor(pixel)); vec2 weight = fract(pixel);
    vec4 sum = vec4(0.0); // rgb premultiplied by coverage
    for (int i = 0; i < 4; ++i) {
        ivec2 p = base + ivec2(i & 1, i >> 1);
        p.x = (p.x % half_width + half_width) % half_width + left;
        p.y = (p.y % height + height) % height;
        float entry = floor(texelFetch(indices, p, 0).r * 255.0 + 0.5);
        float w = (i & 1) != 0 ? weight.x : 1.0 - weight.x;
        w *= (i >> 1) != 0 ? weight.y : 1.0 - weight.y;
        if (masked && entry < 0.5) continue;
        sum += vec4(skyEntry(entry).rgb, 1.0) * w;
    }
    return sum;
}
// Luminous sprites, like the sky: the frame filtered bilinearly in linear
// light, its transparent index (255) becoming fractional coverage so edges
// feather instead of stepping. rgb is premultiplied by coverage; fullbrights
// keep their emission.
vec4 spriteSample(vec2 coord) {
    ivec2 size = textureSize(indices, 0);
    vec2 pixel = coord * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(pixel)); vec2 weight = fract(pixel);
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 4; ++i) {
        ivec2 p = clamp(base + ivec2(i & 1, i >> 1), ivec2(0), size - 1);
        float entry = floor(texelFetch(indices, p, 0).r * 255.0 + 0.5);
        if (entry > 254.5) continue;
        float w = ((i & 1) != 0 ? weight.x : 1.0 - weight.x) * ((i >> 1) != 0 ? weight.y : 1.0 - weight.y);
        vec3 c = paletteDecode(texture(colormap, vec2((entry + 0.5) / 256.0, 32.5 / 64.0)).rgb);
        if (entry >= 224.0) c *= spriteEmission();
        sum += vec4(c, 1.0) * w;
    }
    return sum;
}
vec4 smoothSky(vec2 back_uv, vec2 front_uv) {
    int half_width = textureSize(indices, 0).x / 2;
    vec4 back = skyLayer(back_uv, half_width, false), front = skyLayer(front_uv, 0, true);
    return vec4(displayEncode(back.rgb * (1.0 - front.a) + front.rgb), 1.0);
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
// Cube face (+X,-X,+Y,-Y,+Z,-Z) and its texture position for a direction from
// the light, as gpu_sdl.c's ShadowFace projects them: u along the face's
// right, v down its up axis. xy: uv; z: cube*6 + face layer.
const vec3 FACE_RIGHT[6] = vec3[](vec3(0,0,-1), vec3(0,0,1), vec3(1,0,0), vec3(1,0,0), vec3(1,0,0), vec3(-1,0,0));
const vec3 FACE_UP[6] = vec3[](vec3(0,1,0), vec3(0,1,0), vec3(0,0,-1), vec3(0,0,1), vec3(0,1,0), vec3(0,1,0));
vec3 faceTap(vec3 d, float cube) {
    vec3 a = abs(d);
    int face = a.x >= a.y && a.x >= a.z ? (d.x >= 0.0 ? 0 : 1) : a.y >= a.z ? (d.y >= 0.0 ? 2 : 3) : (d.z >= 0.0 ? 4 : 5);
    float forward = max(max(a.x, a.y), a.z);
    vec2 ndc = vec2(dot(d, FACE_RIGHT[face]), dot(d, FACE_UP[face])) / forward;
    return vec3(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y, cube * 6.0 + float(face));
}
float compareTap(uint tier, vec3 direction, float cube, float reference) {
    vec3 p = faceTap(direction, cube);
    if (tier == 0u) return texture(shadow_maps0, vec4(p, reference));
    if (tier == 1u) return texture(shadow_maps1, vec4(p, reference));
    if (tier == 2u) return texture(shadow_maps2, vec4(p, reference));
    return texture(shadow_maps3, vec4(p, reference));
}
float depthTap(uint tier, vec3 direction, float cube) {
    vec3 p = faceTap(direction, cube);
    if (tier == 0u) return texture(shadow_depths0, p).r;
    if (tier == 1u) return texture(shadow_depths1, p).r;
    if (tier == 2u) return texture(shadow_depths2, p).r;
    return texture(shadow_depths3, p).r;
}
// Reversed depth the receiving plane would have along a tap direction. Wide
// filters then compare like with like on tilted receivers instead of meeting
// the far side of the receiver's own wall. Negative: this direction cannot
// reach the receiving plane in front of the light and contributes no light.
float planeReference(vec3 tap, vec3 origin, vec3 normal, vec3 point) {
    float toward = dot(normal, tap);
    if (toward >= -1e-4) return -1.0;
    return SHADOW_NEAR / majorAxis(tap * (dot(normal, point - origin) / toward));
}
// Sample order: the first four lie on the disc's rim, one per quadrant; the
// first eight cover it evenly. Fewer samples take a prefix of this order.
const int TAP_ORDER[16] = int[](0,1,7,13, 2,11,12,15, 3,4,5,6,8,9,10,14);
// The cube's view of the receiver: tap directions around `ray`, and the
// receiving plane each tap is compared against.
struct ShadowFrame { uint tier; float cube; vec3 origin, normal, point, ray, side, up; mat2 rotation; };
vec3 shadowTap(ShadowFrame f, int i, float radius) {
    vec2 offset = f.rotation * DISK[TAP_ORDER[i]] * radius;
    return f.ray + f.side * offset.x + f.up * offset.y;
}
// Lit samples among taps [first, last) of a disc of the given radius.
float litTaps(ShadowFrame f, int first, int last, float radius) {
    float sum = 0.0;
    for (int i = first; i < last; ++i) {
        vec3 tap = shadowTap(f, i, radius);
        float reference = planeReference(tap, f.origin, f.normal, f.point);
        // A grazing tap that cannot reach the receiver is not an unoccluded
        // light sample: counting it as lit opens bright seams at sealed joins.
        sum += reference < 0.0 ? 0.0 : compareTap(f.tier, tap, f.cube, reference * 1.0005);
    }
    return sum;
}
// Blocker search over taps [first, last): sums SHADOW_NEAR / depth and count.
void blockerTaps(ShadowFrame f, int first, int last, float radius, inout float blockers, inout float count) {
    for (int i = first; i < last; ++i) {
        vec3 tap = shadowTap(f, i, radius);
        float reference = planeReference(tap, f.origin, f.normal, f.point);
        if (reference < 0.0) continue;
        float stored = depthTap(f.tier, tap, f.cube);
        if (stored > reference * 1.0005) { blockers += SHADOW_NEAR / stored; count += 1.0; }
    }
}
float penumbra(float radius, float major, float blocker, float texel, float search) {
    return clamp(radius * max(major - blocker, 0.0) / max(blocker, SHADOW_NEAR), texel, search);
}
// Reversed-depth cube shadows from an emitter of the given radius (PCSS): the
// mean depth of blockers found inside the largest possible penumbra sets the
// filter width radius * (receiver - blocker) / blocker, so contact shadows stay
// sharp and distant occluders soften. Comparisons are hardware filtered.
float shadowFactor(uint slot, vec3 origin, vec3 normal, float radius, float brightness) {
    if (lights.parameters.w == 0u) return 1.0;
    uint shadow = uint(lights.shape[slot].w + 0.5);
    ShadowFrame f;
    f.tier = shadow / 1024u; f.cube = float(shadow % 1024u); f.origin = origin; f.normal = normal;
    float texel = 2.0 * majorAxis(world_position - origin) / lights.shadow_sizes[f.tier];
    f.point = world_position + normal * texel;
    f.ray = f.point - origin;
    float distance = length(f.ray), major = majorAxis(f.ray);
    // Spark particles can coincide with their collection's source. The cube
    // cannot contain a blocker inside its near plane; avoid a zero ray basis.
    if (distance < SHADOW_NEAR) return 1.0;
    f.side = normalize(cross(f.ray, abs(f.ray.z) < 0.9 * distance ? vec3(0,0,1) : vec3(1,0,0)));
    f.up = normalize(cross(f.ray, f.side));
    // A world-anchored rotation turns sampling bands into stable fine grain.
    float angle = fract(sin(dot(floor(world_position), vec3(12.9898,78.233,37.719))) * 43758.5453) * 6.2831853;
    f.rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
    // Samples follow this light's brightness here, since an error in a dim
    // light's shadow is a small error in the image: all 16 from
    // shadow_filter.x (0: always), 8 from a quarter of it, 4 below.
    float full = lights.shadow_filter.x;
    int taps = full <= 0.0 || brightness >= full ? 16 : brightness >= full * 0.25 ? 8 : 4;
    float search = max(min(radius * 3.0, distance * 0.5), texel), width = texel;
    // With every rim sample occluded the point is taken to be inside a thick
    // shadow and its rim test decides. Lit or mixed rims always take the full
    // set, so thin shadows survive; a thin gap of light inside a thick shadow
    // is the case given up.
    if (radius > 0.0) {
        float blockers = 0.0, count = 0.0;
        blockerTaps(f, 0, 4, search, blockers, count);
        if (taps > 4 && count == 4.0 &&
            litTaps(f, 0, 4, penumbra(radius, major, blockers / count, texel, search)) == 0.0) return 0.0;
        blockerTaps(f, 4, taps, search, blockers, count);
        if (count == 0.0) return 1.0;
        width = penumbra(radius, major, blockers / count, texel, search);
        return litTaps(f, 0, taps, width) / float(taps);
    }
    float rim = litTaps(f, 0, 4, width);
    if (taps > 4 && rim == 0.0) return 0.0;
    return (rim + litTaps(f, 4, taps, width)) / float(taps);
}
// A slot's shadow, from its cube's centre (limit.yzw) and emitter radius
// (shape.y). Panel lights share one cube per group (LQ_ShareGroupShadows in
// scene.c), so each pixel filters a cube once and its other members reuse the
// result. Diffuse effects aim each light's test along their own direction and
// are not cached.
vec4 cached_cube[2] = vec4[](vec4(-1.0), vec4(-1.0));
vec4 cached_shadow[2] = vec4[](vec4(1.0), vec4(1.0));
int cached_next = 0;
float lightShadow(uint i, vec3 normal, float brightness, bool diffuse_effect) {
    if ((surface.shadow_free[i / 32u] & (1u << (i % 32u))) != 0u) return 1.0;
    float cube = lights.shape[i].w;
    if (!diffuse_effect)
        for (int k = 0; k < 8; ++k) if (cached_cube[k >> 2][k & 3] == cube) return cached_shadow[k >> 2][k & 3];
    float shadow = shadowFactor(i, lights.limit[i].yzw, normal, lights.shape[i].y, brightness);
    if (!diffuse_effect) {
        cached_cube[cached_next >> 2][cached_next & 3] = cube;
        cached_shadow[cached_next >> 2][cached_next & 3] = shadow;
        cached_next = (cached_next + 1) & 7;
    }
    return shadow;
}
// Unshadowed light from slot i at this point, in Quake brightness units (its
// strength included, colour not), and the direction toward it.
// The pack's mip level here, as the sampler picks it, from the coordinate's
// screen derivatives (no textureQueryLod in MSL 2.1). Pack detail is culled by
// it: where a texel covers less than a pixel, bumps are below what can be seen.
float packLod(vec2 dx, vec2 dy) {
    vec2 size = vec2(textureSize(pack_normal, 0));
    return 0.5 * log2(max(max(dot(dx * size, dx * size), dot(dy * size, dy * size)), 1e-8));
}
// The world-space gradients of the texture coordinates across the face, from
// screen derivatives of position and coordinate (a cotangent frame), so no
// vertex data is needed. Dividing by the determinant keeps the sign right
// whichever way the API's screen y runs.
void textureFrame(vec3 n, vec2 coord, out vec3 du, out vec3 dv) {
    vec3 dp1 = dFdx(world_position), dp2 = dFdy(world_position);
    vec2 duv1 = dFdx(coord), duv2 = dFdy(coord);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    float det = dot(dp1, dp2perp);
    float inverse = abs(det) > 1e-20 ? 1.0 / det : 0.0;
    du = (dp2perp * duv1.x + dp1perp * duv2.x) * inverse;
    dv = (dp2perp * duv1.y + dp1perp * duv2.y) * inverse;
}
// The pack's normal map in world space, from a texel of it (`texel`, as
// sampled). DarkPlaces' _norm red points along the image's rows and green up
// it; texture rows run down the image, so green turns toward decreasing t.
// The loader keeps x and y in red and green and puts gloss in blue
// (LQ_PackGlossIntoNormal), so z is rebuilt: the normal points out of the face.
vec3 bumpNormal(vec3 n, vec3 du, vec3 dv, vec4 texel) {
    float scale = inversesqrt(max(max(dot(du, du), dot(dv, dv)), 1e-20));
    vec2 m = texel.rg * 2.0 - 1.0;
    float mz = sqrt(max(1.0 - dot(m, m), 0.0));
    vec3 bumped = du * (scale * m.x) - dv * (scale * m.y) + n * mz;
    return dot(bumped, bumped) > 1e-12 ? normalize(bumped) : n;
}
// Specular from a pack gloss map, as DarkPlaces' Blinn-Phong with its default
// exponent (32): the half vector between light and eye against the bumped
// normal. Weighted by the light's own arrival (incidence, falloff, shadow)
// and the gloss strength; it adds light that is not the surface's colour.
float glossTerm(vec3 shading, vec3 to_light, vec3 to_eye) {
    vec3 h = to_light + to_eye;
    float l = dot(h, h);
    if (l < 1e-8) return 0.0;
    return pow(max(dot(shading, h * inversesqrt(l)), 0.0), 32.0);
}
// Parallax, as DarkPlaces' relief mapping: the normal map's alpha is height,
// spanning surface.relief.x of the texture's size. The texture's mean height
// (relief.y) lies at the face, as DarkPlaces' "match" bias puts a chosen
// height there, so higher parts stand out of it and lower ones sink. The view
// ray starts where it is at the top of the range, above the face, and marches
// down in even steps (more where the view grazes); the hit is interpolated
// between the last step above the surface and the first below it. Returns
// the texture-coordinate shift to where the ray meets the surface.
vec2 parallaxShift(vec3 n, vec3 du, vec3 dv, vec2 coord, vec2 dx, vec2 dy, float weight) {
    vec3 to_eye = normalize(surface.camera_origin.xyz - world_position);
    float facing = dot(to_eye, n);
    float lu = dot(du, du), lv = dot(dv, dv);
    if (facing <= 0.0 || lu <= 0.0 || lv <= 0.0) return vec2(0.0);
    // Depth in world units, from the texture's size on the face.
    float depth = surface.relief.x * weight * inversesqrt(sqrt(lu * lv));
    // Grazing views are held to a 4:1 slope so the shift stays bounded.
    vec2 full = -vec2(dot(to_eye, du), dot(to_eye, dv)) * (depth / max(facing, 0.25));
    int steps = int(mix(24.0, 8.0, facing));
    float layer = 1.0 / float(steps);
    vec2 shift = -full * (1.0 - surface.relief.y), last_shift = shift;
    float ray = 1.0, height = textureGrad(pack_normal, coord + shift, dx, dy).a, last_gap = 0.0;
    for (int i = 0; i < 24 && i < steps && height < ray; ++i) {
        last_gap = ray - height; last_shift = shift;
        ray -= layer; shift += full * layer;
        height = textureGrad(pack_normal, coord + shift, dx, dy).a;
    }
    float under = height - ray;
    if (last_gap > 0.0 && under > 0.0) shift = mix(shift, last_shift, under / (under + last_gap));
    return shift;
}
// Relief shadows (Luminous's own; DarkPlaces never marches toward a light):
// from the point the view ray found, march up through the height field
// toward the pixel's strongest light. The ray rises from the point's own
// height to the top of the range at parallax's depth and slope, so a pack
// shadows itself with the depth its author tuned. A texel above the ray
// blocks; the result is soft from the closest miss, the least clearance over
// the distance marched, so ridges grade into their shadows. More steps where
// the light grazes; faded out with parallax by footprint (packDetail).
// Shadow cubes still cast every shadow between surfaces.
float reliefShadow(vec3 n, vec3 du, vec3 dv, vec2 coord, vec2 dx, vec2 dy, vec3 to_light, float fade) {
    float facing = dot(to_light, n);
    float lu = dot(du, du), lv = dot(dv, dv);
    if (facing <= 0.0 || lu <= 0.0 || lv <= 0.0 || fade <= 0.0) return 1.0;
    float depth = surface.relief.x * inversesqrt(sqrt(lu * lv));
    // Texture shift per unit of height risen toward the light, held to a 4:1
    // slope as parallax is.
    vec2 rise = vec2(dot(to_light, du), dot(to_light, dv)) * (depth / max(facing, 0.25));
    float start = textureGrad(pack_normal, coord, dx, dy).a;
    if (start >= 0.999) return 1.0;
    int steps = int(mix(16.0, 6.0, facing));
    float layer = (1.0 - start) / float(steps), clearance = 1.0;
    for (int i = 1; i <= 16 && i <= steps; ++i) {
        float climbed = layer * float(i);
        float height = textureGrad(pack_normal, coord + rise * climbed, dx, dy).a;
        clearance = min(clearance, (start + climbed - height) / climbed);
        if (clearance <= 0.0) break;
    }
    // Clear by a third of the climb or more is fully lit.
    return mix(1.0, smoothstep(0.0, 0.33, clearance), fade);
}
// `shading` is the bumped normal where a normal map applies: it sets the
// incidence, while the face's own normal still decides which side lights it.
float lightReach(uint i, vec3 normal, vec3 shading, bool diffuse_effect, bool liquid, float opacity, bool submerged, out vec3 direction) {
    vec4 origin = lights.position_range[i];
    direction = origin.xyz-world_position;
    float distance = length(direction);
    if (distance >= origin.w) return 0.0;
    direction = distance > 0.001 ? direction/distance : normal;
    float facing = diffuse_effect ? 1.0 : dot(normal,direction);
    if (facing <= 0.0) return 0.0; // LIGHT rejects sources behind a face
    if (!diffuse_effect) facing = dot(shading, direction);
    vec4 f = lights.falloff[i];
    /* LIGHT's softened incidence: half the value at grazing. */
    float direct = lightValue(f,distance) * max((1.0 - f.z) + f.z * facing, 0.0);
    // Unbounded falloffs stop at the range (their fade level); taper them to
    // zero over its last fifth (LQ_LIGHT_TAPER) so no edge shows.
    int formula = int(f.w + 0.5);
    if (formula == 1 || formula == 2 || formula == 5) {
        float taper = clamp((origin.w - distance) / (0.2 * origin.w), 0.0, 1.0);
        direct *= taper * taper;
    }
    vec4 cone = lights.spot[i];
    float spot = 1.0;
    if (cone.w >= -1.0) { // see LQ_SpotFactor: world-space blur across the cone edge
        float along = dot(-direction,cone.xyz), inner = lights.shape[i].x;
        if (cone.w > 0.01) {
            float edge = distance * sqrt(max(1.0 - along*along, 0.0)) -
                distance * along * sqrt(max(1.0 - cone.w*cone.w, 0.0)) / cone.w;
            float blur = 16.0 + lights.shape[i].y;
            spot = along > 0.0 ? 1.0 - smoothstep(-blur, blur, edge) : 0.0;
        } else if (along <= cone.w) return 0.0;
        if (inner > cone.w) spot *= smoothstep(cone.w,inner,along);
        if (spot <= 0.0) return 0.0;
    }
    direct *= lights.color_strength[i].w;
    // The cap bounds the light before its cone shapes it, so a torch accent's
    // penumbra still tapers from the cap to zero instead of being clipped flat.
    if (lights.limit[i].x > 0.0) direct = min(direct, lights.limit[i].x);
    direct *= spot;
    if (!liquid && opacity > 0.0 && submerged != (lights.shape[i].z > 0.5)) direct *= 1.0 - opacity;
    return direct;
}
// Ambient-cube irradiance toward n from one block group of a probe texture
// (alpha: sky visibility, unscaled). Trilinear filtering stays inside each
// direction block: z is clamped to its probe centres before the block offset is added.
vec4 probeCube(sampler3D probes, float blocks, float base, vec3 point, vec3 n) {
    vec3 grid = clamp((point - lights.probe_origin.xyz) / lights.probe_origin.w, vec3(0.0), lights.probe_size.xyz - 1.0);
    vec2 xy = (grid.xy + 0.5) / lights.probe_size.xy;
    float layers = lights.probe_size.z * blocks;
    vec3 weight = n * n;
    vec3 face = vec3(n.x >= 0.0 ? 0.0 : 1.0, n.y >= 0.0 ? 2.0 : 3.0, n.z >= 0.0 ? 4.0 : 5.0);
    vec4 sum = vec4(0.0);
    for (int axis = 0; axis < 3; ++axis) {
        float depth = ((base + face[axis]) * lights.probe_size.z + grid.z + 0.5) / layers;
        sum += texture(probes, vec3(xy, depth)) * weight[axis];
    }
    return vec4(sum.rgb * 255.0, sum.a);
}
vec4 effectProbe(sampler3D probes, float blocks, float base, vec3 point, vec3 n, bool diffuse) {
    if (!diffuse) return probeCube(probes, blocks, base, point, n);
    vec3 diagonal = vec3(0.57735027);
    return 0.5 * (probeCube(probes, blocks, base, point, diagonal) +
                  probeCube(probes, blocks, base, point, -diagonal));
}
// Value noise over 3D space, 0-1, for eroding smoke.
float hash3(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}
float noise3(vec3 p) {
    vec3 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i), hash3(i + vec3(1,0,0)), u.x),
                   mix(hash3(i + vec3(0,1,0)), hash3(i + vec3(1,1,0)), u.x), u.y),
               mix(mix(hash3(i + vec3(0,0,1)), hash3(i + vec3(1,0,1)), u.x),
                   mix(hash3(i + vec3(0,1,1)), hash3(i + vec3(1,1,1)), u.x), u.y), u.z);
}
void main() {
    int flags = int(surface.parameters.y);
    // Smoke erodes: noise over the puff's surface (its normal) and where it is
    // (so the pattern churns as it rises) cuts away the fraction uv.y, and
    // finer noise frays the silhouette, where the view grazes the sphere, so
    // a puff ends in a ragged edge, not a bubble's rim. What remains is blended
    // (in linear light, in the float frame) by how much smoke the view passes
    // through: densest straight through the middle, thinning smoothly toward
    // the rim and toward the eroded holes, and thinner as the puff ages, so
    // opacity never steps from a flat disc to nothing.
    float smoke_alpha = 1.0;
    if ((flags & 1024) != 0) {
        vec3 to_eye = normalize(surface.camera_origin.xyz - world_position);
        float facing = abs(dot(normalize(world_normal), to_eye)), rim = 1.0 - facing;
        float n = 0.65 * noise3(world_normal * 2.5 + world_position * 0.15) +
                  0.35 * noise3(world_normal * 6.0 + world_position * 0.4 + 17.0);
        float fringe = noise3(world_normal * 14.0 + world_position * 0.9 + 41.0);
        if (n < uv.y || fringe < smoothstep(0.2, 0.85, rim)) discard;
        float body = smoothstep(uv.y, uv.y + 0.3, n);
        smoke_alpha = 0.7 * facing * mix(0.3, 1.0, body) * (1.0 - 0.5 * uv.y);
    }
    bool diffuse_effect = lights.parameters.y != 0u && (flags & 128) != 0;
    // Solid (3D) particles take the effect colour but face lights by their normals.
    bool lit_effect = diffuse_effect || (lights.parameters.y != 0u && (flags & 512) != 0);
    vec3 effect_light = vec3(0.0);
    vec2 sample_uv = uv;
    // The pack's frame and footprint, from the coordinate before any parallax
    // shift so steps in the shift do not tear the frame or the filtering.
    vec3 frame_du = vec3(0.0), frame_dv = vec3(0.0);
    vec2 frame_dx = dFdx(uv), frame_dy = dFdy(uv), parallax = vec2(0.0);
    bool relief_on = false;
    // Pack detail by footprint (relief.w): parallax and relief shadows, the
    // costly marches, fade out between mip levels 1.5 and 3 and stop past it;
    // the bumped normal fades to the face's between 4 and 6 and stops past 6.
    float relief_weight = 1.0, bump_weight = 1.0;
    if (surface.pack.y > 0.5 && (flags & 3) == 0) {
        textureFrame(normalize(world_normal), uv, frame_du, frame_dv);
        if (surface.relief.w > 0.5) {
            float lod = packLod(frame_dx, frame_dy);
            relief_weight = 1.0 - smoothstep(1.5, 3.0, lod);
            bump_weight = 1.0 - smoothstep(4.0, 6.0, lod);
        }
        if (surface.relief.x > 0.0 && relief_weight > 0.0) {
            parallax = parallaxShift(normalize(world_normal), frame_du, frame_dv, uv, frame_dx, frame_dy, relief_weight);
            sample_uv += parallax;
            relief_on = surface.relief.z > 0.5;
        }
    }
    // Unlit surfaces (sky, opaque liquids) take colormap row 32, the palette's
    // own colours, as software Quake draws them; row 0 is twice that, clipped.
    float index_value, grade = 32.0;
    float gloss = 0.0;                         // gloss strength here (Luminous, pack gloss maps)
    vec3 specular = vec3(0.0), shine = vec3(0.0); // gloss light in brightness units, then as light
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
        if (lights.sky_fog.y > 0.5) sky_color = vec4(paletteLike(skyboxColor(world_position - surface.camera_origin.xyz)), 1.0);
        else if (lights.parameters.y != 0u) sky_color = smoothSky(back_uv, front_uv);
    } else {
        bool liquid = (flags & 2) != 0;
        if (liquid)
            sample_uv += sin(uv.yx * 8.0 + surface.parameters.x) * 0.125;
        index_value = texture(indices, sample_uv).r;
        // A translucent liquid catches light in proportion to its opacity (the
        // blend weight) and passes the rest to surfaces across it. Opaque
        // liquids keep Quake's unlit look.
        float opacity = lights.liquid.x;
        bool submerged = (flags & 64) != 0;
        if (!liquid || (opacity > 0.0 && lights.parameters.y != 0u)) {
            float brightness = 0.0;
            bool actor = (flags & 32) != 0;
            if (lights.parameters.y != 0u && ((flags & 4) == 0 || actor)) {
                vec3 normal = normalize(world_normal);
                vec3 shading = normal;
                if (surface.pack.y > 0.5 && bump_weight > 0.0) {
                    vec4 texel = textureGrad(pack_normal, sample_uv, frame_dx, frame_dy);
                    shading = bumpNormal(normal, frame_du, frame_dv, texel);
                    if (bump_weight < 1.0) shading = normalize(mix(normal, shading, bump_weight));
                    gloss = texel.b * surface.pack.w * bump_weight;
                }
                vec3 to_eye = normalize(surface.camera_origin.xyz - world_position);
                vec3 illumination = vec3(0.0);
                // Shadows are filtered only where they can be seen. A light
                // below the faint level, or below the dominance ratio of the
                // brightest light here, is left unshadowed: its shadow would be
                // lost in the light around it. Each blends in between its
                // threshold and twice that, so nothing switches visibly.
                // Dominance is judged against the brightest light as it
                // arrives, after its own shadow, so a light blocked here cannot
                // excuse others that are blocked too.
                float strongest = 0.0, brightest = 0.0, brightest_shadow = 1.0;
                uint lead = 0xffffffffu;
                vec3 toward, lead_toward = normal;
                // A face whose brightest light is the same everywhere names it
                // (see KnownLead in gpu_sdl.c), and the search is skipped.
                uint known = uint(surface.face_light.x + 0.5);
                if (lights.color_space.z > 0.0 && known != 0u && !actor) {
                    lead = known - 1u;
                    vec3 c = lights.color_strength[lead].rgb;
                    brightest = lightReach(lead, normal, shading, diffuse_effect, liquid, opacity, submerged, toward) * max(c.r, max(c.g, c.b));
                    brightest_shadow = lightShadow(lead,diffuse_effect ? toward : normal,brightest,diffuse_effect);
                    lead_toward = toward;
                    strongest = brightest * brightest_shadow;
                } else if (lights.color_space.z > 0.0) {
                    for (uint word = 0u; word < 4u; ++word)
                    for (uint bits = surface.light_bits[word]; bits != 0u; bits &= bits - 1u) {
                        uint i = word * 32u + uint(findLSB(bits));
                        if (i >= lights.parameters.x) break;
                        vec3 c = lights.color_strength[i].rgb;
                        float value = lightReach(i, normal, shading, diffuse_effect, liquid, opacity, submerged, toward) * max(c.r, max(c.g, c.b));
                        if (value > brightest) { brightest = value; lead = i; }
                    }
                    if (lead != 0xffffffffu) {
                        lightReach(lead, normal, shading, diffuse_effect, liquid, opacity, submerged, toward);
                        brightest_shadow = lightShadow(lead,diffuse_effect ? toward : normal,brightest,diffuse_effect);
                        lead_toward = toward;
                        strongest = brightest * brightest_shadow;
                    }
                }
                // Relief shadows: the strongest light only (see reliefShadow).
                if (relief_on && lead != 0xffffffffu && brightest_shadow > 0.0 && !diffuse_effect) {
                    brightest_shadow *= reliefShadow(normal, frame_du, frame_dv, sample_uv, frame_dx, frame_dy, lead_toward, relief_weight);
                    strongest = brightest * brightest_shadow;
                }
                for (uint word = 0u; word < 4u; ++word)
                for (uint bits = surface.light_bits[word]; bits != 0u; bits &= bits - 1u) {
                    uint i = word * 32u + uint(findLSB(bits));
                    if (i >= lights.parameters.x) break;
                    float direct = lightReach(i, normal, shading, diffuse_effect, liquid, opacity, submerged, toward);
                    if (direct < 0.01) continue;
                    vec3 c = lights.color_strength[i].rgb;
                    if (i == lead) {
                        illumination += c * direct * brightest_shadow;
                        if (gloss > 0.0) specular += c * (direct * brightest_shadow * glossTerm(shading, toward, to_eye));
                        continue;
                    }
                    float value = direct * max(c.r, max(c.g, c.b));
                    float needed = max(lights.color_space.y, strongest * lights.color_space.z);
                    float weight = needed > 0.0 ? clamp(value / needed - 1.0, 0.0, 1.0) : 1.0;
                    if (weight > 0.0)
                        direct *= mix(1.0, lightShadow(i,diffuse_effect ? toward : normal,value,diffuse_effect), weight);
                    illumination += c * direct;
                    if (gloss > 0.0) specular += c * (direct * glossTerm(shading, toward, to_eye));
                }
                // Static-world bounce from those lights, driven by live styles.
                // Bounce under a translucent liquid mostly arrived through its surface.
                float row = light_uv.y * lights.tuning.y, through = submerged && opacity > 0.0 ? 1.0 - opacity : 1.0;
                if (lights.tuning.z != 0.0 && (flags & 4) == 0 && !liquid) {
                    for (int k = 0; k < 4; ++k) if (surface.bounce_weights[k] > 0.0)
                        illumination += texture(bounce, vec2(light_uv.x, (row + float(k)) * 0.25)).rgb *
                            (255.0 * surface.bounce_weights[k] * lights.tuning.w * through);
                }
                // Sky as a diffuse area light: baked cosine-weighted visibility
                // times the sky's brightness. Actors and liquids use the probes.
                bool sky = dot(lights.sky.rgb, vec3(1.0)) > 0.0;
                if (sky && (flags & 4) == 0 && !liquid)
                    illumination += texture(bounce, vec2(light_uv.x, row * 0.25)).a * lights.sky.rgb * through;
                else if (sky && (actor || liquid) && lights.probe_origin.w > 0.0)
                    illumination += effectProbe(probe_static, 24.0, 0.0, world_position + normal * (actor ? 0.0 : 8.0), normal, diffuse_effect).a *
                        lights.sky.rgb;
                // ericw-tools sun: one directional light with baked visibility,
                // softened incidence as for point lights.
                if (dot(lights.sun_light.rgb, vec3(1.0)) > 0.0 && ((flags & 4) == 0 || actor || liquid)) {
                    float facing = diffuse_effect ? 1.0 : -dot(normal, lights.sun_direction.xyz);
                    float visible = 0.0;
                    if (facing > 0.0) {
                        if ((flags & 4) == 0 && !liquid) visible = texture(bounce, vec2(light_uv.x, (row + 1.0) * 0.25)).a * through;
                        else if (lights.probe_origin.w > 0.0)
                            visible = probeCube(probe_static, 24.0, 6.0, world_position + normal * (actor ? 0.0 : 8.0), normal).a;
                    }
                    float as = lights.sun_direction.w;
                    if (!diffuse_effect) facing = -dot(shading, lights.sun_direction.xyz);
                    illumination += lights.sun_light.rgb * max((1.0 - as) + as * facing, 0.0) * visible;
                }
                // Probes: static bounce for actors; gameplay-light bounce for all.
                int probes = int(lights.probe_size.w + 0.5);
                if (probes != 0) {
                    vec3 point = world_position + normal * (actor ? 0.0 : 8.0);
                    // Liquids have no lightmap or bounce texels; the static probes stand in.
                    if ((actor || liquid) && (probes & 1) != 0)
                        for (int k = 0; k < 4; ++k) if (lights.probe_weights[k] > 0.0)
                            illumination += effectProbe(probe_static, 24.0, float(k * 6), point, normal, diffuse_effect).rgb *
                                (lights.probe_weights[k] * lights.tuning.w);
                    if ((probes & 2) != 0)
                        illumination += effectProbe(probe_dynamic, 6.0, 0.0, point, normal, diffuse_effect).rgb * lights.tuning.w;
                }
                illumination *= lights.tuning.x;
                effect_light = illumination / 128.0;
                if ((flags & 16) != 0) illumination = max(illumination, vec3(24.0)); // Quake's view-weapon minimum
                brightness = max(illumination.r,max(illumination.g,illumination.b));
                if (brightness > 0.001) direct_tint = illumination/brightness;
                // Brightness sums as LIGHT summed it; it becomes linear light once.
                // The frame is now floating point and tone-mapped afterwards, so
                // the ceiling is eight times the palette colour (brightness 1024),
                // with the light's hue kept; tone mapping rolls highlights off.
                vec3 capped = brightness > 1024.0 ? illumination * (1024.0 / brightness) : illumination;
                lit_radiance = pow(max(capped / 128.0, vec3(0.0)), vec3(lights.color_space.x));
                // Gloss shine, in the same brightness units, becomes light the same way.
                if (gloss > 0.0) shine = gloss * pow(max(specular * lights.tuning.x / 128.0, vec3(0.0)), vec3(lights.color_space.x));
                linear_lit = !lit_effect;
            } else {
                brightness = dot(texture(lightmap, light_uv), surface.light_weights) * 255.0;
                for (int i = 0; i < 32; ++i)
                    if ((surface.light_bits.x & (1u << i)) != 0u)
                        brightness += max(surface.dynamic_lights[i].w - length(world_position - surface.dynamic_lights[i].xyz), 0.0);
            }
            grade = clamp(floor((255.0 - brightness) * 0.25), 0.0, 63.0);
        }
    }
    vec2 dx = dFdx(sample_uv - parallax), dy = dFdy(sample_uv - parallax);
    float index = floor(index_value * 255.0 + 0.5);
    bool smooth_sprite = (flags & 2048) != 0 && lights.parameters.y != 0u;
    if ((flags & 8) != 0 && index > 254.5 && !smooth_sprite) discard;
    if ((flags & 4) != 0 && !(lights.parameters.y != 0u && (flags & 32) != 0)) grade = clamp(shade, 0.0, 63.0);
    color = texture(colormap, vec2((index + 0.5) / 256.0, (grade + 0.5) / 64.0));
    // Resolve colors before interpolation. Sky, masks and UI keep level zero.
    if ((flags & 9) == 0)
        color = filteredColor(sample_uv, dx, dy, grade);
    if (lit_effect) {
        // Unlit blood, smoke and bubbles go dark even if their palette entries
        // belong to Quake's fullbright range. Emission follows effect provenance.
        // Row 32 is the palette at Quake's normal brightness (128), the level
        // effect_light is relative to; row 0 is double that and would show
        // lit effects twice as bright as the walls around them.
        color = texture(colormap, vec2((index + 0.5) / 256.0, 32.5 / 64.0));
        color.rgb = paletteDecode(color.rgb) * pow(max(effect_light, vec3(0.0)), vec3(lights.color_space.x));
        written_linear = true;
    } else if (linear_lit) {
        if ((flags & 9) != 0) color = litEntry(index);
        // The pack's colour in place of the palette's, lit the same way; its
        // glow (alpha) emits as fullbrights do.
        if (surface.pack.x > 0.5 && (flags & 9) == 0) {
            vec4 hd = textureGrad(pack_color, sample_uv, frame_dx, frame_dy);
            // z matches the original texture's brightness (lq_pack_brightness);
            // the glow keeps the pack's own level.
            color = vec4(packAlbedo(hd.rgb, surface.pack.z) * lit_radiance + packAlbedo(hd.rgb, 1.0) * (hd.a * emission()) + shine, 1.0);
        }
        written_linear = true;
    } else if (lights.parameters.y != 0u && surface.pack.x > 0.5 && (flags & 4) != 0) {
        // A self-lit pack model (flames, bolts, lasers): its colour shows as
        // the palette's does, its glow emits.
        vec4 hd = textureGrad(pack_color, uv, frame_dx, frame_dy);
        color = vec4(packAlbedo(hd.rgb, surface.pack.z) + packAlbedo(hd.rgb, 1.0) * (hd.a * emission()), 1.0);
        written_linear = true;
    } else if (lights.parameters.y != 0u && index < 224.0) color.rgb *= direct_tint;
    else if (lights.parameters.y == 0u && surface.pack.x > 0.5 && (flags & 9) == 0) {
        // Classic GPU: the pack's colour lit as the palette is, by the
        // colormap's scale at this light grade (row 32 is 1x, row 0 2x), and
        // its glow unlit, as GLQuake-era engines drew replacement textures.
        // Through paletteLike, so the screen tints and Quake's gamma reach it.
        vec4 hd = textureGrad(pack_color, sample_uv, frame_dx, frame_dy);
        vec3 base = paletteLike(hd.rgb * pow(surface.pack.z, 1.0 / DISPLAY_GAMMA));
        color = vec4(min(base * ((64.0 - grade) / 32.0) + paletteLike(hd.rgb) * hd.a, vec3(1.0)), 1.0);
    }
    if (sky_color.a > 0.0) { color = sky_color; written_linear = false; }
    if (smooth_sprite) {
        vec4 s = spriteSample(uv);
        if (s.a < 0.02) discard;
        vec3 c = s.rgb / s.a;
        if (lit_effect) c *= pow(max(effect_light, vec3(0.0)), vec3(lights.color_space.x));
        color = vec4(c, s.a);
        written_linear = true;
    }
    // Luminous writes linear light, unclamped, into the floating-point frame;
    // the tone-mapping pass brings it to the display. Classic writes its
    // palette colours as they are.
    if ((flags & 1024) != 0) color.a = smoke_alpha;
    // Entity alpha: blended over the frame by its opacity (in linear light in Luminous).
    if ((flags & 4096) != 0) color.a = (smooth_sprite ? color.a : 1.0) * surface.entity.x;
    if (lights.parameters.y != 0u && !written_linear) {
        // The smooth sky was re-encoded with the plain transfer.
        color.rgb = sky_color.a > 0.0 ? displayDecode(color.rgb) : paletteDecode(color.rgb);
        if (index >= 224.0 && sky_color.a <= 0.0 && (flags & 1) == 0) color.rgb *= emission();
    }
    // FitzQuake fog: GL_EXP2 over the distance from the eye, mixed in linear
    // light in Luminous. The sky takes a fixed share of it (r_skyfog).
    if (lights.fog.w > 0.0) {
        vec3 fog = paletteLike(lights.fog.rgb);
        if (lights.parameters.y != 0u) fog = displayDecode(fog);
        float d = lights.fog.w * length(world_position - surface.camera_origin.xyz);
        float amount = (flags & 1) != 0 ? lights.sky_fog.x : 1.0 - exp(-d * d);
        color.rgb = mix(color.rgb, fog, clamp(amount, 0.0, 1.0));
    }
}
