# Community material packs — October 17, 2026 (updated October 21)

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

**Offset mapping is the only use of height** (`OffsetMapping` in
`shader_glsl.h:872–917`; the alpha is read nowhere else). Every texture is
marked for it (`mod_noshader_default_offsetmapping 1`), and the global switch
decides. Two forms:
- **Offset mapping** (`r_glsl_offsetmapping 1`): 2 steps
  (`r_glsl_offsetmapping_steps`). Each moves the texture coordinate along the
  view direction by `−height × offset + (1 − bias) × offset`, with
  `offset = normalize(eye).xy × scale / steps`.
- **Relief mapping** (`r_glsl_offsetmapping_reliefmapping 1`): 10 linear steps
  (`_reliefmapping_steps`) until the view ray is below the height field, then
  5 halving steps (`_refinesteps`) to find where it entered. This is parallax
  occlusion mapping.
- **Bias:** 0 for textures without a shader, so alpha 1 is the surface and
  everything lower is carved in. Rygel's `tech08_1` peaks at 159 of 255, so
  even its highest parts sit 38% of the depth below the face. A shader's
  `dpoffsetmapping` keyword sets the mode, scale and a bias to treat a given
  height as the surface.
- **Distance:** `r_glsl_offsetmapping_lod` (off by default) cuts steps with
  distance from 32 units (`_lod_distance`).
- Colour, normal, gloss and glow are all sampled at the moved coordinate. The
  surface's geometry, depth and silhouette do not change.

**Normal maps shade** DarkPlaces' realtime lights (`r_shadow_realtime_dlight 1`:
explosions, rockets; `r_shadow_realtime_world 0`: the map's lights) with
specular from `_gloss`, and baked light only where a `.dlit` deluxemap gives
its direction (`r_glsl_deluxemapping 1`). Without one, a lightmapped surface's
normal map shows only under realtime lights.

**Luminous decision, October 20:** parallax (relief mapping, matched to
DarkPlaces' scale and bias) and relief shadows, marched toward each light from
the point the view ray found.

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
  needs its own decision. Decided October 23: drawn only, in Luminous (see
  [Models](#models-october-23)). 61 of the 65 are MD3s under `.mdl` and `.bsp`
  names (all the item boxes, weapons, keys and powerups); armor, grenade,
  lavaball and missile are MDLs.

**Installed for development** (October 20): extracted to
`quake-complete/Quake/rygel_ultra/`, a game folder of its own (git ignores
`quake-complete/`), with DarkPlaces' layout inside. `-game rygel_ultra` turns it
on and the original `id1` stays untouched. The engine already reads loose
models from a game folder, so that switch also loads the pack's replacement
`progs/*.mdl` and item-box BSPs today, before any texture support. Base names:
730 (`tech08_1.jpg`, `tech08_1_norm.tga`, `tech08_1_gloss.jpg`); `*` becomes `#`
(`#04awater1.jpg`). In `tech08_1_norm.tga` the height (alpha) spans 0–159 of
255: plates lowest, riveted strips above them, the olive panel highest.

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
- The game's search path reads no PK3s. Texture packs are read from PK3s
  by their own reader (below), outside it.
- Community skyboxes are planned under agenda item 1.

## Implemented: first stage (October 20)

High-res textures and bump maps, on Options → Luminous Options:
- **Texture pack** (`lq_texture_pack`, saved, default `auto`): a pack is
  anything whose `textures/` holds DarkPlaces material maps (a `*_norm`
  image), so a mod's own textures are not taken for one. Players get packs
  as one `.pk3` download, which DarkPlaces players drop into `id1/`; that
  works here as it is, with nothing unpacked or overwritten
  (`LQ_TexturePacks`, `platform/render_bridge.c`), in this order:
  1. a `.pk3` **beside the app** (the folder holding `LuminousQuake.app`, or
     the executable's folder elsewhere): one pack for every install;
  2. a `.pk3` in the install's **`id1/`**, then the running mod's folder;
  3. a **folder of the install** holding the pack unpacked (`rygel_ultra/`).
  Each place is sorted by name, and an earlier place wins a name. ~/Library
  is not used: macOS hides it. `auto` takes the first pack, `none` none; an
  empty value is `auto` too (the setting's first build saved `""`).
  The menu cycles auto, none and each pack found, shows where the chosen one
  is, and, when none is found, says where to put a `.pk3`. Pack files are
  read directly, never through the game's search path, so a pack's models,
  maps and menu graphics replace nothing, and no `-game` is needed. Choosing
  another pack rebuilds the map's scene; the scene cache keys on the pack.
- **PK3 reading (October 21)** (`renderer/pack_archive.c`): the ZIP central
  directory is read once per load; entries are found by name ignoring case,
  read raw on the loading thread, then inflated (stb_image's zlib) and
  CRC-checked on the job threads with the decoding. Stored and deflated
  entries only; ZIP64, encrypted entries and entries over 256 MB are skipped.
  Rygel's ultra `.pk3` (2.84 GB, 2,815 entries) gives E1M1 the same 81
  textures as its unpacked folder, from byte-identical files; the viewer
  run took 8.6 s against 8.1 s from the folder.
- **High-res textures** (`lq_pack_textures`) and **Bump maps**
  (`lq_pack_bumps`), both saved and on by default, switch at once, so a view
  can be compared both ways.

The first version found packs only through the search path, so it needed
`-game rygel_ultra`. A Dock launch found nothing and the switches did nothing,
which is how it first failed in play; the second needed the pack chosen in
the menu first, and the third read the empty value its predecessor had saved
as `none`. The console now says which pack a map used, or why none.

- **Finding the files** (`LQ_QuakeSceneLoadPack`, `renderer/quake_scene.c`).
  For each world texture: `textures/<map>/<name>`, then `textures/<name>`, as
  `.tga`, `.png`, then `.jpg`, with `*` written `#` and a lower-case retry.
  Suffixes `_norm`, and `_glow` then `_luma` for glow; a glow counts only with
  a replacement colour image, as in DarkPlaces. Files come through the game's
  search path (`COM_LoadMallocFileQuiet`, which searches as `COM_FindFile` does
  without printing every miss), then from the chosen pack, a `.pk3` or a
  folder.
- **Decoding:** stb_image 2.29 (`third_party/stb/stb_image.h`, public domain or
  MIT; copied from dhewm3's patched copy), JPG, PNG and TGA only, at most
  4096 per side, in parallel on the job threads. Images stay with the scene,
  so a cached map keeps them.
- **On the GPU:** RGBA8 with a full mip chain, sampled with trilinear
  filtering and the anisotropy setting. The levels are built on the CPU at
  upload (`LQ_PackHalve`): colour averaged in linear light (the 2.2 power
  `world.frag` decodes with) and normal maps renormalized. The first version
  let the GPU build them from encoded values, which darkened distant texels:
  at 1/16 size the median E1M1 pack texture lost 15%. Colour carries its glow in
  alpha (the glow image's brightest channel); the normal map keeps height in
  alpha, for parallax next.
- **Colour:** in Luminous only, on solid world surfaces and inline brush
  models. The pack's colour is decoded with the palette's 2.2 power and lit
  the same way. The game also changes its palette before we see it: Quake's
  `gamma` (the Options brightness slider, `gammatable`) and the screen tints
  (damage, pickups, powerups, liquids; `V_UpdatePalette` mixes them into the
  palette in software Quake). Pack colours take the tints as the palette does
  (`v_palette_shift`, `lights.palette_shift`). Until October 21 they took
  neither: with `gamma 0.7` in a player's config, every unreplaced texture
  showed pale and washed out beside the pack's (`tech08_4`, which Rygel's
  pack lacks, on E1M1).
- **Quake's gamma in Luminous (October 21).** Software Quake's gamma bends the
  final on-screen colour, so it lifts dark areas most. Luminous used to apply
  it only to texture colour, before lighting, where it did about half as much
  in dim places. Now `world.frag` undoes it when decoding palette colours
  (`paletteDecode`, exponent 2.2 / gamma), pack colours never take it, and
  `tonemap.frag` applies it once to the finished frame (light^gamma up to SDR
  white; HDR highlights above white keep their level). Classic GPU is
  unchanged: it shows the gamma'd palette directly. At `gamma 0.7`, E1M3's
  view at `--camera "1330 640 690 5 180"`: the dim floor rose from 21.2 to
  27.1 mean luma and a shaded pillar from 12.4 to 17.9; the lit back wall
  barely moved (54.9 to 55.4). Glow emits as fullbrights do: colour × glow × `lq_emission`.
  This approximates DarkPlaces, which adds the glow image itself.
- **Bump maps:** the normal map turns into world space with a frame built
  from screen derivatives of position and texture coordinate, so no vertex
  data changed. Green points up the image (against increasing t). The frame
  is divided by its determinant, which is negative under Metal's
  downward screen y; the first version left it out and, by that reasoning,
  lit every bump from the opposite side. Rivets now light on the side
  facing the lamp. The bumped
  normal sets each light's incidence, LIGHT's softened `(1 − s) + s·n·l`
  included, and the sun's. The face's own normal still decides which side a
  light is on, and drives shadow filtering, bounce, sky and probes.
- **Parallax (October 21):** DarkPlaces' relief mapping. The view ray
  marches down through the normal map's alpha in
  8 even steps facing the surface to 24 at grazing views, then interpolates
  between the last step above and the first below the surface (DarkPlaces
  halves 5 more times instead). Depth is **Parallax depth**
  (`lq_pack_parallax`, saved, default 0.04 as DarkPlaces' scale) times the
  texture's size on the face, the geometric mean of its two axes, so
  stretched textures keep round bumps. The shift is held to a 4:1 slope at
  grazing views. **Each texture's mean height lies at the face**
  (`lq_material.pack_height`, measured at load), as DarkPlaces' `match` bias
  puts a chosen height there: higher parts stand out and lower ones sink, and
  the ray starts above the face at the top of the range. DarkPlaces' default
  bias puts height 1 at the face instead, and many of Rygel's height maps sit
  mostly low (`wizmet1_2` averages 0.03, `wgrnd1_5` 0.05, `door04_2` peaks at
  0.36), so the first version carved whole textures to full depth and slid
  them across their faces as the view moved. Everything the surface draws (colour, normal map, the
  palette texture when the pack's colour is off) is read at the shifted
  point; mip levels and the bump frame use the unshifted coordinate, so steps
  in the shift don't tear them. Lightmaps are not shifted. On E1M1's south
  corridor ceiling rivets now stand out and stretch toward the camera, as
  raised bumps do.
- **Relief shadows (October 24)** <a id="relief-shadows-october-24"></a>:
  Luminous Options → **Relief shadows** (`lq_pack_relief_shadows`, saved,
  default on; needs Parallax depth above 0). From the point the view ray
  found, the shader marches up through the normal map's alpha toward the
  pixel's strongest light (the light the shadow-filtering pass already
  picks), at parallax's depth and 4:1 slope limit, 6 steps facing the light
  to 16 at grazing. Any texel above the ray blocks; the result is soft from
  the closest miss (clearance under a third of the climb grades into
  shadow). It multiplies that light's cube shadow, so shadow cubes still cast
  everything between surfaces. It fades out between mip levels 2 and 4, where
  bumps are under a pixel; the level is worked out from screen derivatives,
  since the Metal 2.1 target has no `textureQueryLod`. Not yet: the sun as
  the strongest light, more than one light, and a separate one-channel height
  texture for speed. E1M3's start with the auto-chosen pack: the frame darkens
  about 5% and the mortar under the torch deepens; the direction still
  needs judging by eye with a moving light.
- **Classic GPU draws packs (October 24).** The user decided that Classic
  GPU should have texture packs as GLQuake-era engines did, limited to what
  that rendering supports. The pack loads for both GPU renderers. Classic
  draws its colour lit by the colormap's scale at each light grade
  ((64 − grade) / 32: row 32 is 1×, row 0 2×), through the screen tints and
  Quake's gamma as the palette is, with glow added unlit; and the pack's
  models, skins and skyboxes. Bump maps, parallax, relief shadows and gloss
  stay Luminous's. The pack settings that both renderers use moved to Video
  Options (texture pack, high-res textures, pack brightness, pack models,
  pack model skins); Luminous Options keeps Luminous's own. E1M3's start in
  Classic, pack on against off: 88% of pixels differ, 33% by more than 16
  levels.
- **Gloss specular (October 24)** (Luminous Options → **Gloss**,
  `lq_pack_gloss`, saved, default 1, 0–2): `_gloss` maps, loaded for world
  textures and pack model skins when a pack colour image exists. The shader
  has all 16 texture bindings in use, so the gloss rides in the normal map:
  red and green hold the normal's x and y, z is rebuilt (it points out of
  the face), blue holds the gloss's brightest channel, alpha the height
  (`LQ_PackGlossIntoNormal`; a gloss of another size is resampled; gloss
  without `_norm` gets a flat normal). Mips average x and y as vectors and
  blue and alpha as stored. Every runtime light adds DarkPlaces'
  Blinn-Phong, exponent 32, against the bumped normal, weighted by the
  light's arrival (falloff, incidence, shadow) and the gloss; the sum
  becomes light as illumination does and adds to the pack colour. Per-pixel
  exponent (gloss alpha) and gloss colour are not used: Rygel's 735 gloss
  maps are JPGs with no alpha, and grey. It needs Bump maps on, and fades
  out with the bumps by footprint. E1M3 with Rygel: 46 of 49 replaced
  textures have gloss; Gloss 1 against 0 changes 2.4% of pixels by more
  than 16 levels, 2 against 0 4.4%.
- **Liquids, fences and `_bump` (October 25).** Rygel has 13 liquids
  (`#04water1`: colour, glow, gloss, normal); the loader looks a liquid's `*`
  up as `#`, as DarkPlaces writes it. Both GPU renderers draw the pack colour
  on the waves (sampled at the warped coordinate), translucent liquids lit,
  opaque ones unlit as their palette colours are, glow emitting. Bumps,
  parallax and gloss are not applied to liquids: the waves stand in for them.
  E1M1's `*water0` shows Rygel's ripples in Classic GPU and Luminous.
  - Fence textures (`{name`, FitzQuake's: palette index 255 see-through) were
    drawn solid by the GPU renderers. World faces now carry the masked flag
    (the scene's flag check and the lightmap, bounce and probe code that took
    "no flags" to mean a lit face were widened to match), the shader discards
    by the original texture's index 255, and a pack colour shows where the
    original is solid. In Luminous fences still cast their shadow cubes as
    solid quads. The rerelease's `maps/test/test_shadow` shows the grid
    see-through; no pack here has fence textures.
  - Maps in folders under `maps/` (`test/test_shadow`) were refused by the
    GPU scene builder and fell back to Software; they now load.
  - `_bump` images, height only, become normal maps where a texture has no
    `_norm`, as DarkPlaces does (`LQ_PackNormalFromBump`: its
    `Image_HeightmapToNormalmap` slopes at `r_shadow_bumpscale_bumpmap` 4,
    the height kept for parallax). In Rygel that is 6 model skins: the gibs
    and lightning bolts. Not checked on screen.
- **Texture size and compression (October 25).** Video Options →
  **Hi-res textures** (`lq_pack_textures`, saved) is now off, full, 1/2,
  1/4 or dynamic (new profiles start on dynamic; a saved "on" reads as full).
  The user's design: full uncompressed, the smaller sizes compressed, and
  dynamic choosing per map. Changing it rebuilds the map.
  - **Measured:** Rygel's pack decodes to about 1,000 times the size of the
    8-bit originals: E1M1 0.6 MB against 557 MB (colour and normal, RGBA8
    with mips); all id1 maps 4.9 MB against 4.7 GB. At E1M3, the scene's
    pack copy: full 258 MB, 1/2 with BC7 21 MB, 1/4 6 MB.
  - **Sizes** come from the loader's own mip levels (`LQ_PackHalve`), after
    the brightness, height and gloss statistics are taken at full size. Off
    still loads the pack (at the dynamic size) so Luminous keeps its bumps.
  - **BC7** (`renderer/bc7.c`, our own encoder): mode 6 only, one line
    through RGBA per 4x4 block (principal-axis fit, the four shared-bit
    choices, one least-squares pass), 16 bytes a block, a quarter of RGBA8.
    The whole mip chain is encoded at load on the worker threads and kept
    compressed in the scene, so the scene cache's copy shrinks too; the GPU
    samples it directly (`SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM`, checked
    per device). Round trip: Rygel colour 43.8 dB; normal maps 27–32 dB on x
    and y, 39.5 dB gloss, 32–36 dB height, since one line serves four
    channels that change independently; worth a look by eye before a better
    mode is added. About 0.2 s per 1024² image on one core at -O2; the file
    is always optimized, as the Dock app is a Debug build. Only images whose
    sides are multiples of 4 are compressed; model skins follow the size,
    uncompressed.
  - **Dynamic:** full if the pack's RGBA8 mip chains fit a quarter of free
    RAM, else 1/2 if a sixteenth of that does, else 1/4. At E1M3 on this
    64 GB Mac it chose full.
- **Not in Rygel's pack, so not built:** sky texture layers (packs bring
  skyboxes instead, which load) and sprite frames (DarkPlaces'
  `progs/<sprite>.spr_<frame>`). They wait for a pack that uses them.
- **Beyond textures, undecided:** the pack's `gfx/` (135 HUD, menu and
  console images, which the 8-bit 2D path can't draw), and its `maps/*.lit`
  and `.dlit` (coloured light and its direction for lightmaps; Luminous
  computes its own light, Classic GPU would use them).
- **Before October 25:** liquids, sky, masked (`{`) textures and sprites kept their
  original textures (models since October 23: [Models](#models-october-23)); `_bump` height-only images
  (used by DarkPlaces only where `_norm` is missing; in Rygel's pack, 6
  model skins: the gibs and lightning bolts) are a later stage.
- **Detail by footprint (October 24)** (`lq_pack_detail_cull`, saved,
  default on): where a pack texel covers less than a pixel, the costly work
  stops. The pack's mip level is worked out once per pixel from screen
  derivatives (so it follows resolution and viewing angle, not a fixed
  distance). Parallax depth and relief shadows fade out together between mip
  levels 1.5 and 3 and are skipped past 3; the bumped normal fades to the
  face's between 4 and 6 and is skipped past 6. Prompted by heat and frame
  rate after relief shadows landed. At E1M3's start, on against off: close
  surfaces unchanged; 39% of pixels differ at all, 1.1% by more than 16 of
  255 levels, on the mid-distance floor and walls. Not yet measured for
  speed.
- **E1M1 with Rygel's ultra pack:** 81 of 86 textures replaced (81 colour, 79
  bump, 39 glow); about 9 s to load in the debug build, including bounce.
- **E1M3 brightness (October 21).** Matched, the pack's walls in the view
  above came within 5% of the originals (back wall 54.9 against 57.8). The
  matching dims Rygel's `rock4_1` (pack brighter than the original: 0.59 at
  full size) and lifts `wbrick1_5` (2.21, held at the 2× cap). Matching by
  mean display value or by median instead of linear mean barely changes
  `rock4_1` (0.78, 0.79 and 0.91 at 256²), so the method was kept.
- **Brightness.** The pack's images are sRGB (every JPG carries the IEC
  61966-2.1 profile; the TGAs none), and decoding them with the 2.2 power
  shows each at normal brightness exactly as drawn, as the palette textures
  are. But they are darker than Quake's: across E1M1's 81 the median pack
  image has 0.87 of its original's linear brightness, and the corridor walls
  0.66–0.73 (`tech08_1` 0.73, `twall3_1` 0.67, `tech07_2` 0.66). Maps were
  lit for the originals, so **Pack brightness** (`lq_pack_brightness`, default
  `match`) scales each pack texture's lit colour to its original's mean
  linear brightness (non-fullbright palette texels against the whole image;
  clamped to 0.5–2×); glow keeps the pack's level. `as made` shows the pack
  as drawn.
- At the south corridor (`--camera "1000 1760 -330 5 0"`), mean luma: 34.4
  with the original textures; 28.0 with the pack as first drawn (gamma-space
  mips, as made); 32.7 with linear mips and matched brightness. The bump direction looks right there (riveted strips lit on their
  upper edges from the ceiling light); a test scene with a known bump should
  still confirm it.
- The viewer loads a pack with `--texture-pack <path>`, a `.pk3` or a pack
  folder, or the name of one in the install (or from a `-game` folder); `--no-pack-textures`, `--no-bump-maps` and `--pack-as-made` turn
  the stages off, `--parallax <depth>` sets the depth (0 off), and
  `--quake-gamma <g>` reproduces a player's `gamma`.
- In the game (`+lq_texture_pack rygel_ultra`, no `-game`), E1M1 loads 81
  of 86 textures from the pack in 1.04 s.

## Models (October 23)

In Luminous, from the chosen texture pack (`platform/pack_models.c`), read at
map load for each precached model and kept across maps while the pack stays
the same:
- **Replacement meshes** (Luminous Options → **Pack models**,
  `lq_pack_models`): a pack file with the model's own name
  (`progs/armor.mdl`, `maps/b_batt0.bsp`) read from the pack only, never the
  game's folders, which hold the originals. DarkPlaces tells the format by
  its header, and so does this: MD3 (`IDP3`) or MDL (`IDPO`). An MD3
  surface's image is its shader name without extension, under `textures/`
  and then as given; a surface without one is left out (`v_light.mdl`'s
  bolt guides), and a model with none left (`eyes.mdl`) stays original. A
  replacement MDL takes `progs/<model>.mdl_<n>` skins, or its own.
- **Drawn only**: the game keeps the original model for collision, bounds,
  flame lights, Software and Classic GPU. Frames follow the entity's frame
  and blend as MDL poses do; skins follow its skin number.
- **Skins for models the pack doesn't replace** (**Pack model skins**,
  `lq_pack_skins`): `progs/<model>.mdl_<n>` on the original mesh, unless the
  entity has player colours (`_pants`/`_shirt` aren't read).
- Each pack image comes with its `_norm` (bump maps, by the Bump maps
  switch) and `_glow` or `_luma`; colour is matched to the original model's
  skin brightness by Pack brightness. Normal maps use the same screen-space
  frame as world faces, so models need no tangents. No parallax on models.
- Self-lit models (flames, bolts, lasers, lava balls) show the pack's colour
  at palette brightness, with its glow emitting.

E1M1 with Rygel's ultra pack: "Pack models: 29 replaced, 1 with pack skins,
of 40 new to this map, in 0.24 s" (debug build); the view shotgun is the
pack's MD3 with Pack models on and the original with it off.

## Luminous plan

1. **Loader.**
   - Texture packs from PK3s: done October 21 (above). Still open, for the
     game's own search path:
     PK3 and `.pk3dir` packages in each game directory, in DarkPlaces' order:
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
3. **Parallax and relief shadows** (decided October 20).
   - Parallax: relief mapping as DarkPlaces does it (linear steps, then
     halving steps), with its depth scale and bias, so packs look as authored.
   - Relief shadows: march from the point the view ray found toward the
     pixel's **strongest light only**, through the height field, with soft
     results from the closest miss. Only one light marches because the cost is
     unknown and no other engine does this; more lights may come later.
   - Step counts grow with grazing angle; the effect fades with distance and
     mip level.
   - The march reads only height, so height gets its own single-channel
     texture: a quarter of an RGBA read, compressed or not.
4. **Replacement diffuse and glow.** These bypass the palette and colormap path
   and need their own decision on colour grading.
5. **Model skins.** Done October 23 without tangents ([Models](#models-october-23));
   `_pants`/`_shirt` still open.
6. **Controls.** Each feature gets a switch on Options → **Luminous Options**
   as it lands, for A/B comparison: replacement textures, bump maps,
   parallax, relief shadows, texture compression.
7. **Texture compression, decided by measurement.** The M1 Max samples BC1,
   BC3, BC4, BC5, BC7 (and sRGB variants) and ASTC through SDL's Metal
   backend (checked October 20).
   - Memory: a 1024² RGBA8 image with mips is 5.6 MB; BC7 or BC5 is 1.4 MB;
     BC1 or BC4 0.7 MB. E1M1's 86 materials × base, normal and gloss is about
     1.4 GB uncompressed and about 0.4 GB compressed. 64 GB doesn't need
     that; 8–16 GB Macs will.
   - Speed: the relief march's many dependent reads gain most from smaller
     textures in cache. Whether frame time improves is for the performance
     round to show, with compression on and off.
   - Cost: BC1/BC4/BC5 encode in milliseconds; good BC7 takes far longer, so
     encoded results would be cached on disk, as DarkPlaces' `dds/` cache is.
     BC5 suits normals; BC1 bands on smooth gradients.

Test data (QRP, Rygel) goes in an ignored game directory, never into the
repository.
