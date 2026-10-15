# The Necropolis lighting analysis

## Scope and input

This is a first quantitative pass over `maps/e1m3.bsp` from the local
`quake-complete/Quake/id1/pak0.pak` installation. The extracted BSP is 1,253,708
bytes with SHA-256
`c86a67d50d0711fce48279af372a56576634fe71926d9f178195c813d52b3b4e`.
Its exact retail/rerelease provenance is unknown. The entity text contains
rerelease-style `_color` keys, so it is not safe to label this an untouched 1996
lighting build.

The read-only `--light-analysis` output contains 180,417 face/style sample rows
from 5,176 faces. There are 289 retained light entities, of which three use
animated styles. Each nonzero style used by the BSP has exactly one source entity:

| Style | Entity | Origin | Scalar | Entity relationships |
| --- | ---: | --- | ---: | --- |
| 32 | 43 | `(-628, -1252, 380)` | default 300 | `spawnflags=1`, `targetname=t13` |
| 33 | 52 | `(-760, -1240, 104)` | 600 | targets `t18`; `t18` is an `info_null` at `(-818, -1240, 104)` |
| 34 | 115 | `(64, -160, -152)` | 250 | `spawnflags=1`, `targetname=t76` |

This makes the three stored style channels unusually useful: each isolates one
source's baked contribution instead of mixing several same-style lights. Styles
32 and 34 are tied to trigger/door target names and begin switched off. Style 33
is a targeted source aimed 58 units along negative X.

## Sample signal

| Style | Samples | Mean byte | Samples ≥ 8 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| 0 | 163,832 | 56.52 | 147,619 | 255 |
| 32 | 7,203 | 28.99 | 4,347 | 112 |
| 33 | 3,366 | 2.63 | 70 | 238 |
| 34 | 6,016 | 10.56 | 2,005 | 93 |

Style 33 is sparse overall, but its strongest samples cluster directly in front
of the source: the brightest points include `(-831, -1232, 112)` and adjacent
samples, with value 238. This is strong evidence that the channel records the
targeted source's narrow bright region plus a much dimmer fringe. Zero values are
common (45.5% for style 33); 255 clipping is absent from that channel.

## Compiler-prior comparison

For each sample, the prototype computes source distance, rejects samples behind
the oriented face, applies the original softened face term
`0.5 + 0.5 * dot(incoming, normal)`, then predicts
`max(0, scalar - distance) * face_term * 0.5`, using the original tool's default
range scale. For style 33 it also applies the original targeted-light default
20° half-angle. Predictions are clamped to the stored byte range.

This first comparison deliberately omits BSP ray visibility. It uses a best-fit
single multiplier through zero only to measure agreement; that multiplier is
not an inferred physical power.

| Style | Sample correlation | Default-model RMSE | Best global multiplier | RMSE after multiplier |
| --- | ---: | ---: | ---: | ---: |
| 32 | 0.965 | 11.83 | 1.253 | 8.38 |
| 33 | 0.914 | 8.17 | 0.746 | 6.51 |
| 34 | 0.854 | 14.85 | 0.602 | 8.88 |

The high correlations support using the original compiler's distance and face
terms as a reconstruction prior. They do not show that a runtime model can omit
shadows: occluded and dark samples remain in the error, and the large sample
population makes a correlation score alone easy to overread. A grid search over
style 33's cone half-angle gives its lowest error near 22° (correlation 0.928),
close to the documented 20° default. Treat this as a compatibility clue, not a
claim that the local BSP used those exact compiler settings.

For that targeted source, all 24 records within 20° of its axis are at least 8;
the next 32 records in the 20–30° band are also at least 8 and average 44.03.
The 30–40° band averages 3.26 and has no values ≥8. The fitted 22° cutoff
captures the 20–30° sample group because the samples cluster near the band edge.
Fourteen additional samples ≥8 occur outside 40°, so the first-pass residual is
not zero; ray visibility, face geometry, compiler variation, or BSP provenance
must be checked before assigning that residual to indirect light.

## What we can infer

- The style layers provide per-style, per-face observations, and in this map the
  three animated styles each have one entity source. That is enough to compare
  each source independently.
- The original scalar and compiler distance/angle terms explain much of the
  spatial shape. The fitted multiplier is a useful map/style exposure diagnostic,
  not a separate authored source power.
- Style 33's target direction and narrow bright footprint agree with the targeted
  light interpretation. Low residual bytes outside the bright footprint do not
  establish a physically meaningful spill term.
- The source and sample data do not tell us which surfaces were blocked without
  a BSP ray query. Nor do they recover unclamped values, hidden editor intent,
  original compiler flags, or the precise provenance of this BSP.
- Styles 32 and 34 have switched-off initial state and linked gameplay targets.
  Their baked layers are still valuable observations of the lit state; runtime
  reconstruction must follow live style changes.

## BSP visibility on raw sample positions

A read-only pass through the world BSP's nodes and solid leaves now checks a
segment from each of the three isolated styled sources to each reported sample
position. The reproducible diagnostic is
`python3 tools/analyze_bsp_lights.py ./build/luminousquake-gpu "$PWD/quake-complete/Quake"`.
It evaluates the raw texture-aligned grid emitted by `--light-analysis`, **not**
the final positions at which the original light compiler necessarily traced.

| Style | Raw records | Raw endpoints in solid | Bright records blocked by BSP | Of those, endpoints in solid | Bright blocked with valid endpoint |
| --- | ---: | ---: | ---: | ---: | ---: |
| 32 | 7,203 | 1,271 | 726 | 715 | 11 |
| 33 | 3,366 | 639 | 18 | 18 | 0 |
| 34 | 6,016 | 979 | 365 | 326 | 39 |

The apparently blocked bright samples are overwhelmingly grid points inside
solid space. Applying visibility to those raw points makes the style 32 and 33
prediction errors worse; that is a sample-position problem, not evidence that
the original bake ignored walls. The [original Quake `LIGHT` implementation](https://github.com/id-Software/Quake-Tools/blob/master/qutils/LIGHT/LTFACE.C)
explicitly notes that the grid can extend beyond a face and moves obstructed
sample points toward its center before tracing.

The BSP contains 4,318 static-world faces and 858 brush-submodel faces. Of the
raw style records above, 1,740 (style 32), 824 (style 33), and 337 (style 34)
belong to submodels; those require separate treatment of model origin and pose.
Even among static-world faces with nonsolid raw endpoints, 4, 0, and 39 bright
records respectively are blocked by this simple BSP segment test. These small
remainders warrant inspection after sample relocation; they do not establish
indirect light or an artistic cheat by themselves. The result remains an estimate
of the local BSP's lighting, whose provenance is still uncertain.

For **static-world faces only**, the diagnostic also models `LIGHT`'s six
move-toward-center probes on an ordinary 16-unit grid. This reduces raw endpoints
in solid space from 1,000 to 25 (style 32), 498 to 13 (style 33), and 934 to 79
(style 34). The source-to-sample segment still blocks 6, 0, and 69 bright records
respectively after relocation. The simple compiler-prior prediction and this
visibility check have the following root-mean-square byte differences from the
stored samples, **without fitting a new scale**:

| Style | Static-world samples | Raw-grid prediction | Relocated prediction, no visibility | Relocated prediction with visibility |
| --- | ---: | ---: | ---: | ---: |
| 32 | 5,463 | 11.90 | 11.81 | 11.47 |
| 33 | 2,542 | 7.36 | 8.86 | 8.86 |
| 34 | 5,679 | 14.81 | 14.86 | 13.12 |

Visibility helps styles 32 and 34 under this model. Style 33 becomes less close
after relocation; its strongest narrow cone and the many brush-submodel samples
make it especially sensitive to the actual compiler sampling mode. The local BSP
may have used extra filtered samples or other settings that these ordinary-grid
probes do not recover. These numbers locate uncertainty; they are not a score for
the desired Enhanced appearance.

The 69 remaining blocked bright style-34 records are localized rather than
spread evenly: 30 are on face 2484, 11 on face 867, and 6 on face 2492. Faces
2484 and 2492 lie on the `x=0` plane near the source, so this is a specific wall
region to inspect in Classic and the Enhanced viewer. It is too early to label
that region an artistic exception; a different sample-filter setting or a
boundary trace could change this classification.

## Next analysis step

Determine whether the local BSP's lighting was compiled with extra filtered
samples; the visible byte array does not declare that choice. Inspect the
remaining blocked bright records by face and region, then account for brush
submodel origins and pose before interpreting their style values. Compare
residual images rather than relying on a map-wide score. If stable low-frequency
residuals remain, test whether they suggest a useful ambient/probe term, while
keeping direct-light choices open to deliberate per-map authoring. Do not infer
unique per-light powers from this map: the compiler scalar still couples cutoff
and contribution, and unknown compiler settings remain confounders.
