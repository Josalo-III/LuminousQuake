# Reconstructing Quake lighting

Luminous rendering aims to preserve the original game's choices about what a
light reveals and what it leaves in darkness, without treating the baked BSP
lightmap as a runtime direct-light texture. The original image is an authored
illusion, not ground truth for a unique set of physical lights. Several source,
falloff, occlusion and indirect-light arrangements could create a similar image.
This document uses the baked data as evidence for an estimate rather than a
second, hidden light source or a recoverable physical solution. Reconstruction
is a way to initialize the kind of direct lights Luminous will actually render;
the goal is to relight the scenes as closely to the original experience as we
can manage.

## What survives in an installed BSP

| Source | Reliable information | Does not provide |
| --- | --- | --- |
| Entity text | Light origins, one authored scalar (`light`/`_light`, default 300), style, targets, and sometimes color | Separate physical power, range, falloff curve, exact compiler invocation |
| BSP geometry | Faces, oriented planes, lightmap texel locations, texture alignment, static occluder geometry | Original editor brushes, named lighting intent, dynamic objects at compile time |
| BSP lighting lump | Final 8-bit samples, separated by up to four light styles per face | Which same-style source supplied each sample; unclamped brightness; direct versus indirect contribution |
| Original Quake `LIGHT` source | The baseline compiler algorithm and defaults | The command-line settings and map/source revision used for an individual retail BSP |
| Game/style data | Runtime light-style state and switch behavior | A physical restatement of the static bake |

The entity scalar is a compiler control, not a modern photometric quantity. In
the inspected original `LIGHT` tool it gates distance and starts the direct
contribution calculation. The stored result then depends on ray visibility,
surface angle, overlapping sources, style grouping, output range scaling and
8-bit clamping. It is therefore wrong to call that scalar an authored power or
an authored range.

## Baseline to reproduce before improving

The original tool is a valuable prior because it turns the surviving entity text
and BSP geometry into a testable prediction. With its defaults, it does roughly
the following for a light and a lightmap sample:

1. Reject a sample behind the face, beyond the source scalar, or blocked by BSP
   geometry.
2. Measure the ray distance `d` and the incoming-light angle `n·l`.
3. Add `(source_scalar - d) * (0.5 + 0.5 * n·l)` to that source's style layer.
4. Apply the compiler range scale of `0.5`, then clamp the final layer to 8-bit.

The exact original code has details for targets, face sampling and optional
extra samples. LuminousQuake will reimplement only the documented behaviour it
needs; it will not import the old tool implementation. The formula is a
reconstruction prior, not the final runtime shading model.

## Reconstruction pipeline

### 1. Preserve evidence and establish provenance

Keep each face's lightmap samples, style slots and world-space sample locations
available to an analysis tool. Record the map checksum, source PAK/loose-file
identity, observed entities, and whether the assumed original compiler defaults
were used. Analysis output belongs in a generated cache or the separate enhanced
asset project; neither installed PAKs nor the original BSP are modified.

### 2. Build a deterministic static-light prediction

For every static light/sample pair, test source-facing geometry and static BSP
visibility. Start with the original compiler defaults, then retain the predicted
per-style direct contribution before output clamping. This requires a renderer-
owned static ray-query structure; camera PVS is not a substitute for light
visibility.

The runtime renderer now occludes light with cube shadow maps and bounds each
source's reach with the compiled PVS (see [Luminous renderer](LUMINOUS_RENDERER.md)).
Neither is a sample-level ray query for reconstruction: shadow maps are view-
independent GPU images, and PVS is deliberately conservative.

### 3. Estimate only identifiable parameters

Compare prediction to the stored samples by style on texels that are neither
near black nor clipped white. Estimate map-global exposure and compiler
compatibility parameters first. Estimate per-light corrections only where
samples make a source distinguishable from its neighbours. Use a robust loss so
a few clipped, corrupt, or deliberately exceptional texels cannot dominate the
result. These estimates are starting values for the runtime lights, not proof of
the original author's settings or a mandate to copy every baked sample.

The analysis report must include residual images, sample count, confidence, and
which parameters are underdetermined. Sources that share a style and influence
the same samples often cannot be separated uniquely; retain a group correction
or leave the default rather than inventing a precise-looking individual value.

### 4. Split direct light from the residual

The estimated, visibility-tested source model provides a starting rig for
Luminous direct lighting. The remaining low-frequency residual is evidence for
ambience, bounce light, minlight, deliberate artistic cheats, or information lost
through clamping. It may inform sparse irradiance probes or deliberately authored
per-map ambient controls, but it must not be sampled as the old direct lightmap
during Luminous rendering.

When this rig cannot reproduce a scene's composition, adjust or supplement its
lights and indirect controls by eye. An original map may have been lit for a
particular view, deliberately brightened where no plausible source exists, or
left dark despite nearby sources. Such cases are expected, not reconstruction
failures to conceal with an ever more elaborate fit. Keep those authored
adjustments explicit and tied to the map so they can be reviewed and revised.

This preserves darkness as an intentional part of the scene while allowing a
moving object, a switched light, or a new shadow caster to affect direct light
properly.

### 5. Use a bounded static composition patch when necessary

If the inferred rig plus explicit authored lights and indirect controls cannot
recreate an important original composition, Luminous may use a documented,
map-local static contribution in the spirit of Tenebrae's dim lightmap base.
This is a fallback for a named problematic region, not the default Luminous
lighting model. It must be derived and stored separately from the installed BSP,
have an adjustable strength and spatial mask, and state why the direct-light rig
was insufficient.

The patch must not restore a baked direct-light layer across the map. It should
contain only the stable residual needed to retain the intended mood or gameplay
cue. It cannot stand in for a switched or moving light, cannot prevent dynamic
objects from receiving the reconstructed lights and shadows, and must remain
inspectable in an Luminous debugging view. Classic continues to render the
original lightmap unchanged.

### 6. Use the result at runtime

Runtime draws use estimated and, where needed, authored source parameters,
current styles, geometric light/surface relationships, shadow visibility, and
only approved static composition patches.
Static lights can use cached shadow data; moving lights and moving casters need
bounded updates. The renderer should select only a limited number of relevant
lights and casters for each view, while the analysis cache remains immutable
during a frame.

## Calibration and validation

Validation has two different questions. Synthetic scenes can establish that the
renderer obeys its defined behavior: a blocker casts a shadow, a switched light
changes the lit region, and a moving object responds to current illumination.
An original map cannot prove that our recovered lights are the unique or
historically correct lights. For that map, Classic is a visual and gameplay
reference for a close experience, not a pixel-exact acceptance target.

For each reference map, produce paired Classic and Luminous captures along with:

- a direct-light prediction image;
- a residual/indirect image;
- a confidence image marking clipped and underdetermined areas; and
- numeric difference by style and by focal region, used to locate mismatches
  rather than as a pass/fail score.

Review focal lights, silhouettes, passages, puzzle cues and deliberately black
spaces by hand in the original campaign. Check that gameplay-critical landmarks
remain readable, concealment and ambushes retain their effect, and switched
lights convey the same state changes. Prioritize that experienced composition
over numerical closeness to individual lightmap samples. Record intentional
departures from the bake and any per-map adjustments so the estimate remains
inspectable. Compare moving and switched lights in a synthetic scene before
enabling an experimental in-game Luminous mode.

## Initial implementation order

1. Reconstruct per-face sample positions from texture axes and face planes, and
   expose original style layers without changing Classic rendering. The first
   read-only diagnostic is available now:

   ```sh
   ./build/luminousquake-gpu -basedir "$PWD/quake-complete/Quake" \
     -map e1m3 --light-analysis | rg '^LQ_LIGHT_' > /private/tmp/e1m3-light-samples.csv
   ```

   Rows contain the face, sample-grid coordinates, unadjusted texture-grid world
   position and oriented face normal, style index and raw byte value. The
   original `LIGHT` tool could move obstructed grid points toward the face
   center before tracing; this diagnostic does not yet reproduce that step.
   The output is a derived analysis artifact; installed BSP and PAK files are
   only read.
2. Build a read-only map-analysis command that writes comparison reports and
   reference images outside installed game data.
3. Implement static BSP ray queries and reproduce the original direct-light
   prediction with its documented default parameters.
4. Add map-global estimation, residual reporting and confidence classification.
5. Define the versioned derived-light cache format and a per-map authoring
   override schema, including narrow static composition patches for cases where
   the inferred rig misses an important original scene.
6. Add shadow-caster culling and a portable point-light shadow behavior test
   using the estimated sources; only then consider exposing Luminous in the game
   menu.

## Necropolis first pass

The source comparison and provisional fit results for this map are recorded in
[The Necropolis lighting analysis](NECROPOLIS_LIGHTING_ANALYSIS.md).

The available local E1M3 (`The Necropolis`) loads as 5,176 BSP faces and 289
captured light entities. Its sample dump contains 180,417 face/style records:

| Style | Records | Mean byte value | Zero | 255 |
| --- | ---: | ---: | ---: | ---: |
| 0 | 163,832 | 56.52 | 1.4% | 0.1% |
| 32 | 7,203 | 28.99 | 13.1% | 0% |
| 33 | 3,366 | 2.63 | 45.5% | 0% |
| 34 | 6,016 | 10.56 | 32.7% | 0% |

These figures are an inventory of the currently available installation, not a
claim that it is the original retail BSP or that the samples preserve unclamped
compiler output. The low means and large zero fractions in styles 33 and 34 make
them useful cases for examining darkness and switch/style interpretation. The
small 255 fraction in style 0 suggests broad clipping is not the dominant source
of ambiguity in this map, though overlapping same-style lights remain inseparable
from these records alone.

The standalone Luminous viewer evaluates each entity's own compiler formula
(`delay`, `wait`, `light`), which is the reconstruction prior, not fitted output.
This local E1M3 was compiled with ericw-tools: worldspawn enables `_bounce` and
`_dirt`, and 49 rerelease spotlights use `light 2500` with `delay 3` (no falloff)
or `delay 2` (inverse square). The style-33 source is also inverse square, so
the first-pass linear comparison below understates that source's model mismatch.
