# Community material packs — October 17, 2026

Luminous will use community retexture packs when they are installed: normal maps,
height, gloss (specular) and glow. Original assets alone remain the baseline, and
Classic and Software never read pack data. This note records how DarkPlaces, the
engine those packs are authored for, finds and interprets the files, so Luminous
reads the same data the same way. Nothing here is implemented yet.

Reference: DarkPlaces `5d80f03` (September 1, 2026), GPL-2.0-or-later, cloned for
reading into the git-ignored `reference/darkplaces/`. File and line references
below are to that revision.

## Finding the files

**Image search** (`image.c:997–1031`, `loadimagepixelsbgra`). The requested name
has its extension stripped, and every `*` becomes `#` (`*water1` → `#water1`).
The first directory component chooses a list of formats, tried in order:

| Request starts with | Tried |
| --- | --- |
| `textures/` | `%s.tga`, `.png`, `.jpg`, `.pcx`, `.wal` |
| `gfx/` | `%s.tga`, `.png`, `.jpg`, `.pcx`, `.lmp` |
| anything else (models) | `override/%s.{tga,png,jpg}`, `textures/%s.{tga,png,jpg}`, `%s.{tga,png,jpg,pcx,lmp}` |

A JPEG has no alpha. A sibling `<name>_alpha.jpg` of the same size supplies it
from its blue channel. Files come through the normal search path, so a mod
directory overrides `id1`, and PAKs/PK3s are searched with loose files.

**World textures** (`model_brush.c:1892–1894`). For map `maps/e1m1.bsp`, try
`textures/e1m1/<name>` first, then `textures/<name>`. Sky textures still go
through the split-sky loader. Animated frames keep their own names (`+0slime`,
`+aslime`), so every frame has its own file.

**Model skins** (`model_alias.c:1253–1268`). `progs/ogre.mdl_0` is skin 0, and
`progs/ogre.mdl_0_1` is frame 1 of a grouped skin. Extra `_N` files beyond the
model's own skin count are appended as new skins.

**Q3 shader scripts.** Before the image lookup, DarkPlaces tries a
`scripts/*.shader` entry named `<map>/<name>`, then `<name>`. Some packs set
per-texture parameters there (`dpoffsetmapping`, `dpglossintensitymod`,
`dpglossexponentmod`; `model_shared.c:2126–2159`). Luminous should read just
those keywords and ignore the rest of the shader language.

## Suffixes

All of these are optional and sit beside the base image (`gl_rmain.c:2447–2534`):

| Suffix | Meaning |
| --- | --- |
| `_norm` | Tangent-space normal in RGB, height in alpha. Name from Tenebrae; the standard. |
| `_bump` | Greyscale height only; converted to `_norm` at load (`r_shadow_bumpscale_bumpmap`, default 4). Used only when `_norm` is missing. |
| `_gloss` | Specular colour in RGB; alpha scales the exponent per pixel. |
| `_glow` | Emissive; `.blend`, `_blend` and `_luma` (Tenebrae) are accepted as alternatives. |
| `_pants`, `_shirt` | Colour-mapped regions for player skins. |
| `_reflect` | Cube-map reflection mask. Out of scope. |

DarkPlaces loads the extra maps only when the replacement base image itself
exists (`R_SkinFrame_LoadExternal` returns early without it). Luminous can relax
that: a `_norm` beside an original 8-bit texture is still valid, because texture
coordinates are normalized and the two images line up at any resolution.

## Interpreting the data

**Normal encoding.** RGB is mapped with `n = rgb − 0.5` and normalized
(`shader_glsl.h:1515`). The built-in height-to-normal converter
(`image.c:1758`, `Image_HeightmapToNormalmap_BGRA`) writes
`x = left − right` and `y = below − above` in image rows. That is the OpenGL
convention: green points up the image. Tangents come from texture-coordinate
derivatives, and both are negated when their cross product opposes the face
normal (`model_shared.c:847`, `Mod_BuildBumpVectors`). For Quake BSP faces the
derivatives are the texinfo `vecs` axes, which `renderer/quake_scene.c` already
projects onto the face plane. A test scene with a known bump must confirm the
green sign before anything ships.

**Height.** It is the `_norm` alpha, with 1 at the surface and 0 at the deepest
point. DarkPlaces enables offset mapping only when that alpha is not uniformly
opaque (`TEXF_ALPHA`, `gl_rmain.c:1625`) or a shader sets a bias, which is a
good test for whether a pack has height at all. The depth scale is
`r_glsl_offsetmapping_scale` (0.04) × the shader's `offsetscale`, in normalized
texture units: 0.04 of the texture's width. For a 64-texel texture at scale 1,
the full height range is about 2.6 world units. Offset mapping is off by default
(`r_glsl_offsetmapping 0`), and it moves texture coordinates only.

**Self-shadowing is not in DarkPlaces.** It never marches the height field
toward a light. Luminous's relief shadows are therefore new behaviour, built on
DarkPlaces' depth scale and height orientation so that packs keep the depth
their authors tuned. In Luminous, a pixel's relief shadow multiplies its
direct light; shadow cubes still provide all shadows between surfaces.

**Specular** (`shader_glsl.h:30–41`, `gl_rmain.c:6840–6860`).

- Blinn-Phong: `pow(max(n·h, 0), 1 + (exponent − 1) · gloss.a) · gloss.rgb ·
  light colour · intensity`.
- `r_shadow_glossexponent` 32 and `r_shadow_glossintensity` 1; shader keywords
  scale either per texture.
- `r_shadow_gloss 1` (default) adds specular only where a `_gloss` map exists.
  `2` gives every surface a flat 0.125 sheen; Luminous will not offer that.
- `r_shadow_glossexact` switches to the true reflection vector.

**Generated bumps from base textures.** `r_shadow_bumpscale_basetexture`
defaults to 0, so DarkPlaces does not invent relief from diffuse images by
default. That matches this project's rule that painted shading is not treated
as geometry.

## Luminous plan

1. **Loader.**
   - An overlay lookup with the search order above, through the existing
     PAK/loose-file layer, game directory first.
   - TGA, PNG and JPG through a vendored stb_image; `_alpha.jpg` pairing.
   - Report found maps per texture in `--scene-info`.
   - Engine tests use generated fixtures, never pack files.
2. **World surfaces.**
   - A tangent and handedness sign in the world vertex format.
   - `_norm` and `_gloss` sampled in the Luminous branch.
   - Direct light and specular use the mapped normal; bounce and probes keep
     the geometric normal.
3. **Relief shadows.**
   - March from each shaded pixel toward a light through the height field.
   - Use the DarkPlaces depth scale, with soft results from the closest miss.
   - Bound the cost: only a pixel's strongest few lights march, step count
     grows with grazing angle, and the effect fades out with distance and mip
     level.
4. **Replacement diffuse and glow.** These bypass the palette and colormap path
   and need their own decision on colour grading.
5. **Model skins.** Per-vertex tangents for MDL, `_pants`/`_shirt`.

Test data (QRP, Rygel) goes in an ignored game directory, never into the
repository.
