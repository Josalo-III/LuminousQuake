# Enhanced renderer — October 15, 2026

Enhanced lighting inputs are part of the shared renderer scene. The standalone
viewer lights the world from runtime lights with falloff and soft shadows, but
the Enhanced menu choice stays disabled. Classic GPU remains the playable
reference; its implementation is the starting point for the upgraded renderer.

## Shared and replaceable components

| Component | Enhanced direction |
| --- | --- |
| PAK/loose-file loading, original BSP/MDL/sprite formats | Retain; no replacement game data required |
| GPU buffers, resource ownership and map/restart lifecycle | Retain and extend |
| Visibility, animated textures/models, moving brushes, particles | Retain; camera visibility is not light/shadow visibility |
| Sky, water, palettes, fullbrights and transparency | Retain existing behavior until a particular effect is deliberately superseded |
| Translucent liquids (`r_wateralpha` < 1) | Lit by runtime lights and static probes; light crossing the surface scales by 1 − opacity (direct and static bounce) |
| Filtering, mipmaps, anisotropy, MSAA, resolution/FOV and VSync | Shared settings; retain relevant implementations |
| HUD, menus, console, UI scaling, input and saved settings | Shared |
| Baked lightmaps and Classic model shading | Preserve unchanged for Classic and comparison; Enhanced replaces direct light, with an explicit map-local static composition patch only when reconstruction and authored controls cannot retain an important original scene |
| QuakeC, physics, networking, saves and demos | Shared gameplay; rendering changes must not alter their behavior |

## Implemented foundations

- World-space normals in the renderer-owned vertex format: oriented BSP face
  normals, transformed brush normals and rotated MDL vertex normals. Sprites and
  particles retain zero normals for their existing unlit/effect paths. These are
  geometry normals, not inferred texture normal maps. The Enhanced branch uses
  them for incidence; Classic lighting ignores them.
- A bounded, owned inventory of light entities read from the retained BSP entity
  text. Ordinary QuakeC light spawning can discard gameplay entities without
  destroying these descriptions. No VM fields or engine entity layouts change.
- Source entity index, classname, origin, intensity, style, initial spawnflags,
  target/targetname and cone angle. Raw light/color values are retained for later
  interpretation. Missing/zero scalar intensity follows the original LIGHT
  compiler's default of 300; that value is not yet a runtime light radius.
- Rerelease `_color` values are accepted as three finite nonnegative components,
  in either normalized 0–1 or byte-scale 0–255 form. Malformed colors, extended
  four-component `_light` values, invalid scalar inputs and truncated fields are
  marked unresolved. Nonfinite origins and invalid styles are omitted.
- A pure helper applies live lightstyle values to supported source intensity.
  Initial spawnflags remain metadata: they must not hold a switched light off
  after the authoritative live style turns it on. The helper is a scalar input
  calculation, not a spatial light evaluation or a shadow test.
- `luminousquake-gpu --scene-info` reports inventory counts; add `--light-info`
  for source details, without creating a GPU/window:

  ```sh
  ./build/luminousquake-gpu -basedir "$PWD/quake-complete/Quake" -map e1m1 --light-info
  ```

The local, unverified E1M1 contains 252 captured sources: 5 targeted and 14 with
nonzero styles. All 87 previously unresolved sources used valid `_color` values
and are now parsed; E1M1 and E1M3 report zero unresolved descriptions. These
counts describe that installation and do not prove runtime equivalence to its
baked lighting. Installed assets remain unchanged.

## Runtime direct light and shadows

The shared GPU shader has an explicitly gated Enhanced branch. It lights world
surfaces only from runtime lights; the baked lightmap is not sampled. Darkness
comes from each light's falloff and from occlusion.

- **Falloff.** Each source keeps its compiler semantics: LIGHT/ericw-tools
  `delay` 0–5 (linear, 1/x, 1/x², none, local minlight, 1/(x+128)²), `wait`
  attenuation, worldspawn `_range`/`_dist`/`_anglescale`, and LIGHT's softened
  incidence term `(1 − s) + s·n·l`. `LQ_LightSourceRange` gives the distance at
  which a source adds under half a brightness unit; unattenuated sources are
  bounded at 16384 units. Local-minlight sources are not direct lights and stay
  out of this path.
- **Spotlights.** `mangle` (yaw, pitch up) or a matched target aims a cone whose
  full angle is `angle` (default 40°). Unmatched targets remain point lights, as
  in LIGHT. The cone edge is blurred over ±(16 + emitter radius) world units at
  the receiver (`LQ_SpotFactor`, shared by shader and bounce). That matches the
  softness the bake got from LIGHT's 16-unit lattice and filtering; without it,
  accent spots on key items render as hard-edged discs. An authored `_softangle`
  adds a smoothstep inner cone.
- **Accent spotlights.** Spotlights using ericw's unbounded falloffs (`delay` 1,
  2, 3 or 5) are the designers' accents on key items, and they run far past
  saturation: `light 2500` unattenuated is about five times full white. Enhanced
  scales their output by `LQ_ACCENT_SPOT_GAIN` (0.15) at load, so bounce and
  probes dim with them. They then draw the eye in the darker scene without
  dominating it. Linear spotlights, such as id's targeted fixtures, keep their
  compiler strength. The viewer's `--accent-gain G` overrides the gain for
  comparison; 0.3 still dominates the wall, and 0.08 nearly disappears.
- **Selection.** Up to 32 sources are chosen per frame from those whose PVS reach
  (leaves their own leaf can see, within range) meets the camera's PVS. A
  selected source keeps its slot, and so its cached shadow cube, while it stays
  selected, including while its style is momentarily dark.
- **Occlusion.** Each slot owns a 512² reversed-depth (`near / distance`) D32 cube
  in a cube-array texture. The sealed BSP world casts with the faces turned away
  from the light, so receivers never compare against themselves and no large
  depth bias is needed. Water does not cast. A cube is redrawn in full only when
  its slot's light or position changes.
- **Moving casters.** In game, actors cast with their live geometry: monsters,
  items and ammo/health boxes, and also moving brush models (doors, lifts,
  plats). The view weapon, flames, particles and sprites do not cast. The static
  caster buffer holds only the world model; the viewer adds brush models at their
  compiled positions, since nothing moves there. Each frame, the bounds of every
  moving caster are tested against each slot's range and its six face frusta.
  Only the faces they touch are redrawn (static world plus those casters), along
  with faces they touched when last drawn, so old shadows are erased. Static
  shadows elsewhere stay cached.
- **Penumbrae.** Lights have an emitter radius: ericw `_deviance` when present,
  otherwise 8 units. PCSS finds the mean blocker depth and filters over
  `radius × (receiver − blocker) / blocker`, so contact shadows stay sharp and
  distant occluders soften. Taps use the receiving plane's own depth, so tilted
  surfaces do not shadow themselves. A point source (radius 0) gives the hard
  geometric shadows that motivated this.

Commands (standalone viewer only; `lq_renderer 2` is not connected):

```sh
./build/luminousquake-gpu -basedir "$PWD/quake-complete/Quake" -map e1m3 --enhanced
./build/luminousquake-gpu -basedir "$PWD/quake-complete/Quake" -map e1m3 --enhanced --no-shadows
./build/luminousquake-gpu -basedir "$PWD/quake-complete/Quake" -map e1m3 --enhanced \
  --light-radius 0 --camera "-620 -1800 200 10 270" --fixed-camera --run-seconds 1 --screenshot /tmp/e1m3.bmp
```

`--light-info` also lists each source's formula, attenuation, range and cone.
On exit the viewer reports active slots and how many cubes were drawn.

The earlier prototype looked fullbright in E1M3 because it treated every source
as linear with a radius equal to its `light` value. That map's 32 `delay 3`
spotlights (`light 2500`) then covered everything without a cone. With the
correct formulas and cones, falloff alone restores the dark rooms; shadows then
add the occlusion. The scene matches Classic most closely where the bake was
mostly direct light. Where it differs, the cause is usually bounce and dirt, as
under torch brackets: the bracket's real shadow is filled in Classic by the
map's compiled `_bounce`.

## Bounce light

Light also bounces off lit textures as a secondary source. At map load,
`LQ_SceneComputeBounce` runs on the worker pool:
1. **Direct light per sample.** For each sample on LIGHT's 16-unit lattice, sum
   every source's direct light using the same falloff, cone and BSP occlusion as
   the runtime path. Multiply by the face's mean texture color to get exitance.
2. **Gather.** Each sample casts 64 cosine-weighted rays through the world BSP.
   The ray-to-face test follows GLQuake's RecursiveLightPoint, with exact convex
   polygon containment. The mean exitance of the surfaces hit is the bounced
   irradiance.
3. **Scale.** Multiply by worldspawn `_bouncescale` (default 1).

Results are kept per light style: each face keeps its four strongest bounce
styles in four stacked layers of an atlas that mirrors the lightmap layout. The
shader weights each layer by its live style value, so switched and flickering
lights still drive their bounce. The baked lightmap is never read.

One bounce is gathered. E1M3 takes 1.6 s on six workers (183,046 samples × 64
rays); E1M1 takes 1.7 s. A closed white room test checks hits, cosine weighting,
style layers and albedo. E1M3's stone textures reflect only about 20–25% and
the map sets `_bouncescale 0.5`, so physically based bounce there is subtle
(mean 3 brightness units).

Viewer tuning flags:
- `--exposure S` scales all Enhanced light; below 1 is darker.
- `--bounce-gain G` scales bounce artistically.
- `--no-bounce` disables bounce.
- `--bounce-rays N` sets the rays per sample.

For example, `--exposure 0.7 --bounce-gain 8` gives a darker scene with a
visible fill. The defaults (1, 1) are physically based.

## In-game experimental mode and weapon light

`lq_renderer 2` (Options → Renderer → Enhanced, then `vid_restart`) selects the
GPU path with Enhanced lighting. At map load it computes bounce for the live
world.

Each frame, map lights fill 24 stable slots. The nearest live gameplay dlights
take the last eight:
- rockets, muzzle flashes and explosions;
- powerup glows (quad/pentagram, `EF_DIMLIGHT`/`EF_BRIGHTLIGHT`).

Each dlight keeps Quake's radius as its value and range, so a surface facing it
gets the same `radius − distance` as Classic. It has a 4-unit emitter and a warm
fire tint, since Quake dlights carry no color. Its shadow cube is redrawn every
frame as it moves.

Scripted E1M1 captures (rocket launcher fired from the start) show the muzzle
flash, the rocket in flight and its explosion lighting the corridor. An ASan
build ran the same session without reports. Switching between Classic and
Enhanced rebuilds the map for the new mode.

## Probe grid: actors and moving-light bounce

The hybrid's dynamic half is an irradiance probe grid over the world bounds,
64 units apart. The spacing widens for very large maps, staying within 3D
texture limits. Each probe stores an ambient cube: irradiance toward ±X, ±Y and
±Z, in brightness units.

- **Static layer.** Computed at map load right after the face bounce. Each
  open-space probe casts 96 evenly spread rays. The surfaces they hit contribute
  their direct exitance plus their bounce times texture color. Probes inside
  solid copy their neighbours, so filtering near walls does not darken. Layers
  are map-wide: style 0, then the three strongest other styles. Animated styles
  that miss out fold into style 0; switchable ones are left out rather than shown
  while off. On E1M3 the grid is 57×53×25 (4,525 probes in open space), adding
  little to the 1.7 s load.
- **Dynamic layer.** Rebuilt every frame on the worker pool, only for probes near
  active gameplay lights. 16 rays per probe find the surfaces a rocket, muzzle
  flash or explosion lights (with a BSP occlusion test toward the light) and
  gather what they reflect. World surfaces and actors both sample it, so weapon
  light bounces onto nearby walls.
- **Actors.** MDL models (except flames, which stay fullbright emitters), the
  view weapon and standalone ammo/health boxes carry `LQ_SURFACE_ACTOR`. In
  Enhanced they are lit per pixel by the same runtime lights and shadow cubes as
  the world, plus static and dynamic probe irradiance. The view weapon keeps
  Quake's 24-brightness minimum. Classic ignores the flag and uses `shade`.

The closed-room test checks probe magnitudes against hand arithmetic: a centre
probe of about 105 in all six directions at 0.4 reflectance, and a muzzle-flash
bounce inside its expected band. A GPU readback checks that an actor is lit by a
runtime light in Enhanced and keeps its vertex shade in Classic.

E1M1 profile while firing rockets (`lq_profile`, 7 s after warm-up, M1 Max,
640×480):

| Mode | CPU per frame | Scene preparation |
| --- | ---: | ---: |
| Classic | 3.6 ms | 0.5 ms |
| Enhanced | 4.2 ms | 0.9 ms |

Enhanced figures include the dynamic probes. Interactions are built only for
visible faces, using cached bounds. GPU time is not measured.

The six-axis GPU test also drives a moving actor box. Exactly 6 faces are
redrawn for a new cube, 1 while the box stays, 1 when it moves (and the shadow
follows), 1 to erase it after removal, then none. In demo1, an explosion beside
a monster throws the monster's silhouette across the wall and ceiling behind it.
With monsters moving near lights, demo1 holds Classic's frame rate. Average
presentation/submission CPU is 3.6 ms (9 ms at worst), covering shadow redraws
and dynamic probes.

Current limits:

- Particles and sprites keep Classic shading. Actor silhouettes cast from back
  faces chosen per vertex normal, which can thin shadows at grazing angles.
  Brush-model faces are lit and gather bounce at their compiled positions.
- Face bounce is static. Weapon light reaches surfaces only through the probe
  grid, which is coarser than the bounce atlas. Probes do not test visibility,
  so light can leak through thin walls between probe cells.
- 24 shadowed map lights per view, so lights can pop as the set changes. Bounce
  and probes take 1–2 s of map load and are not cached yet.
- PVS reach may exclude surfaces lit through water if the map was vised with
  opaque water.
- Only Metal has run; the Vulkan path, including `imageCubeArray`, is untested.
  Shadow memory is about 200 MB at 512² for 32 cubes; the bounce atlas adds up to
  four layers the size of the used lightmap rows.

Quake's stored data is a baked lightmap rather than a shadow map. Enhanced keeps
that image as evidence of the original composition: it can guide comparisons,
light reconstruction and deliberate darkness, but it is not sampled or multiplied
into Enhanced direct illumination. Runtime lights and geometry must produce the
lighting. Where the surviving entity descriptions cannot reproduce an authored
choice, an inspectable per-map override or indirect-light control is preferable to
quietly restoring baked direct light.

If those controls still cannot recreate an important area, Enhanced may use a
bounded, documented composition patch in the spirit of Tenebrae's dim static
lightmap base. It is a named map-region fallback, not a general lightmap
multiplier: it must retain only the needed stable residual, leave switched and
moving illumination to the live light system, and be visible in debug tools.

[Light reconstruction](LIGHT_RECONSTRUCTION.md) defines the evidence available
from an installed BSP, the original compiler prior, estimation limits and
the derived-cache workflow. The baked result does not uniquely determine the
lights that produced it: Classic is the reference for a close experience, while
synthetic scenes test specific runtime-light and shadow behavior. Estimated
lights are a starting rig; explicit per-map adjustments will be needed where
the original scene relied on an artistic cheat or an irrecoverable choice. In
particular, the entity `light` scalar is not treated as an authored physical
power or range; the viewer's present linear influence is a deliberately
temporary value.

## Validation

Generated fixtures exercise BSP-to-owned-light conversion: default and explicit
intensity, color normalization, style/target/cone metadata, `delay`/`wait`
formulas, matched-target and `mangle` spotlights with `_softangle`, rejection of
invalid formulas and extended values, and invalid origins/styles. Scene tests
cover every falloff formula and its range, bounded deterministic selection,
stable slots as the camera moves, PVS reach through a two-leaf BSP, held dark
sources, finite normals, ownership and spot/falloff validation.

The GPU test builds a controlled scene on each of the six cube axes: a point
light, a closed box hidden from the camera and a receiving wall. Readback checks
that the shadow falls at the diagonally offset position (a face flip on either
axis would fail), moves with the light, reuses the cached cube when nothing
changes and disappears beyond range. It also checks that a point source gives a
hard edge and a 32-unit emitter a graded penumbra. Both native suites (normal and
ASan/UBSan) pass all 21 tests, and an E1M3 Enhanced run is sanitizer-clean.
Other-platform runtime validation remains pending.

## Next implementation sequence

1. Probe visibility weighting against wall leaks, and a cache for bounce/probes
   so map load does not repeat the gather.
3. Implement the staged [light reconstruction](LIGHT_RECONSTRUCTION.md): retain
   style-layer evidence, compare the runtime prediction, estimate only
   identifiable parameters, and report residual/confidence data.
4. Enable an explicitly experimental in-game Enhanced path, keeping Classic for
   comparison. Normal/specular inputs and deliberate atmosphere adjustments
   follow. A map-local static composition patch is a last resort, used only
   where falloff, occlusion and authored controls cannot recover a scene.

No normal-map, specular or fog claim follows from this checkpoint. Enhanced asset
creation remains in the separate data project.
