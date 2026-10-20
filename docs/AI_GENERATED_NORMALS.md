# AI generated normals — October 20, 2026

An experiment: can an AI infer believable relief and lighting-free paint from
an original 8-bit Quake texture, then relight it from a height field? The test
texture is `tech08_1` from E1M1, a 128×128 brown wall with riveted metal plates.
The work was done by Claude in a chat session, from a description of the
texture's shape by Joseph. Nothing here is in the engine. The agenda still
[decides against](AGENDA.md#decided-against) generating relief from the
original textures. This page records what was tried and how far it got.

## Reproduce

```sh
python3 tools/ai_normals.py
```

This needs numpy, scipy and Pillow, and the installed `id1/pak0.pak` (by default
`quake-complete/Quake/id1/pak0.pak`; `--pak` changes it). It takes about 13 s
and writes to `build/ai_normals/`. The outputs are not checked in, because they
are derived from id's textures.

| File | What it is |
| --- | --- |
| `sheet.png` | All the steps side by side |
| `original.png` | The 128×128 texture from `maps/e1m1.bsp` |
| `tech08_1.png` | Inferred paint, no lighting, 1024×1024 |
| `tech08_1_norm.png` | Normal map in DarkPlaces `_norm` form: green up the image, height in alpha |
| `height.png` | Height, darkest lowest |
| `ao.png` | Ambient occlusion from the height |
| `lit_*.png` | Relit from above, top-left, left, right and below |

## The brief

Joseph gave the depth order:
- The darker olive area stands out from the lighter areas.
- On the lighter areas, the riveted strips sit above the base.
- The rivets on both surfaces stick out.
- The marks on the olive area are damage that cuts into the surface.
- The stains on the lighter areas are flat.

The next step was to infer the paint without the lighting painted into the
texture, at 1024 square. A light pass would then produce the shadows and
highlights from the height field instead.

## Method

### 1. Height

Colour alone does not separate the olive area from the plates: the olive area
has grey pixels in it and the plates have rust. So the outline is a fitted
shape. The dark edge line of the olive V was traced row by row. It is straight
(`x = 19.76 + 0.3068·y`) and mirrored about x = 64.5. The plates' rounded
bottom corners are a 22-texel fillet, fitted to points traced along the flare.
The rest is found from the pixels:

- **Strips** start every 13 rows from row 23. Each is 5 rows tall: a bright top
  edge, a rivet row, then a dark row under it.
- **Strip rivets** are a bright pixel over a dark one on a strip's third row.
  There are 197, every third column.
- **Olive rivets** are local minima of the blurred brightness, darker than 38.
  There are 17: seven pairs down the V, one where it meets and a pair below.
  Each head sits half a texel above its minimum. The dark streak under it is
  treated as shadow.
- **Damage** is measured on the olive, away from its edges and rivets. A
  texel's depth weight rises from 0 to 1 as it differs from the median of its
  5×5 block by 8 to 22 brightness levels.

Heights are in texels, with one texel taken as one world unit:

| Feature | Height |
| --- | --- |
| Plates | 0 |
| Strips | +0.8, with 0.5–1 texel slopes |
| Strip rivets | +0.5 above the strip, radius 0.8 |
| Olive area | +2.5, with a slope of about 2 texels at its edge |
| Olive rivets | +1.2 above the olive, radius 2.4 |
| Damage | down to −0.7 |

The model is built on a continuous grid, so it can be sampled at any
resolution. It is sampled at 8 samples per texel for 1024×1024.

### 2. Paint without lighting

Pixels whose colour is painted-in light or shadow are marked by where they sit
in the height model. They are replaced with paint from nearby pixels of the
same material, shown magenta on the sheet: 6,275 of 16,384 texels.

| Material | Marked as lighting | Filled from |
| --- | --- | --- |
| Plates | Two rows under each strip; rows 19–22 under the olive's top edge | The same column above and below, so the vertical streaks continue |
| Strips | Top edge, rivet row and bottom edge | Along the strip |
| Olive | Edge outline, rivets and the dark streak under each, damage specks | Smooth fill, plus grain copied from other olive texels at fixed offsets |

The fill uses normalized convolution, widening until every hole has a value.
Smoothing alone left flat smudges on the olive, so its fine grain is copied
from olive texels at a fixed list of offsets. Neighbouring holes take the same
offset, so the copies form coherent patches. Stains are kept as paint
everywhere.

Each material's paint is then upscaled 8× with Lanczos, on a 3×3 tiling so the
texture still tiles. The materials are joined on the 1024 outlines, so the
borders stay sharp although the paint is soft. Damage cuts are tinted toward a
dull brown metal (70% of the plates' median colour), as if they go through the
olive paint. That is a choice, not something the texture shows.

### 3. Light

All lighting is done in linear light. The paint is decoded from sRGB, lit, and
encoded again with a soft shoulder above 0.8.

- **Diffuse.** A flat, unshadowed surface keeps its paint colour under every
  light, and slopes facing the light go brighter.
- **Cast shadows.** Each pixel marches toward the light through the height
  field in quarter-texel steps, until the ray clears the tallest point. A
  penumbra term softens the shadow edges.
- **Ambient occlusion.** The highest horizon is found in 12 directions, out to
  7 texels. Ambient light is 22%.
- **Specular.** Blinn-Phong, viewed straight on:

  | Material | Strength | Exponent | Colour |
  | --- | --- | --- | --- |
  | Plates | 0.10 | 40 | 60% from the paint |
  | Olive paint | 0.004 | 20 | White |
  | Olive rivets | up to 0.044 | up to 40 | White |
  | Damage | +0.02 | +10 | White |

  The first olive value tried, 0.06, turned the dark olive grey.

## Results

Lit from above at about 40° — the direction the original was painted for — the
result's mean colour against the original, in 0–255 sRGB:

| Area | Original | Relit |
| --- | --- | --- |
| Olive | 66.8, 49.6, 28.5 | 70.6, 53.7, 34.0 |
| Plates | 100.2, 77.7, 62.1 | 101.5, 80.4, 66.1 |

The shadows fall where the original has them painted:
- under every strip;
- under the olive's top edge;
- along the V's edges;
- below each rivet.

Lit from the side or from below, the relief stays consistent and the original's
painted shading is gone.

The `_norm` map's green points up the image: the top edge of a strip, which
faces up, reads 224, and its bottom edge reads 17.

## Limits

- **Hand fitting.** The outline, strip spacing and rivet sizes are fitted to
  this one texture. The script is a record of the experiment, not a converter
  for other textures.
- **Detail.** 128 texels carry only so much paint detail. Upscaled to 1024,
  the stains and streaks are soft. The height field supplies most of the sharp
  detail.
- **Damage.** Lit from above, it shows more than the original's specks, and
  each mark is a round pit, not a scratch.
- **Rivets.** They are a little glossy and lighter than the original's dark
  rivets. Their painted darkness was read as shadow; it may have been their
  colour.
- **Scale.** The height range is 3.7 texels. DarkPlaces' default offset-mapping
  scale gives a 128-texel texture a 5.1-unit range ([material
  packs](MATERIAL_PACKS.md#interpreting-the-data)). In-engine use would need
  the alpha scaled to match.
- **Not tested in the engine.** Luminous does not yet read `_norm` files
  (agenda item 4).
