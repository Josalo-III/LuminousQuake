# Enhanced renderer foundations — October 14, 2026

The first Enhanced implementation slice adds lighting inputs to the shared
renderer scene. It does not change visible lighting or enable the Enhanced menu
choice. Classic GPU remains the playable reference; its implementation is the
starting point for the upgraded renderer.

## Shared and replaceable components

| Component | Enhanced direction |
| --- | --- |
| PAK/loose-file loading, original BSP/MDL/sprite formats | Retain; no replacement game data required |
| GPU buffers, resource ownership and map/restart lifecycle | Retain and extend |
| Visibility, animated textures/models, moving brushes, particles | Retain; camera visibility is not light/shadow visibility |
| Sky, water, palettes, fullbrights and transparency | Retain existing behavior until a particular effect is deliberately superseded |
| Filtering, mipmaps, anisotropy, MSAA, resolution/FOV and VSync | Shared settings; retain relevant implementations |
| HUD, menus, console, UI scaling, input and saved settings | Shared |
| Baked lightmaps and Classic model shading | Preserve for Classic and comparison; replace the direct-light contribution in Enhanced rather than multiplying baked lighting into new direct illumination |
| QuakeC, physics, networking, saves and demos | Shared gameplay; rendering changes must not alter their behavior |

## Implemented in this slice

- World-space normals in the renderer-owned vertex format: oriented BSP face
  normals, transformed brush normals and rotated MDL vertex normals. Sprites and
  particles retain zero normals for their existing unlit/effect paths. These are
  geometry normals, not inferred texture normal maps. Current shaders still use
  Classic lighting; the normal data is preparation for their successors.
- A bounded, owned inventory of light entities read from the retained BSP entity
  text. Ordinary QuakeC light spawning can discard gameplay entities without
  destroying these descriptions. No VM fields or engine entity layouts change.
- Source entity index, classname, origin, intensity, style, initial spawnflags,
  target/targetname and cone angle. Raw light/color values are retained for later
  interpretation. Missing/zero scalar intensity follows the original LIGHT
  compiler's default of 300; that value is not yet a runtime light radius.
- Extended/colored light values, invalid scalar inputs and truncated descriptive
  fields are marked unresolved rather than silently treated as supported lights.
  Nonfinite origins and invalid styles are omitted with diagnostics. No guessed
  illumination is submitted for unresolved descriptions.
- A pure helper applies live lightstyle values to supported source intensity.
  Initial spawnflags remain metadata: they must not hold a switched light off
  after the authoritative live style turns it on. The helper is a scalar input
  calculation, not a spatial light evaluation or a shadow test.
- `luminousquake-gpu --scene-info` reports inventory counts; add `--light-info`
  for source details, without creating a GPU/window:

  ```sh
  ./build/luminousquake-gpu -basedir "$PWD/quake-complete/Quake" -map e1m1 --light-info
  ```

The local, unverified E1M1 contains 252 captured sources: 5 targeted, 14 with
nonzero styles, and 87 marked unresolved. These counts describe that installation;
they are not a claim about every release or proof that those sources reproduce
its baked lighting. Installed assets remain unchanged.

## Validation

Generated fixtures exercise BSP-to-owned-light conversion, default and explicit
intensity, style/target/cone metadata, unresolved extended light values and
rejection of invalid origins/styles. Scene tests cover finite normals, source
ownership, live style changes and initially switched-off lights. Existing Metal
pixel tests exercise the enlarged vertex stride while checking the Classic
image, actor submission, masks, palette filtering, mipmaps and anisotropy.
Both complete native suites pass all 20 tests, including Metal readback, in
normal and ASan/UBSan builds. The local E1M1 inventory also loads and releases
under sanitizers. Other-platform runtime validation remains pending.

## Next implementation sequence

1. Extend the implemented Phase 4A timings and bounded worker pool only when a
   measured Enhanced workload has owned immutable inputs and disjoint results.
   Keep the render/window owner and gameplay serial.
2. Resolve target directions and colored/extended source parameters against
   documented source semantics. Define intensity/range/attenuation and material
   fallbacks using synthetic scenes before calibrating original maps. Preserve
   unsupported metadata and report limits.
3. Establish light/surface/caster relationships, visibility and a portable shadow
   proof. Test blockers, moving objects, light toggles and camera changes. Camera
   PVS alone cannot establish a light's influence or prevent illumination through
   walls. Preserve original lighting choices about what remains hidden.
4. Enable an explicitly experimental Enhanced path using original assets once
   direct illumination and occlusion work together. Preserve the Classic path for
   comparison. Keep baked lightmaps as calibration/reference data, avoiding double
   counting. Normal/specular inputs and deliberate atmosphere adjustments follow.

No shadow, normal-map, specular, fog or runtime-lighting claim follows from this
foundation checkpoint. The worker pool currently handles only UI and palette
conversion; Enhanced asset creation remains in the separate data project.
