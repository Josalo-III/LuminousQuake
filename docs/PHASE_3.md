# Phase 3, first slice: native GPU world rendering

October 13, 2026. `luminousquake-gpu` is an experimental SDL3 GPU world viewer.
E1M1's BSP triangles are transformed, depth-tested, textured and shaded on Metal
on the M1 Max. This is the first renderer-boundary milestone, not the completed
GPU gameplay client. `luminousquake` remains the SDL2/software playable baseline.

## API decision and existing hardware renderer

Quake already has a hardware-rendering lineage. Its GLQuake implementation is
included in this repository. The new path uses its geometry/UV/lightmap and
sky/water conventions as references while replacing the graphics submission API.
There is no need to rediscover Quake's formats or discard its rendering knowledge.

SDL3 GPU is the selected shared graphics API. The initial embedded shader formats
are SPIR-V for Vulkan and MSL for Metal. SDL3's D3D12 backend requires DXIL/DXBC;
that shader-generation step is future work. Windows can use the Vulkan path once
validated. No Mac-only engine interface or OpenGL context is introduced.

SDL3 and SDL2 are separate executable dependencies at this stage: the GPU viewer
links SDL3, the playable client links SDL2, and headless targets link neither.
Migration of the shared playable window/input/audio layer to SDL3 remains part of
the integration work. Keeping that change separate allows renderer development
and comparison against the currently playable engine.

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
cmake -S . -B build-gpu -DCMAKE_BUILD_TYPE=Debug -DLQ_BUILD_GPU=ON -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/sdl3;/opt/homebrew/opt/sdl2"
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

Build without the SDL2 playable client using `-DLQ_BUILD_CLIENT=OFF`; build without
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

1. Move the playable platform layer to SDL3 and connect renderer choice/lifecycle
   to the existing engine, including clean map transitions and video restart.
2. Submit animated MDL entities, brush entities, sprites, particles and UI through
   explicit renderer-facing data, retaining the software comparison path.
3. Add visibility, texture animation and required effects to complete the original
   appearance/playability baseline; compare named camera captures and gameplay.
4. Establish replacement-asset lookup and resource lifetimes, then continue with
   buffered rendering/material enhancement and runtime lighting in the main plan.
