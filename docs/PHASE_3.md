# Phase 3, first slice: native GPU world rendering

This document records earlier milestones. The current GPU gameplay path and
validation are described in [Classic GPU checkpoint](CLASSIC_GPU_CHECKPOINT.md);
its normal 3D rendering now supersedes the mixed software/GPU bridge below.

October 13, 2026. `luminousquake-gpu` is an experimental SDL3 GPU world viewer.
E1M1's BSP triangles are transformed, depth-tested, textured and shaded on Metal
on the M1 Max. This is the first renderer-boundary milestone, not the completed
GPU gameplay client. `luminousquake` remains the software playable baseline.

## October 14: live gameplay bridge

`luminousquake` now applies `lq_renderer` at the first rendered frame after
startup configuration, and when `vid_restart` runs. Software (0) uses the SDL
framebuffer presenter. Classic GPU (1) claims the same SDL3 window for its GPU
device, copies the live engine BSP into owned scene data, and renders from
`r_refdef` with the live engine light-style values. Enhanced (2) reports that it
is pending and uses Software. The preference remains archived in all cases.

The Classic GPU path is an interim mixed renderer. Quake still computes its
software frame and depth. Visible brush/animated/dynamically lit surface spans
and actor/weapon/sprite/particle depth changes identify software layers, which
are uploaded as an RGBA overlay. HUD/crosshair changes are composited after 3D
rendering; menus, console, underwater warp and legacy special debug views retain
a complete software frame. Palette/gamma/content shifts update the GPU colormap.
GPU view rectangles follow the engine screen-size/FOV settings, and presentation
preserves aspect ratio. This is a functional integration step, not a completed
fully GPU renderer or a performance optimization. UI difference detection and
mixed software/GPU depth are temporary bridges to explicit frame submissions;
exact visual equivalence and all gameplay situations remain unvalidated.

Client-state clearing invalidates the copied level, including same-map save/load
and demo changes where legacy hunk addresses may be reused. The next live frame
replaces GPU level resources. Renderer restart releases the GPU/window claim
before restoring the software presenter; returning to GPU creates fresh scene
resources. Allocation/device/upload failures restore software rendering and
explain the fallback in the console. CPU arrays are freed after GPU upload.

The isolated content smoke test can request `--renderer classic`. It verifies
movement/attack, save/load, demo playback, a transition to Start and switching
Software then Classic GPU, rejecting unexpected fallback. `--capture PATH`
retains its final frame only at an explicitly chosen location. The asset-free
GPU test additionally checks overlay orientation, transparent pixels and alpha
blending. The software baseline remains available through `lq_renderer 0`.

Validation: the combined Debug and ASan/UBSan suites pass 17 tests. Native GPU
readback verifies overlay alpha/orientation and a nonzero-origin view rectangle.
Software and Classic GPU content smoke tests pass; the Classic test also passes
under sanitizers, activates its startup preference without `vid_restart`, and
switches backends in the running game. A final Start frame with world, weapon
and HUD was visually inspected. Native launch-focus/fullscreen checks pass for
the standard software client. Cross-platform CI/runtime validation remains
pending, and these checks do not establish complete game or GLQuake equivalence.

## API decision and existing hardware renderer

Quake already has a hardware-rendering lineage. Its GLQuake implementation is
included in this repository. The new path uses its geometry/UV/lightmap and
sky/water conventions as references while replacing the graphics submission API.
There is no need to rediscover Quake's formats or discard its rendering knowledge.

SDL3 GPU is the selected shared graphics API. The initial embedded shader formats
are SPIR-V for Vulkan and MSL for Metal. SDL3's D3D12 backend requires DXIL/DXBC;
that shader-generation step is future work. Windows can use the Vulkan path once
validated. No Mac-only engine interface or OpenGL context is introduced.

The playable window/input/audio layer now uses SDL3, matching the GPU viewer's
dependency. Headless targets need no SDL. The software renderer still produces
the original indexed framebuffer; SDL3 presents it with nearest texture sampling
and letterboxing. Quake's mixer feeds a locked SDL3 audio stream in signed 16-bit
stereo at the chosen mixer rate; SDL converts it to the device format. Focus loss
pauses playback and clears queued stream data. Window/input/audio code is ready
for the gameplay renderer integration, but the standalone viewer still has its
own entry point and camera loop.

The playable Options menu has a persistent three-choice Renderer selector:
Software (0), Classic GPU (1), and Enhanced (2), stored in `lq_renderer`.
Classic GPU preserves GLQuake-style baked lighting through the Metal/Vulkan
backend; Enhanced is the subsequent Tenebrae-inspired lighting/material path.
Left/right cycle both ways and Enter advances. The menu identifies Classic GPU
as retaining CPU actors/UI, marks Enhanced as pending, and explains restart or
`vid_restart` application of the preference.
Software and the interim Classic GPU bridge are operational in the playable
client. Enhanced rendering will follow explicit GPU actor/brush/effect/UI
submissions and completion of the classic appearance baseline.

## Renderer boundary and ownership

`renderer/scene.h` contains only standard C types: vertices, triangle indices,
material images, draw ranges, palette-colormap image, a four-channel lightmap
atlas and camera/light-style inputs. There are no QuakeC entities, `model_t`
pointers, hunk/cache handles, SDL types or native graphics objects in that scene.

`quake_scene.c` uses the original Quake filesystem/loaders, then copies assets
into scene-owned allocations. Geometry, edge/index references, lighting ranges,
texture dimensions and finite coordinates are checked at the conversion boundary.
This does not constitute a general audit of the historical parsers before it.
The loader has one scene per process; file handles end with the process.

The backend uploads immutable geometry/images and copies draw metadata. The
viewer immediately frees the CPU scene after upload; continued rendering and
readback verify this ownership separation. GPU resources are released explicitly,
pending work completes before device teardown, and render-target recreation
replaces color/depth allocations together. Map-reload integration is still pending.

## Current rendering

- BSP world polygons become indexed triangle lists in GPU buffers.
- Original texture indices are sampled with nearest filtering; the palette and
  64 colormap grades preserve original shading/fullbright colours.
- A 2048-square atlas stores four independent lightmap channels, with duplicated
  border texels and bilinear light sampling. Per-draw style weights reproduce the
  classic preset flicker/pulse styles. Switchable lights use their initial state;
  there is no running QuakeC state to update them in this viewer.
- Shader-based two-layer sky and water UV warp follow GLQuake conventions.
  Their animation/appearance is an initial implementation, not a full visual
  equivalence claim across all maps/effects.
- A perspective Quake-coordinate camera uses a 90-degree horizontal field of view,
  depth [0,1], and native window pixel dimensions. Resize and fullscreen are exposed.
- A free-flight camera provides mouse/trackpad pitch/yaw and WASD/E/Q movement.
  `--fixed-camera` suppresses camera input for repeatable captures.
- Readback uses a download transfer buffer and fence before accessing GPU results.
  Captures are original GPU color targets, not screenshots of a software framebuffer.

This viewer does not draw animated actors, weapons, sprites, particles, moving
brush entities or the HUD/menu. It has no collision, combat, audio or QuakeC
execution. Animated texture sequences, visibility/frustum culling, draw batching,
dynamic lights, underwater view warp and modern material replacements remain
work for integration/subsequent phases. It currently submits every world surface;
no performance advantage over the software engine is claimed.

## Build and run

On the development Mac, native Homebrew SDL3 3.4.16 is installed. Initial
validation also used a locally built official SDL3 3.4.18 SDK. SDL3 3.2+ is the
code's minimum API requirement.

```sh
brew install sdl3
cmake -S . -B build-gpu -DCMAKE_BUILD_TYPE=Debug -DLQ_BUILD_GPU=ON -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/sdl3
cmake --build build-gpu --parallel 10
./build-gpu/luminousquake-gpu -basedir "$PWD/quake-complete/Quake" -map e1m1
```

Controls: WASD fly, E/Q or Space/Control vertical, Shift faster, F1 pointer capture,
F11 fullscreen, Escape exit. Pointer motion changes both view axes. This is a map
viewer, so flying through walls is expected.

For content conversion without a window, add `--scene-info`. For an invariant
camera/capture, add `--fixed-camera --run-seconds 4 --screenshot /tmp/quake-gpu.bmp`.
The viewer reads game assets and writes only an explicitly requested screenshot;
it does not write configuration, saves or demos into the source installation.

Build without the playable client using `-DLQ_BUILD_CLIENT=OFF`; build without
all windowing libraries using `-DLQ_BUILD_CLIENT=OFF -DLQ_BUILD_GPU=OFF`.
Windows/Linux require an SDL3 development package or source build; pass its
installation prefix to CMake. The CI Vulkan job pins official SDL3 3.4.18.

The committed generated shader header allows ordinary builds without shader
compilers. To regenerate from the GLSL sources:

```sh
brew install glslang spirv-cross
python3 tools/build_shaders.py
```

The script uses Vulkan 1.0 SPIR-V, validates it when `spirv-val` is available, and
cross-compiles to Metal 2.1 source. Resource sets follow SDL3's stage-specific
binding conventions. No shader compiler or Qt dependency exists at runtime.

## Validation and limits

October 14 platform-migration checkpoint: the SDL3 software client passes all
13 existing tests in Debug and ASan/UBSan builds. The combined software/GPU
configuration passes 17 tests, including native Metal readback. A native platform
run verifies startup keyboard focus, relative mouse math, text/modifier handling,
fullscreen enter/exit, audio restart and frame capture. The isolated E1M1
movement/attack, save/load and demo record/playback smoke passes in both Debug
and the sanitizer build. The Windows/Linux CI definitions now build the pinned
SDL3 SDK for the playable client as well as the viewer; those jobs remain unrun.

- Asset-free native GPU test draws a near red triangle before a far blue wall,
  checks palette colours, image orientation and depth occlusion in actual
  readback, resizes and replaces render targets/resources, changes a light-style
  weight, and repeats the check after CPU scene destruction.
- A GPU-independent scene-contract test checks invalid indices/ranges/nonfinite
  coordinates, ownership cleanup, and the Quake-to-GPU projection.
- A generated PAK/BSP fixture exercises actual legacy loading and scene conversion,
  missing-data handling and rejected map/duration arguments without game assets or
  a window system.
- Debug, Release and ASan/UBSan configurations pass on native arm64 (17 tests
  with the GPU test enabled). A headless-only configuration passes 8 tests, and
  a GPU-only configuration without SDL2 passes 12. Native Metal fixture and E1M1
  rendering/readback run under sanitizers without reports. E1M1 converts to
  16,794 triangles, 6,148 draws and 65 indexed materials in the supplied sample.
- Fixed-camera E1M1 and Start captures were visually inspected. They show
  recognizable source geometry, texture alignment, lightmap shading, fullbright
  panels and the initial sky treatment. This is not a numerical pixel-equivalence
  or full-gameplay comparison.
- The archive remains unverified test content and is ignored by Git. No retail
  data or retail-derived screenshots/maps are tracked.
- A Linux CI job is configured to test the Vulkan backend under Xvfb with Mesa's
  software Vulkan implementation. That job has not run for this project; physical
  Windows/Linux GPU behavior and D3D12 remain unvalidated.

`--gpu-test` requires a real SDL3 GPU backend/window system, even though it needs
no game data. Configure `-DLQ_GPU_TESTS=ON` to include it in CTest; otherwise the
scene/import checks remain suitable for environments with no GPU. Lack of a GPU
is an explicit failure for the requested GPU test, not a claimed pass.

## Next integration work

1. Replace the mixed rendering bridge with explicit GPU frame submissions,
   retaining the integrated renderer choice, map transitions and video restart.
2. Submit animated MDL entities, brush entities, sprites, particles and UI through
   explicit renderer-facing data, retaining the software comparison path.
3. Add visibility, texture animation and required effects to complete the original
   appearance/playability baseline; compare named camera captures and gameplay.
4. Establish replacement-asset lookup and resource lifetimes, then continue with
   buffered rendering/material enhancement and runtime lighting in the main plan.
