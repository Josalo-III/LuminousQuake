# Luminous renderer — October 15, 2026

Luminous is an experimental playable renderer (`lq_renderer 2`), using runtime
lights, soft shadows, bounce, probes and sky illumination. It was called
Enhanced until October 16, 2026, and was renamed to avoid confusion with the
existing Enhanced Quake; the setting and its saved value are unchanged, and
the viewer still accepts `--enhanced`. **Classic GPU is the
gold standard for testing lighting:** compare the same map, camera, lightstyle
and gameplay event against it to judge composition, darkness and effect timing.
Synthetic scenes test individual mechanisms; they complement the Classic GPU
comparison. Luminous's additional shadows and colored light need not match every
Classic pixel, but changes must preserve the readability of its lighting.

## Shared and replaceable components

| Component | Luminous direction |
| --- | --- |
| PAK/loose-file loading, original BSP/MDL/sprite formats | Retain; no replacement game data required |
| GPU buffers, resource ownership and map/restart lifecycle | Retain and extend |
| Visibility, animated textures/models, moving brushes, particles | Retain; camera visibility is not light/shadow visibility |
| Sky, water, palettes, fullbrights and transparency | Retain existing behavior until a particular effect is deliberately superseded |
| Sky and opaque liquids | Drawn with the palette's own colours (colormap row 32) in Classic and Luminous, as software Quake draws them. Both GPU renderers first used row 0, which is about twice as bright and clips: id1's palette collapses to 132 colours there, and an E1M1 sky crop showed 4. The Luminous sky is always filtered bilinearly, whatever the texture filter: each layer within its own half of the texture, with the front layer's index-0 mask as fractional coverage so cloud edges feather, blended in linear light. Sky light is computed from the same colours; `LQ_SKY_LIGHT_GAIN` 2 keeps the outdoor light level tuned against the row-0 sky |
| Translucent liquids (`r_wateralpha` < 1) | Lit by runtime lights and static probes; light crossing the surface scales by 1 − opacity (direct and static bounce) |
| Filtering, mipmaps, anisotropy, MSAA, resolution/FOV and VSync | Shared settings; retain relevant implementations |
| HUD, menus, console, UI scaling, input and saved settings | Shared |
| Baked lightmaps and Classic model shading | Preserve unchanged for Classic and comparison; Luminous replaces direct light, with an explicit map-local static composition patch only when reconstruction and authored controls cannot retain an important original scene |
| QuakeC, physics, networking, saves and demos | Shared gameplay; rendering changes must not alter their behavior |

## Implemented foundations

- World-space normals in the renderer-owned vertex format: oriented BSP face
  normals, transformed brush normals and rotated MDL vertex normals. Sprites and
  particles retain zero normals for their existing unlit/effect paths. These are
  geometry normals, not inferred texture normal maps. The Luminous branch uses
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

The shared GPU shader has an explicitly gated Luminous branch. It lights world
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
  saturation: `light 2500` unattenuated is about five times full white. Luminous
  scales their output by `LQ_ACCENT_SPOT_GAIN` (0.15) at load, so bounce and
  probes dim with them. They then draw the eye in the darker scene without
  dominating it. Linear spotlights, such as id's targeted fixtures, keep their
  compiler strength. The viewer's `--accent-gain G` overrides the gain for
  comparison; 0.3 still dominates the wall, and 0.08 nearly disappears.
- **Flame lights.** A map light with a static flame model at its origin
  (`flame.mdl` on wall torches, `flame2.mdl` on flame balls) follows that
  flame's animation (`LQ_FlameLight`). Each pose's emission is the model's
  surface area weighted by the luminance of the skin it shows, sampled when
  the model is first seen. The light's strength follows each pose's emission
  relative to the cycle's mean. On the shareware models, a wall torch runs
  0.88–1.14× over its 0.6 s cycle and the large flame 0.82–1.13× over 1.1 s.
  The light's position follows the shift of the emission centre, about a unit
  up in the bright poses and down in the dim ones. The bracket a few units
  below the flame therefore casts a shadow that moves on distant walls. Quake
  starts every static flame's cycle together, which would pulse a whole room
  in step, so Luminous gives each flame, model and light alike, its own phase
  derived from its position. The flame mesh keeps its authored poses. With
  Smooth animation (`r_lerpmodels`), the light's strength steps at 20 Hz:
  each pose's value, then one a third of the way to the next pose's value
  through the first half of the loop, two thirds through the second. The light
  moves only when the authored pose changes (ten times a second), so its
  shadow cube is redrawn at that rate rather than every frame. Static bounce
  and probes keep the mean strength. Classic and
  Software keep the original lockstep animation.
- **Fluorescent tubes** (`renderer/fluorescent.c`). `light_fluoro` and
  `light_fluorospark` (the base levels: e1m1 has 6 and 2, e4m1 17 and 4)
  flicker with US mains as a camera records them. A tube on a magnetic ballast
  lights on both halves of each 60 Hz cycle: its drive is a rectified sine,
  1 − ⅔cos 2ωt − ²⁄₁₅cos 4ωt relative to its mean, plus a 60 Hz term from worn
  electrodes (3% steady, 20% failing). Each phosphor filters the drive as a
  first-order low-pass (red 4 ms, green 2 ms, blue 0.5 ms), so blue modulates
  deepest and the tube is bluer at its peaks. A camera's shutter T keeps
  sinc(πfT) of a term at f Hz, and its frames see the alias of f by its frame
  rate. Luminous evaluates the alias at continuous time, which matches the
  camera's samples at its frame instants and does not depend on the display's
  rate. `r_fluorescent` picks the camera: 0 eye (fused, steady); 1 film,
  23.976 fps with a 180° shutter (default): 120 Hz aliases to 0.12 Hz, so the
  light breathes over about 8 s, peak to peak 6% red, 10% green and 17% blue;
  2 NTSC 29.97 fps at 1/60, whose shutter spans whole cycles, so it is steady
  (why US video needs no sync); 3 NTSC at 1/250, the same beat at 30% / 51% /
  85%; 4 PAL 25 fps at 1/50, a 5 Hz flutter at film's depth. The grid wanders
  by 0.015 Hz over 97 s, so the beat drifts. Tubes within one 256-unit cell
  share a circuit, and circuits sit on one of three phases. A plain light
  within 24 units of a tube is part of its fixture (the rerelease adds a beam
  spotlight beside each tube) and follows it. Static bounce of steady tubes
  keeps the mean.
- **Failing tubes.** Each `light_fluorospark` on style 0 or 10 (and its fixture
  lights) gets a private style from 64 up at load, before bounce, so its face
  bounce layer goes dark and returns with it. Its timeline is a pure function
  of time and position, cut into 1.6 s windows: nothing (35%), a dropout of
  35–140 ms (35%), chatter of 3–6 short dropouts (15%), or a restrike (15%),
  where the starter heats the cathodes (5%, orange) and fails with a flash (80%,
  bluish) once or twice before the arc holds. A struck arc overshoots by up to
  45%, bluish, decaying over 60 ms. Over an hour a tube is dark 4.3% of the
  time and glowing 4.1%, with 0.7 outages a second. Probe layers are map-wide
  and keep only the three strongest non-zero styles, so actors may miss a
  failing tube's bounce. Classic and Software keep style 10.
- **Selection.** Up to 128 sources are chosen per frame from those whose PVS
  reach (leaves their own leaf can see, within range) meets the camera's PVS,
  nearest (range-weighted) first. In game, map lights get 120 slots and gameplay
  lights the last 8. When more sources qualify than there are slots, those that
  light no visible face are dropped first. Then those that light a face on
  screen rank ahead of those lighting only faces elsewhere in the PVS, and
  nearness orders each group. The screen test widens the view by 30°
  (`LQ_SELECTION_FOV_MARGIN`), so lights just out of view already hold slots
  and drawn cubes as the player turns. Nearness alone let lights behind the
  camera displace those on what the player was looking at. Backing away from
  E1M1's first building turned its facade lights off, and translucent water
  (`r_wateralpha` < 1) made this worse by adding the faces across the water.
  Confirmed fixed in play at that spot (October 17, 2026).
  The face scan walks each visible face's precomputed reach bits, so
  selection costs about 0.6 ms a frame at that spot, half its earlier cost. The limit comes from the shader's
  light uniform block: 80 bytes per light, 10 KB for 128, inside the 16 KB every
  Vulkan device guarantees. Each face carries a 128-bit mask of the lights that
  reach it, and the shader visits only the set bits; actors, effects and moving
  brushes get the lights whose range meets their bounds. A selected source keeps
  its slot, and so its cached shadow cube, while it stays selected (including
  while its style is momentarily dark) and while its rank stays within half
  again of its resolution band. LIGHT summed every source at every sample; this
  previously stopped at 24 map lights, which left E1M1's rerelease exterior
  facade unlit (50 of the 70 lights reaching that view were dropped; now 3).
- **Occlusion.** Each slot owns a reversed-depth (`near / distance`) D32 cube:
  gameplay lights and the 8 highest-ranked map lights at 512², the next 32 at
  256², the remaining 80 at 128², 174 MB in all. A cube is six layers of a 2D
  array texture and the shader selects the face. SDL 3.4's Metal backend sizes
  cube arrays six times too large (it passes the layer count as Metal's cube
  count), and SDL addresses depth-target layers with 8 bits, so each array holds
  at most 42 cubes: the 128² cubes span two arrays. Both sides of occluders cast, retaining the nearest
  wall surface instead of only its exit face. This seals grazing joins such as
  the first E1M1 lift entrance. Receiver-plane comparisons and a one-texel normal
  offset keep the receiving face from shadowing itself. Water and sky do not
  cast: LIGHT's `TestLine` stops only at solid space, so a light in a sky
  volume still reaches the room below. The bounce pass traces light-to-sample
  visibility the same way; its gathering rays still end at sky, which is the
  sky light's emitter. A cube is redrawn in full only when its slot's light or
  position changes.
- **Moving casters.** In game, actors cast with their live geometry: monsters,
  items and ammo/health boxes, and also moving brush models (doors, lifts,
  plats). The view weapon, flames, particles and sprites do not cast. The static
  caster buffer holds only the world model; the viewer adds brush models at their
  compiled positions, since nothing moves there. Each frame, the bounds of every
  moving caster are tested against each slot's range and its six face frusta.
  Only the faces they touch are redrawn, along with faces they touched when last
  drawn, so old shadows are erased. A caster whose bounds match the previous
  frame's is at rest: it is drawn into any face that is redrawn but does not
  cause a redraw itself, so standing doors and lifts cost nothing. Each cube also
  has a static-only copy, drawn when the light changes. A face redrawn for moving
  casters is copied from it and only the casters are drawn on top, rather than the
  whole world again (Doom 3 keeps static and entity shadows apart the same way).
  On E1M3's start (1080p, 2× MSAA, VSync off) this took Luminous from 76 to 84
  fps. Monsters idling within reach of about 19 lights still redraw some 1,100
  cube faces a second, now cheaply. Static shadows elsewhere stay cached.
- **Light volumes.** After Doom 3, a light acts only where it can reach.
  Linear lights end exactly. Inverse and inverse-square lights stop where they
  fall below 1 brightness unit (`LQ_LIGHT_FADE_LEVEL`) or the map's higher
  ericw-tools `_gate`, and taper to zero over the last fifth of that range so no
  edge shows. No-falloff lights are regional by design (the rerelease's
  sunbeams) and keep the full map extent. A spotlight lights only surfaces its
  blurred cone can reach (`LQ_SpotReachesBox`), and draws only the cube faces
  the beam covers; faces outside it are cleared and read as unshadowed. Within
  64 units of the lamp, edge spill outside those faces goes unshadowed.
- **Shadow cutoffs.** A light below `lq_shadow_faint` brightness (default 4) at
  a point, or below `lq_shadow_dominance` (default 5%) of the brightest light
  there after that light's own shadow, is not shadow-filtered; each blends in
  between its threshold and twice that. On E1M3's start this changed at most 3
  of 255 levels.
- **Measured** on E1M3's start (1080p, 2× MSAA, VSync off): Luminous went from
  76 fps to 109 with the caster changes (rest detection, static copy, per-face
  culling), and about 110 with cones, falloff and cutoffs; the last three are
  within run-to-run variation at that spot.
- **Penumbrae.** Lights have an emitter radius: ericw `_deviance` when present,
  otherwise 8 units. PCSS finds the mean blocker depth and filters over
  `radius × (receiver − blocker) / blocker`, so contact shadows stay sharp and
  distant occluders soften. Taps use the receiving plane's own depth, so tilted
  surfaces do not shadow themselves. A point source (radius 0) gives the hard
  geometric shadows that motivated this. Grazing taps that cannot reach the
  receiver contribute no light; treating them as unoccluded opens bright seams.

Commands (standalone viewer; the playable client uses `lq_renderer 2`):

```sh
./build/luminousquake-gpu -basedir "$PWD/quake-complete/Quake" -map e1m3 --luminous
./build/luminousquake-gpu -basedir "$PWD/quake-complete/Quake" -map e1m3 --luminous --no-shadows
./build/luminousquake-gpu -basedir "$PWD/quake-complete/Quake" -map e1m3 --luminous \
  --light-radius 0 --camera "-620 -1800 200 10 270" --fixed-camera --run-seconds 1 --screenshot /tmp/e1m3.bmp
```

`--light-info` also lists each source's formula, attenuation, range and cone.
On exit the viewer reports active slots and how many cubes were drawn.

`--light-budget` reports, without opening a window, how many map lights could
light faces seen from `--camera` and how many the slot limit drops there, for the
game's 120 map slots and the viewer's 128. A light counts when a face is in its PVS
reach and range and in front of it. The report covers the camera's whole PVS
(what the game submits), then faces whose bounds meet a 4:3 view. It lists the
lights the game drops for faces in view, strongest first, by falloff at the
nearest such face. That value is an upper bound: it ignores incidence, spot
cones and shadows. At E1M1's exterior (`176 1240 -180 -28 90`, rerelease data)
70 lights reach faces in view; 24 map slots dropped 50 of them, 120 drop 3.
The shareware map at the same camera has 37, all kept.

```sh
./build/luminousquake-gpu -basedir "$PWD/quake-complete/Quake" -map e1m1 \
  --camera "176 1380 -170 -22 90" --light-budget
```

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
`LQ_SceneComputeBounceTraced` computes it:
1. **Direct light per sample.** For each sample on LIGHT's 16-unit lattice, sum
   every source's direct light using the same falloff, cone and BSP occlusion as
   the runtime path. Multiply by the face's mean texture color to get exitance.
2. **Gather.** Each sample casts 64 cosine-weighted rays through the world BSP.
   The ray-to-face test follows GLQuake's RecursiveLightPoint, with exact convex
   polygon containment. The mean exitance of the surfaces hit is the bounced
   irradiance.
3. **Scale.** Multiply by worldspawn `_bouncescale`. A map that sets `_bounce`
   without `_bouncescale` uses 1. A map with neither was baked without bounce
   (every 1996 map) and uses 0.25: enough indirect light for moving and switched
   lights without lifting darkness the original authored without it.
4. **Repeat.** Reflect that bounce off the texture color as the next exitance
   and gather again, as ericw-tools' radiosity does. Each bounce is the last
   times color times scale, so it fades geometrically. Bouncing stops at the
   map's `_bounce` count (at most 16) or once a bounce adds under 1% of the
   first; id maps converge in three or four.

Results are kept per light style: each face keeps its four strongest bounce
styles in four stacked layers of an atlas that mirrors the lightmap layout. The
shader weights each layer by its live style value, so switched and flickering
lights still drive their bounce. The baked lightmap is never read.

A closed room test checks hits, cosine weighting, style layers and albedo for
one bounce. It also checks that repeated bounces at 0.4 reflectance sum to
1/(1 − 0.4) of the first and stop after five to seven bounces. Static probes
see each surface's direct exitance plus its summed bounce through its color.
Gameplay lights' per-frame probe bounce is still a single bounce.

**GPU rays.** The passes that walk the BSP per sample run as a compute shader
(`renderer/shaders/bounce.comp`, through SDL's GPU API on Metal and Vulkan):
sky visibility, sun visibility, direct light and the gather. It uses the same
ray directions, BSP order and polygon containment as the CPU code, with an
explicit stack in place of recursion. The CPU keeps each face's style choice,
the sky and sun reflection, the atlas and the static probes (0.14–0.27 s).
Every pass stays in `bounce.c` as the reference and the fallback if the GPU
cannot run one. Dispatches cover 65,536 samples each, so no command buffer
runs long enough to trip a GPU watchdog on slower hardware.

Whole-bounce time on an M1 Max, 64 rays per sample (measured with a single
bounce; each further bounce repeats only the gather, which on the GPU adds
about 0.2–0.4 s on id maps):

| Map (samples) | CPU, six workers | GPU rays |
| --- | ---: | ---: |
| Shareware E1M2 (188,813) | 4.65 s | 0.75 s |
| Rerelease E1M1 (179,600) | 3.83 s | 0.69 s |
| Rerelease E2M2 (207,615) | 4.01 s | 0.75 s |
| Arcane Dimensions `ad_e1m1` (305,680, sun) | 10.05 s | 1.99 s |

CPU bounce had grown from 1.6 s on E1M3 since the sky became an area light
and the map-light limit rose to 120, which made the Luminous load wait
seconds. `--compare-bounce` in the viewer runs both and reports the
differences. Across eight id maps and `ad_e1m1`, 16 of about 59,000 faces
choose different styles (8 of them on `ad_e1m1`), under 0.04% of atlas values differ by more
than 2 brightness units, and probes differ by at most 1 on id maps. A few
grazing samples, under 0.02%, differ more: a ray landing on the other side of
a face edge or split plane in single-precision GPU arithmetic. Rendered frames
differ only where animated sky and liquids differ between any two runs. E1M3's stone textures reflect only about 20–25% and
the map sets `_bouncescale 0.5`, so physically based bounce there is subtle
(mean 3 brightness units).

Viewer tuning flags:
- `--exposure S` scales all Luminous light; below 1 is darker.
- `--light-gamma G` (1–3, default 2.2) sets the exponent from summed
  brightness to linear light; see [colour](#colour-linear-light-at-32-bits).
- `--bounce-gain G` scales bounce artistically.
- `--bounce-scale S` replaces the map's per-reflection bounce scale.
- `--no-bounce` disables bounce.
- `--bounce-rays N` sets the rays per sample.
- `--cpu-bounce` traces every pass on the CPU.
- `--compare-bounce` computes bounce both ways, prints timings and
  differences, and exits.

For example, `--exposure 0.7 --bounce-gain 8` gives a darker scene with a
visible fill. The defaults (1, 1) are physically based.

## Colour: linear light at 32 bits

Since October 17, 2026, Luminous shades in linear light at full precision
(`renderer/shaders/world.frag`). Classic GPU keeps its palette-exact colormap
path and Classic's model shading. Sky and opaque liquids are unlit; see the
table at the top.

- **Light adds up in Quake's brightness units**, as LIGHT summed it: direct
  light, bounce, sky, sun and probes, where 128 shows a texture at its palette
  colour. The total becomes linear light once, as `(b / 128)^2.2`
  (`LQ_LIGHT_GAMMA`). A texel lit to brightness `b` therefore looks as it does
  in Classic, so Luminous keeps Classic's composition of light and dark. This is
  how DarkPlaces treats lightmaps in its sRGB mode.
- **Surface colour is linear.** Each texel's palette colour (colormap row 32,
  which reproduces the palette exactly: 11 entries point to another index of
  the same colour) is decoded to linear light. It is lit before
  filtering, so bilinear, trilinear and anisotropic filtering mix light rather
  than encoded values. Fullbrights (224–255) emit their colour unlit.
- **No 64-step shading.** The colormap rows no longer quantize light. On a
  ceiling gradient in E1M1 the same crop shows 1,595 distinct colours instead
  of 394. Coloured light is applied per channel in linear light rather than as
  a tint over a grey shade.
- **The display transfer is a pure 2.2 power**, the CRT gamma Quake's palette
  was drawn for. Exact sRGB differs only below about 4%, where its linear toe
  crushed Classic's shadows (E1M3's dark stone went visibly blacker with it).
- **The ceiling is Classic's** until HDR output: twice the palette colour
  (brightness 255), scaled to keep the light's hue.

`--light-gamma 1` reads summed brightness as linear light directly. At four
test cameras it is 17–35% brighter on average, with flatter contrast and
lifted shadows. 2.2 keeps the old composition: mean brightness within 0.3 of
the 8-bit path at all four.

Limits:
- Translucent surfaces (water, particles) still blend in display space, in an
  8-bit target. Blending in linear light arrives with the HDR float target.
- With VSync off and 16× anisotropic trilinear filtering, the old and new
  shaders both ran at about 120 frames a second at E1M1's exterior. No cost
  was visible, but that rate may be a cap, so the GPU cost is not measured.

## In-game experimental mode and weapon light

`lq_renderer 2` (Options → Renderer → Luminous, then `vid_restart`) selects the
GPU path with Luminous lighting. At map load it computes bounce for the live
world.

Each frame, map lights fill up to 120 stable slots. The nearest live gameplay dlights
take the last eight:
- rockets, muzzle flashes and explosions;
- powerup glows (quad/pentagram, `EF_DIMLIGHT`/`EF_BRIGHTLIGHT`).

Each dlight keeps Quake's radius as its value and range, so a surface facing it
gets the same `radius − distance` as Classic. Local presentation metadata now
distinguishes muzzle flashes, rockets/lava balls, explosions and powerup glows:
warm flashes/fire, orange projectiles, and neutral powerup glow. Most emitters
have a 4-unit radius; explosions use 12 units. Their shadow cubes follow the
live light positions. Classic retains the original dlight calculation.

Scripted E1M1 captures (rocket launcher fired from the start) show the muzzle
flash, the rocket in flight and its explosion lighting the corridor. An ASan
build ran the same session without reports. Switching between Classic and
Luminous rebuilds the map for the new mode.

### Particles, sprites and travelling weapon light

Luminous particles use centred irregular polygons with three to six sides and
deterministic variations in rotation, aspect and scale. Side counts are mixed
evenly, with area normalized so extra sides do not make a particle larger.
Classic's triangle is the largest size: particles span 50–100% of its width,
a quarter to all of its area, so the variety only adds smaller flakes.
A particle keeps its shape as it moves; generating
the variation does not consume the gameplay random stream. Classic GPU retains
its original triangles.

Particle emission is recorded when an effect spawns, independently of palette
color. Blood (including gib trails), tar blobs and smoke receive runtime direct
light, static/sky probes and moving-light bounce. They become dark with zero
illumination, including palette entries that Quake normally treats as fullbright.
Small particles and non-emitting sprites gather light from every direction so
their lighting does not depend on their billboard facing the camera. Their
color starts from the palette at Quake's normal brightness (colormap row 32)
and scales with the light they receive, as a wall's does. Until October 16
they started from row 0, about twice as bright, so lit blood and smoke looked
nearly fullbright and pale smoke washed out to white.

| Effect | Self-lit appearance | Illumination source |
| --- | --- | --- |
| Rocket / lava ball | Lava ball self-lit; rocket body receives light | Existing moving projectile dlight, with shadows and bounce |
| Lightning (`bolt.mdl`, `bolt2.mdl`, `bolt3.mdl`) | Yes | Up to six blue-white sources distributed along each live beam |
| Bullet/nail impacts, wizard/knight sparks | Yes | One palette-colored source per impact collection, fading over 0.2 s |
| Explosion particles / `s_explod.spr` | Yes | Existing explosion dlight; generic particle-message explosions get a collection light |
| Flames / `s_light.spr` | Yes | Retained authored map-light sources |
| `s_bubble.spr`, blood, tar and smoke | No | Receive scene light; create no light |
| Tracer trails, lava/teleport particles, laser model | Yes | Existing gameplay/authored lights where supplied; no per-particle lights |

Rocket models and self-lit projectile/beam models do not cast shadows: a source
inside its own closed mesh would otherwise shadow the surrounding world. Monsters
and walls still occlude the travelling light. The bright part of a rocket trail
is self-lit; as its ramp becomes smoke it receives scene lighting instead.

Lightning sources cover the beam between its current endpoints and disappear
when the beam expires. They and spark collections share the existing eight
dynamic shadow slots with gameplay dlights; heavy simultaneous effects can
exceed that budget. Spark lights use a separate bounded event queue, preserving
the Classic dlight pool. Map changes clear the queue.

Validation for this change (October 15): native arm64 Debug and ASan/UBSan
suites each pass all 21 tests, including Metal readback and socket checks. The
GPU tests cover lit-effect darkness, unoriented illumination, fullbright
emission, fullbright palette entries on non-emitting effects, non-casting
self-lit models, and Classic isolation. CPU checks cover particle convexity,
stable variation across 4,096 seeds, all four side counts, no particle larger
than Classic's triangle (the largest close to it), and continuous
light coverage alongside a 600-unit beam.

Paired 800×600 Classic GPU / Luminous gameplay captures cover idle, a rocket in
flight, lightning and shotgun sparks in E1M1. Each uses `host_framerate 0.02`,
the same input sequence, and a paused capture state with fresh temporary settings.
Saved player and rocket states matched between the original renderer pairs.
Particle area now stays at or below Classic's triangle while outline and scale
vary. The polygon checks pass in Debug and ASan/UBSan. A further Luminous rocket
capture verified rendering with the three-to-six-side mixture; its camera moved,
so it is not a matched comparison against Classic.
Luminous shows warm travelling rocket illumination and colored
lighting along the live lightning beam. These bounded samples do not establish
full-campaign lighting equivalence or GPU performance.

The first button-operated lift's left entrance seam reproduces at
`--camera "144 576 88 -5 205"`. Classic has no bright seam; Luminous's leak
persists without bounce. Isolating the spotlight at `-120 504 144` confirms that
direct light crosses a BSP-solid wall. Keeping both sides in the shadow map and
rejecting unreachable grazing taps removes the seam in the repeated capture.
The earlier view at `680 512 110` targeted a different lift.
Playable captures also compare Classic, the previous Luminous build and the
corrected build from the lower landing looking back up, with the lift at
`0 0 -232`. Saved player origins/view angles and lift positions match across all
three captures. A bright sliver at the underside join disappears after the fix.

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
- **Dynamic layer.** Rebuilt every frame, only for probes near active gameplay
  lights. 16 rays per probe find the surfaces a rocket, muzzle flash or
  explosion lights (with a BSP occlusion test toward the light) and gather what
  they reflect. World surfaces and actors both sample it, so weapon light
  bounces onto nearby walls. The CPU picks the lights and the box of probes
  they reach (`LQ_SceneDynamicProbeLights`). The GPU traces them in the frame's
  own command buffer, writing the probe texture directly: `bounce.comp` built
  with `LQ_PROBES`, one thread per ray and 16 to a probe, summed in shared
  memory. The map's BSP, faces and probe validity stay resident. Each dispatch
  also clears the previous frame's box. This replaced about 1 ms of CPU per
  frame during fights in the shareware demos (six workers, plus re-uploading
  the whole 0.6–1.7 MB probe texture) with about 0.3–0.5 ms of GPU work. A
  first version with one thread per probe took 3–4 ms: a few thousand
  probes are too few threads to keep the GPU busy. Without storage-texture
  support, the worker pool and upload remain. The viewer's
  `--compare-probes` runs both, with lights near the camera, then again after
  moving them. On E1M2 and E1M3 they match exactly apart from one value off by 1.
- **Actors.** MDL models (except flames, which stay fullbright emitters), the
  view weapon and standalone ammo/health boxes carry `LQ_SURFACE_ACTOR`. In
  Luminous they are lit per pixel by the same runtime lights and shadow cubes as
  the world, plus static and dynamic probe irradiance. The view weapon keeps
  Quake's 24-brightness minimum. Classic ignores the flag and uses `shade`.

The closed-room test checks probe magnitudes against hand arithmetic: a centre
probe of about 105 in all six directions at 0.4 reflectance, and a muzzle-flash
bounce inside its expected band. A GPU readback checks that an actor is lit by a
runtime light in Luminous and keeps its vertex shade in Classic.

## Sky light

The sky is a diffuse area light whose radiance is the mean color the map's sky
material draws with (front layer over back, colormap row 0, area-weighted over
sky faces). Brightness 128 shows a texture at its palette color, so a white
surface under a fully open sky matches the sky's own color; E1M1's day sky
gives 89/105/119, E1M2's dusk sky 43/24/29 (printed with the bounce timing at
load). `LQ_SKY_LIGHT_GAIN` scales it.

- **Baked visibility, live radiance.** At map load each bounce sample casts the
  bounce pass's 64 cosine-weighted rays; the fraction reaching sky is its
  irradiance from a unit-radiance sky over π. It is stored in the alpha of
  bounce layer 0 and multiplied by the sky uniform in the shader, so the sky's
  whole hemisphere lights a surface with its occlusion (eaves, canyon walls,
  window wells) at the bounce atlas's 16-unit resolution. Static probes store
  the same visibility per ambient-cube direction (layer 0 alpha) for actors
  and translucent liquids.
- **Sky bounce.** Sky-lit samples reflect `visibility × sky × albedo` into the
  style 0 bounce, so open doors and windows spill sky light indoors.
- **Tradeoff.** No per-pixel sky rays or sky shadow maps: a diffuse sky has no
  sharp shadows, and a baked visibility term costs one texture tap per pixel.
  The price is about 60–90% more bounce bake time (E1M1 debug build: 1.9 s →
  3.5 s), no sky occlusion by moving brushes or actors, and one color per map
  (no brighter sun direction in the sky texture).

## Classic and rerelease data

Sky: id's qbsp sealed sky brushes as solid, so original maps have no sky leaves
(shareware E1M1: 0 of 1,149); ericw-tools keeps them (rerelease E1M1: 26). Bake
rays end at the front of a sky face or in a sky leaf, so both kinds of map get
sky light and sun visibility. Before this, original maps received no sky light.

The engine accepts the 1.01 release's `id1/pak0.pak` (directory CRC 62751) as
unmodified, alongside 1.06 (32981). Otherwise the original check treats 1.01
shareware data as a modified game and refuses to start.

`timedemo demo1`, Luminous, debug build, uncapped: shareware 119 fps with 24 map
lights → 155 fps with 120 (per-face light tests now reject on the precomputed
reach bit before any geometry); rerelease 119 → 104 fps, which now lights with
its 206 surface lights and up to 120 map lights. Classic shareware: 238 fps.

The same binary runs on either installation and follows the keys present in
the local map. Original id maps carry only what id's LIGHT read (`light`,
`style`, `angle`, `target`), so nothing below applies to them. The rerelease
maps were compiled with ericw-tools, and their entity keys describe lighting
the baked lightmap contains:

| Key (local rerelease data) | Maps | Luminous |
| --- | --- | --- |
| `delay`, `wait`, `mangle`, `_softangle`, `_color` | most | Falloff formulas, cones and color (above) |
| `_sun 1` light (start 200, E1M1 75, E4M1 75) | 3 | Directional sun (below) |
| worldspawn `_sunlight "1.0 1.0 1.0"` | 11 | Parsed as intensity 1: too faint to add a brightness unit, so left out, as it adds nothing to the lightmap |
| `_surface` template | 30, 206 templates | Copies on every matching face (below) |
| `_surface_spotlight` | start (2) | Copies aim along the face normal |
| `_dirt` (ambient occlusion), `_bounce` | all | Not reproduced; Luminous has its own bounce |
| `_project_texture` | 1 | Not reproduced |

- **Sun.** The strongest `_sun` light (aimed by its target or `mangle`,
  otherwise straight down) or worldspawn `_sunlight` (`_sun_mangle`, default
  straight down; `_sunlight_color`) becomes one directional light. Its strength
  is the intensity times the map's range scale (0.5), with the softened
  incidence term, as ericw-tools writes it. At map load each bounce sample
  facing the sun casts 16 rays spread over `_sunlight_penumbra` degrees; the
  fraction reaching sky is its visibility, kept in bounce layer 1 alpha at the
  lightmap's 16-unit resolution. Each probe keeps its own sun visibility (probe
  layer 1 alpha) for actors and liquids, and sunlit samples reflect into the
  style 0 bounce. E1M1's sun is 38 brightness along `0.34 −0.42 −0.84`; on a
  rooftop with no other light (face 883) the rerelease lightmap holds 35, and
  38 × (0.5 + 0.5 × 0.84) = 35. Moving actors and doors do not shadow the sun.
  Bake time on E1M1 rises about 8% (3.46 s → 3.75 s, debug build).
- **Surface lights.** A `_surface` template is copied onto each world face with
  that texture (case-insensitive; sky excluded). Faces are split on axis planes
  at multiples of 128 units, as ericw-tools' default `-surflight_subdivide` and
  GLQuake's warp subdivision do, and a copy sits `_surface_offset` (default 2)
  units out from each piece's centroid. Copies inside solid are skipped, as are
  copies on a liquid face's underside. The template itself does not emit: maps
  keep templates together out of sight (E3M6's sit together at `-1088 -240`).
  E1M1's 9 templates on 163 faces become 206 lights (252 entities → 448
  sources). They share the 120 runtime map-light slots with everything else.

E1M1 profile while firing rockets (`lq_profile`, 7 s after warm-up, M1 Max,
640×480):

| Mode | CPU per frame | Scene preparation |
| --- | ---: | ---: |
| Classic | 3.6 ms | 0.5 ms |
| Luminous | 4.2 ms | 0.9 ms |

Luminous figures include the dynamic probes. Interactions are built only for
visible faces, using cached bounds. GPU time is not measured.

The six-axis GPU test also drives a moving actor box. Exactly 6 faces are
redrawn for a new cube, 1 while the box stays, 1 when it moves (and the shadow
follows), 1 to erase it after removal, then none. In demo1, an explosion beside
a monster throws the monster's silhouette across the wall and ceiling behind it.
With monsters moving near lights, demo1 holds Classic's frame rate. Average
presentation/submission CPU is 3.6 ms (9 ms at worst), covering shadow redraws
and dynamic probes.

Current limits:

- Actor silhouettes cast from back faces chosen per vertex normal, which can thin shadows at grazing angles.
  Brush-model faces are lit and gather bounce at their compiled positions.
- Sky light is diffuse only, one color per map, baked visibility (no
  occlusion by moving objects). Only maps that define an ericw-tools sun get
  one; one sun per map, also baked, so moving objects do not shadow it.
- Face bounce is static. Weapon light reaches surfaces only through the probe
  grid, which is coarser than the bounce atlas. Probes do not test visibility,
  so light can leak through thin walls between probe cells.
- 120 shadowed map lights per view; lights can still pop as the set changes,
  and a light can change shadow resolution when its rank leaves its band. A map
  load or teleport draws every new cube in one frame. Bounce and probes take
  1–2 s of map load and are not cached yet.
- PVS reach may exclude surfaces lit through water if the map was vised with
  opaque water.
- Only Metal has run; the Vulkan path is untested. Shadow memory is 174 MB
  for 128 cubes; the bounce atlas adds up to
  four layers the size of the used lightmap rows.

Quake's stored data is a baked lightmap rather than a shadow map. Luminous keeps
that image as evidence of the original composition: it can guide comparisons,
light reconstruction and deliberate darkness, but it is not sampled or multiplied
into Luminous direct illumination. Runtime lights and geometry must produce the
lighting. Where the surviving entity descriptions cannot reproduce an authored
choice, an inspectable per-map override or indirect-light control is preferable to
quietly restoring baked direct light.

If those controls still cannot recreate an important area, Luminous may use a
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
hard edge and a 32-unit emitter a graded penumbra. Penumbra checks average several
locations because the world-anchored 16-tap pattern varies at individual pixels.
A separate sealed-corner fixture uses a grazing eight-unit emitter behind a
thick wall, mirrored vertically: blocked receivers stay dark above and below,
while disabling shadows illuminates the same samples. Both native suites (normal and
ASan/UBSan) pass all 21 tests, and an E1M3 Luminous run is sanitizer-clean.
Other-platform runtime validation remains pending.

## Next implementation sequence

1. Probe visibility weighting against wall leaks, and a cache for bounce/probes
   so map load does not repeat the gather.
3. Implement the staged [light reconstruction](LIGHT_RECONSTRUCTION.md): retain
   style-layer evidence, compare the runtime prediction, estimate only
   identifiable parameters, and report residual/confidence data.
4. Refine the experimental in-game Luminous path against Classic GPU.
   Normal/specular inputs and deliberate atmosphere adjustments follow.
   A map-local static composition patch is a last resort, used only
   where falloff, occlusion and authored controls cannot recover a scene.

No normal-map, specular or fog claim follows from this checkpoint. Luminous asset
creation remains in the separate data project.
