# Community material packs — October 17, 2026 (updated October 20)

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
from its blue channel. Files come through the normal search path (next
section).

**Search path** (`fs.c:1340–1379`, `FS_AddGameDirectory`). Each game directory
adds, in this order:
1. every `.pak`, sorted by name;
2. every `.pk3`, `.pk3dir` folder, `.dpk` and `.dpkdir`, sorted by name;
3. the directory itself.

Each one added goes in front of the previous ones. So loose files beat every
package, a PK3 beats a PAK, and within a kind the last name alphabetically
wins. A later game directory (`-game`) beats `id1`. Names inside a PK3 match
without regard to case (`fs.c:871`). Packs are distributed as PK3s with
arbitrary names, such as `rygel-dp-texturepack-high.pk3`, dropped into `id1/`.

**Precompressed DDS** (`gl_rmain.c:2354–2444`). With `r_texture_dds_load 1`
(default 0), `dds/<name>.dds` and `dds/<name>_norm.dds` and so on are tried
before the images. It is a load-time cache that DarkPlaces can write itself
(`r_texture_dds_save`). Packs don't depend on it.

**World textures** (`model_brush.c:1892–1894`). For map `maps/e1m1.bsp`, try
`textures/e1m1/<name>` first, then `textures/<name>`. Sky textures still go
through the split-sky loader. Animated frames keep their own names (`+0slime`,
`+aslime`), so every frame has its own file.

**Model skins** (`model_alias.c:1253–1268`). `progs/ogre.mdl_0` is skin 0, and
`progs/ogre.mdl_0_1` is frame 1 of a grouped skin. Extra `_N` files beyond the
model's own skin count are appended as new skins.

Sprites follow the same pattern: `progs/s_explod.spr_0`, or `_i_j` for a
group (`model_sprite.c:216–224`).

**2D pictures** (`gl_draw.c:164–179`). HUD, menu and console pictures load
through the same lookup, as `gfx/<name>`. Replacements are usually larger than
the originals. `Image_GetStockPicSize` gives them the stock picture's size, so
the layout doesn't change.

**Skyboxes** (`r_sky.c:117–123`). For a sky named `<s>`, each face is tried as
`<s>_rt`, `<s>rt`, `env/<s>rt`, then `gfx/env/<s>rt` (likewise `bk`, `lf`,
`ft`, `up`, `dn`). The name comes from worldspawn `sky`, `skyname` or `qlsky`
(`cl_parse.c:391–427`), or from a Q3 shader's `skyparms`; `loadsky` sets it by
hand. Retail id1 maps have no sky key, so a pack's `env/sky4*.tga` needs a
shader, or the player, to name it.

**Coloured lightmaps** (`model_brush.c:2110–2170`). `maps/<map>.lit` is used
only if it starts with `QLIT`, is version 1 and is exactly 8 + 3 × the BSP's
lighting lump in size. `maps/<map>.dlit`, a deluxemap holding each lightmap
texel's average light direction, is read only after a valid `.lit`, with the
same check. Both therefore fit only the BSP they were compiled from:
rerelease and retail maps differ. DarkPlaces uses the deluxemap to bump-map
baked light (`r_glsl_deluxemapping 1`). Luminous already knows each light's
direction, so it doesn't need deluxemaps for bump mapping.

**Q3 shader scripts.** Before the image lookup, DarkPlaces tries a
`scripts/*.shader` entry named `<map>/<name>`, then `<name>`. Some packs set
per-texture parameters there (`dpoffsetmapping`, `dpglossintensitymod`,
`dpglossexponentmod`; `model_shared.c:2126–2159`). Luminous should read just
those keywords and ignore the rest of the shader language. `skyparms`
(`model_shared.c:2052`) is the other one that matters, for skyboxes.

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

## What packs ship

Checked October 20 against Rygel's DarkPlaces texture pack, "ultra"
(`rygel-dp-texturepack-ultra.pk3`, 2.84 GB, 3.70 GB unpacked, 2,794 files; its
readme dates it to DarkPlaces of July 2007). The "high" pack is a downscaled
version of the same thing.

| Folder | Files | What |
| --- | ---: | --- |
| `textures/` | 2,397 | World textures, 14 of them per-map (`textures/e4m3/window03.jpg`) |
| `gfx/` | 135 | HUD, menu and console art (PNG and TGA) |
| `maps/` | 119 | `.lit` (38), `.dlit` (38), `.rtlights` (32), item-box BSPs (11) |
| `progs/` | 87 | Replacement models (54 `.mdl`) and model skins (`armor.mdl_0_norm.tga`) |
| `cubemaps/`, `env/` | 54 | Light cubemaps; `sky1` and `sky4` skyboxes |

- **Formats:** 1,504 JPG (base, gloss, luma), 981 TGA (normals), 133 PNG.
- **Extra maps:** 729 `_norm` with height in alpha, 735 `_gloss`, 147 `_luma`,
  13 `_glow` and 67 `_bump`.
- **No shader scripts**, and every name is lower case.
- **Skyboxes** are chosen by hand: the readme has the player type `loadsky sky1`
  or `loadsky sky4`.
- **Depth:** the readme recommends `r_glsl_offsetmapping_scale 0.05`, a little
  above DarkPlaces' default of 0.04.
- **Models:** the replacement `.mdl` files are partly for DarkPlaces' own mod
  (`v_dprock.mdl`), and the readme's install steps edit that mod's folders.
  Loading a pack's `progs/*.mdl` changes models, not just their looks, so that
  needs its own decision.

Every TGA seen in the first files read is uncompressed (type 2 at 24 or 32
bits, or type 3 greyscale). The pack uses `_luma`, Tenebrae's name, for most
glow maps; DarkPlaces tries it last (`gl_rmain.c:2482`).

KGB|romi's `romirtlights_id1.pk3` is `.rtlights` and cubemaps only. Per-map
light files were [decided against](AGENDA.md#decided-against).

So reading this pack needs PK3, TGA, JPG, PNG and the `textures/<map>/`
lookup. RLE TGA and `_alpha` JPGs are in the DarkPlaces loader and other packs
may use them.

## This engine today

- `COM_AddGameDirectory` (`WinQuake/common.c:1765`) adds `pak0.pak`,
  `pak1.pak`… only, and puts the loose directory *below* them, as 1996 Quake
  did. DarkPlaces puts it above.
- No PK3 support and no image decoders. zlib is already linked, and
  `platform/localization.c` has a bounded ZIP reader for one entry of the
  rerelease's `QuakeEX.kpf`.
- Community skyboxes are planned under agenda item 1.

## Luminous plan

1. **Loader.**
   - PK3 and `.pk3dir` packages in each game directory, in DarkPlaces' order:
     loose files over PK3s over PAKs, last name wins, case-insensitive
     names. A ZIP central-directory reader with deflate (zlib), bounded entry
     sizes and CRC checks. Whether PAK loading also takes DarkPlaces' order
     (loose files over PAKs) is a separate decision: it changes which files
     original installs load.
   - An overlay lookup with the search order above, game directory first.
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
