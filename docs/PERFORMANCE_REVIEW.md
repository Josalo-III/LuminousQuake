# Performance review

Reviewed October 19, 2026, against the current working tree, including
uncommitted changes. These are code-review findings, not measured bottlenecks;
their individual frame-time impact has not been established.

The largest likely costs are fragment lighting, unnecessary draw submission,
and per-frame GPU resource creation. Follow the existing
[profiling method](PROFILING.md) before choosing or evaluating optimizations.

## 1. Expensive shadow filtering per pixel, per light

**Measured October 19, 2026:** the largest cost found. Turning filtering off
while still drawing cubes saves 4.5 ms of a 10.75 ms frame in demo1 and 2.9 ms
of 8.9 ms in demo2 (optimized build, 1920×1080; see the
[agenda](AGENDA.md), item 6). The dominant light's filter result is reused in
the per-light loop, not evaluated twice.

**Done October 19, 2026:** sample count now follows each light's brightness at
the point (16, 8 or 4; `lq_shadow_detail`), with an early exit inside thick
shadows. demo1 93.0 → 123.4 fps, demo2 112.2 → 140.8 fps; details and image
comparison in the [agenda](AGENDA.md).

**Location:** `renderer/shaders/world.frag`, `shadowFactor` and the main lighting
loops.

Each shadow evaluation can perform 16 blocker-depth samples plus 16 comparison
samples. With dominance enabled, the shader also evaluates light reach twice
across the surface's light list. Overlapping panel emitters multiply this cost.

Profile this first at high resolutions. Investigate adaptive sampling and
reducing redundant overlapping lights, with image comparisons to assess any
change to the authored lighting or shadow quality.

## 2. Main GPU pass lacks camera-frustum and backface rejection

**Done October 19, 2026** for the main pass: back-facing and off-screen world
faces and off-screen frame geometry are skipped on the CPU (see the
[agenda](AGENDA.md), item 6). Actor triangles are still drawn two-sided.
Measured gain on an optimized build: 0.05 ms (demo1) and 0.13 ms (demo2) a
frame. The PVS already removes most faces, and the GPU rejects off-screen
triangles cheaply, so this pass was not the bottleneck.

**Locations:** `platform/render_bridge.c`, `LQ_RenderGPUView`;
`renderer/gpu_sdl.c`, `LQ_GPUCreate` and `LQ_GPURender`.

World surfaces are gathered from the PVS and submitted individually, including
surfaces outside the screen. The graphics pipeline also disables backface
culling. The frustum test used for light selection does not filter actual world
submission.

Cull world surfaces and entities before submission. Validate winding before
enabling backface culling, and preserve surfaces that require two-sided
rendering. Keep shadow-caster visibility separate from camera visibility:
offscreen geometry can still cast visible shadows.

## 3. Overlay allocates GPU resources every frame

**Location:** `renderer/gpu_sdl.c`, `LQ_GPUOverlay`, `UploadImage`, and `Transfer`.

`LQ_GPUOverlay` creates a replacement texture every frame. Its upload helper
also creates a transfer buffer and submits a separate command buffer, then
releases the temporary resources.

Retain the texture and staging buffer across frames, recreating only when
dimensions change. Preserve safe reuse while earlier frames are in flight.
Consider combining the upload with other frame uploads.

## 4. Every particle becomes a separate draw call

**Locations:** `platform/gpu_entities.c`, `LQ_EntitiesParticle` and `Draw`;
`renderer/gpu_sdl.c`, `DrawSurface` and `SurfaceUniforms`.

Each particle calls `Draw`; submission then binds 14 texture/sampler entries
and pushes uniforms for each draw. Luminous also scans runtime lights for each
particle's bounds. Explosions and smoke can therefore produce substantial CPU
submission overhead.

Batch compatible particles or instance shared geometry. Preserve blend
ordering, per-particle appearance, and lighting; batching must retain adequate
light coverage for every particle in the batch.

## 5. Shadow-caster rest detection is quadratic

**Location:** `renderer/gpu_sdl.c`, the rest-bounds search in `LQ_GPUFrame`.

Every current caster searches every previous caster's bounds. With similar
counts across frames, this grows as O(casters²). Moving brushes contribute
individual faces, so counts can grow quickly.

Carry stable entity/surface identifiers into the frame data to permit direct
matching while retaining the existing drift threshold. Check behavior when
entities disappear, reappear, or change models.

## 6. World visibility and lighting repeatedly scan and copy full draw arrays

**Locations:** `platform/render_bridge.c`, `LQ_RenderGPUView` and
`LuminousLights`; `renderer/scene.c`, `LQ_BuildRuntimeLightsInView` and
`LQ_BuildVisibleLightInteractions`.

Each frame copies every world draw, clears every index count, restores visible
surfaces, copies the array again for interactions, and checks every runtime-light
slot against every visible surface. Light selection additionally allocates
scratch arrays and sorts candidates.

Investigate compact visible-draw lists, persistent scratch storage, and sparse
source-to-slot mappings. Preserve stable light slots and shadow tiers: the
earlier offscreen-light experiment caused cube rebuilds when looking around
(see [the optimization history](AGENDA.md)).

## 7. Actor geometry is expanded, processed repeatedly, and uploaded every frame

**Locations:** `platform/gpu_entities.c`, `Alias` and `Draw`;
`renderer/gpu_sdl.c`, `LQ_GPUFrame`.

Alias models duplicate vertices per triangle corner and generate identity
indices. The backend subsequently validates every index and vertex, computes
bounds in another pass, then uploads both arrays.

Indexed reusable meshes and GPU pose interpolation offer larger potential
savings. Account for texture seams when sharing vertices. A smaller first step
is accumulating bounds during construction and avoiding redundant identity-index
generation/upload. Retain validation at appropriate input boundaries.

## 8. Shadow refresh repeats expensive caster classification

**Location:** `renderer/gpu_sdl.c`, the static caster-range loop in
`RenderShadows` and `CasterFaces`.

For each refreshed cube face, the code scans static caster ranges and calls
`CasterFaces`, which computes visibility for all six faces. A full cube rebuild
can repeat that classification six times per range.

Precompute face masks once per light change, ideally using spatially restricted
candidate ranges. Retain spotlight and range rejection and the existing merging
of adjacent index ranges.

## 9. Luminous still computes unused Classic actor lighting

**Location:** `platform/gpu_entities.c`, `Alias`.

Alias construction performs `R_LightPoint`, scans gameplay lights, and
calculates vertex shade even when Luminous lights the actor from runtime lights
and probes.

Select the lighting path before these calculations and skip Classic work where
its result is unused. Verify emissive models, view weapons, and Classic mode
separately.

## Additional smaller targets

- `platform/gpu_entities.c`, `LQ_FlameLight`: scans all static entities for each
  queried light. Cache the binding from a light source to its flame entity.
- `platform/gpu_entities.c`, `LQ_AnimatedMaterial`: searches materials by name
  repeatedly. Cache the mapping from animated texture pointers to materials.
- `platform/ui_scale.c` and `platform/render_bridge.c`: the scaled UI path
  downsamples the framebuffer, expands the UI, then samples it down again for
  GPU overlay conversion. Investigate presenting the logical UI canvas directly
  in GPU mode while preserving coverage, console, menu, and fallback behavior.

## Suggested measurement and implementation order

1. Measure shadow filtering and camera culling first, using CPU Time Profiler
   and Metal System Trace to distinguish CPU submission costs from GPU work.
2. Evaluate overlay resource reuse, particle batching, and caster matching as
   concrete CPU-side improvements.
3. Use both stationary scenes and turning/combat sequences. Stationary
   measurements alone previously missed shadow-cache churn.
4. Compare repeatable optimized native arm64 runs at multiple resolutions;
   record average costs and frame-time tails, and control for thermal state.
5. Check visual output and map-change/video-restart behavior after each change.

This review made no implementation changes and ran no performance benchmarks.
