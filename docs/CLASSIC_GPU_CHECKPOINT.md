# Classic GPU feature implementation checkpoint — October 14, 2026

Milestone: the planned Classic GPU baseline features are implemented, including
mipmaps and anisotropic filtering. This records implementation completion for
that baseline, not complete campaign/platform validation or all rerelease visual
features. Rerelease fog, the Phase 4A engine worker pool and Luminous rendering
remain pending.

Normal Classic GPU frames rasterize all 3D geometry on the GPU. QuakeC, physics,
entity animation, particle simulation and asset decoding remain CPU work. The
software renderer remains selectable; Luminous mode is still reserved.

## Implemented

- BSP visibility and static-entity collection from the original PVS; world
  geometry/lightmaps persist on the GPU for the map lifetime.
- Animated MDL poses and grouped skins, UV seam handling, player-color skin
  translation, per-vertex lighting and the weapon depth range.
- Transformed inline brush models and independent BSP item models, sprite
  orientation/group animation/transparency, and GPU particle geometry while
  preserving the existing particle simulation.
- Animated world/brush textures, original sky and turbulent water conventions,
  live lightstyles, Classic point-light contribution, content/palette changes and
  underwater projection distortion. This is original-style rendering, not the
  Luminous shadow/material implementation.
- Owned frame geometry, cached texture copies and cycling GPU buffers. Map
  changes and renderer restart invalidate CPU/GPU resource caches.
- Explicit UI alpha coverage, including black UI pixels, rather than detecting
  changes against a software 3D frame. Whole-pixel UI blocks upload at logical
  resolution. Menus darken the GPU scene; legacy special/debug views retain a
  complete software frame. The 2D drawing code still paints palette bitmaps.
- Video options for palette-aware smooth filtering and off/2×/4× MSAA, with GPU
  VSync obeying the preference and unsupported settings reporting fallback.
  Masked sprites/UI retain crisp sampling. Mipmaps (off/nearest level/trilinear)
  and shader anisotropy (off/2×/4×/8×/16×) are implemented as saved Video Options;
  see the texture-filtering notes below.
- Presets through 3840×2160 and 3840×2400. Portable software edge arithmetic
  uses 13.19 fixed point instead of 12.20; the original assembly targets retain
  their old constants. Console travel time uses elapsed time and screen height,
  preserving its speed at the 800×600 reference.

## Validation and limits

### Texture filtering follow-up

Indexed world and actor textures receive a full mip chain through 1×1. Reduction
averages RGB colors using the base palette and quantizes the result back to a
palette entry; it never averages palette numbers. Uniform blocks retain their
exact entry, and wholly fullbright blocks remain in the fullbright range. Mixed
fullbright/non-fullbright blocks quantize into the lit range, so tiny emissive
details can dim at distance. Palette quantization is an approximation to RGB
mipmaps, chosen to retain Quake's colormap and palette effects at every level.

The shader resolves colors before bilinear/trilinear blending. Trilinear mode
includes bilinear spatial filtering regardless of the crisp/smooth preference. Anisotropy uses
the texture-space pixel footprint and up to 16 taps along its major axis; it is
implemented in the shader rather than relying on a hardware sampler blending
index values. It needs mipmaps and increases shader work as the selected limit
rises. Both new preferences default to off. Sky, masked textures and the UI keep
level-zero sampling. No replacement assets or PAK writes are required.

Generated CPU tests cover color reduction, fullbrights, odd dimensions, 1×1
images and dimension limits. Native Metal readback checks minification, both
mip modes, all anisotropic settings and the masked-texture bypass. The Video
Options test stages/applies both preferences and checks their archive flags.
Transfer rows are tightly packed with aligned level starts: older SDL Metal
uploads ignore custom row pitch; see the [SDL implementation](https://github.com/libsdl-org/SDL/blob/release-3.4.2/src/gpu/metal/SDL_gpu_metal.m).

Normal and ASan/UBSan native suites pass all 20 tests with these additions.
An 800×600 native sanitizer gameplay smoke with smooth filtering, trilinear mips,
16× anisotropy and 4× MSAA passes movement/attack, save/load, demo recording and
playback, map changes, renderer switching and fresh-process settings persistence.
The normal-build capture from the same settings was visually inspected.
This does not establish 4K performance or other-platform runtime correctness.

### Earlier baseline checks

Native arm64 Debug and ASan/UBSan suites passed 20 tests, including Metal
readback. Generated GPU tests check dynamic frame ownership, world/actor
occlusion, weapon depth, masked sprite transparency, palette-color interpolation,
2×/4× MSAA where supported and dynamic light contribution. The app fixture now
passes by explicitly isolating its basedir from remembered installations.

The supplied, unverified local PAK passes movement/attack, save/load, demo
record/playback, map changes, Software/Classic GPU switching and saved-resolution
relaunch checks. Native sanitizer gameplay passes at 800×600 with smooth
filtering/4× MSAA. The optimized Release smoke passes at 3840×2160, 105° FOV,
smooth filtering/4× MSAA; its capture was visually inspected. Debug 4K runs did
not complete the same command sequence within 20 seconds. No frame-rate or
speedup claim follows from these tests.

After the final bridge cleanup, both 20-test suites passed again. The final
Release 4K gameplay rerun was not executed because automatic approval review
could not complete due to the account usage limit; the earlier 4K pass stands.

This is a working Classic GPU baseline, not complete campaign coverage or exact
pixel equivalence with GLQuake. Original authenticated disc data, physical
Windows/Linux GPUs, extreme mod cases and long play sessions remain unverified.
The general worker pool remains the planned Phase 4A work; this checkpoint does
not claim parallel rendering preparation. Game data and captures remain ignored.
