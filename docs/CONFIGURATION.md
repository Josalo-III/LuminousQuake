# Configuration: where settings live and how they load

Quake players have shared `.cfg` files for thirty years. This page says where
LuminousQuake keeps them, the order it reads them in, and which classic
settings no longer apply or now mean something different.

## Three editions, three profiles

LuminousQuake recognizes which release of Quake it is running and keeps a
separate profile for each:

| Edition | How it is recognized |
| --- | --- |
| `shareware` | No `gfx/pop.lmp` (episode 1 only) |
| `retail` | The 1996 registered packs (`PAK0.PAK` + `PAK1.PAK`), any vendor (CD, GOG, Steam's original files) |
| `rerelease` | The 2021 Nightdive/id release (its `pak0.pak` adds files such as `gfx/base.wad`) |

The console prints the edition at startup:
`LuminousQuake edition: retail; user data: …`.

Each profile holds that edition's settings, saves and demos, one folder per
game directory:

```
<profile root>/
  shareware/id1/
  retail/id1/
  rerelease/id1/
  rerelease/ad/        a mod (-game ad) on the rerelease
```

| System | Profile root |
| --- | --- |
| macOS | `~/Library/Application Support/LuminousQuake` |
| Windows | `%LOCALAPPDATA%\LuminousQuake` |
| Linux | `$XDG_DATA_HOME/LuminousQuake`, else `~/.local/share/LuminousQuake` |

`-userdir PATH` replaces the root; the edition and game folders are still
added beneath it.

A mod's profile (`rerelease/ad`) holds that mod's own `config.cfg`, saves and
demos. It does not read the `id1` profile's `config.cfg`.

**Older builds** kept one shared profile at `<root>/id1` (and `<root>/<mod>`)
for every edition. The first rerelease run after October 18, 2026 moves those
folders into `rerelease/`, since that was the edition most used. Retail and
shareware start from their own defaults (see below).

**The game installation is never written.** Saves, demos, screenshots and
configs all go to the profile, whatever folder the game data is in.

## What runs at startup, in order

1. **LuminousQuake's built-in key defaults**: WASD, mouse attack, Space jump,
   Shift run, `` ` `` console, F11 fullscreen, 1–8 weapons
   (`platform/in_sdl.c`).
2. **`quake.rc`** from the game data. Every edition's id1 `quake.rc` runs, in
   turn:
   1. **`exec default.cfg`**, from the game data. Shareware and retail ship
      one. It starts with `unbindall` and sets id's 1996 layout: arrows to
      move, `a`/`z` to look up and down, `d`/`c` to fly, CTRL to fire, and the
      right mouse button to move forward. That replaces step 1's defaults.
      The rerelease ships no `default.cfg`, so the built-in WASD layout
      stays.
   2. **`exec config.cfg`**, **from the profile only**. A `config.cfg` in the
      installation is never read; for example, GOG ships one in
      `id1/config.cfg`. In a new profile with no `config.cfg` yet, the
      engine supplies one instead. It puts the WASD layout back on top of
      `default.cfg`'s 1996 one: `w`/`a`/`s`/`d`, Space jump, Shift run, left
      mouse attack, right mouse unbound, mouse wheel for next and previous
      weapon, `` ` ``/`~` console, F11 fullscreen, 1–8 weapons. Every edition therefore starts on WASD. Once
      you quit or apply video settings, the saved `config.cfg` takes over.
      See [key layouts](#key-layouts) for id's original layout.
   3. **`exec autoexec.cfg`**, from the profile if there is one, otherwise
      from the game data. GOG's retail ships an `id1/autoexec.cfg` containing
      `+mlook`, and it runs unless your profile has its own.
   4. **`stuffcmds`**: the command line's `+command` arguments.
   5. **`startdemos`**.

   A mod's `quake.rc` replaces id1's and can do more. Arcane Dimensions', for
   example, runs `sv_protocol 999` *after* `stuffcmds`, so it overrides a
   `+sv_protocol` on the command line.

### Key layouts

LuminousQuake starts every new profile on WASD. id's 1996 layout, as
shareware and retail's `default.cfg` set it, is easy to load in any edition,
including the rerelease, which ships no `default.cfg`:

| Command | Layout |
| --- | --- |
| `keys_legacy` | id's 1996 layout: arrows to move and turn, `a`/`z` look up and down, `d`/`c` fly up and down, CTRL fires, the right mouse button moves forward, `\` held for mouse look, F11 zooms |
| `keys_wasd` | LuminousQuake's layout: `w`/`a`/`s`/`d`, Space jump, Shift run, left mouse fires, right mouse unbound, mouse wheel up/down for next/previous weapon (`impulse 10`/`12`), F11 fullscreen |

The new bindings are saved in `config.cfg` like any other. To make the
choice stick regardless of the menus, put the command in `autoexec.cfg`.
`keys_legacy` starts with `unbindall`, exactly as `default.cfg` does;
`keys_wasd` changes only the keys it names.

### `config.cfg` and `autoexec.cfg`

Settings can live in either file. They behave differently:

- **`config.cfg` is the engine's.** It's rewritten when you apply video
  settings and when you quit. It holds every key binding plus the console
  variables marked as saved (`archive`). Anything else you add by hand, such
  as aliases, `exec` lines, comments or unsaved variables, is lost at the next
  write.
- **`autoexec.cfg` is yours.** The engine reads it after `config.cfg` and
  never writes it. Put aliases, scripts and anything you want to win over
  `config.cfg` here. A setting in `autoexec.cfg` overrides the saved one at
  every start, so changing it in a menu lasts only until the next launch.
- Neither is required. With no `autoexec.cfg`, nothing extra runs. With no
  `config.cfg`, the edition starts from step 1 and its `default.cfg`.

### Using a shared community config

Copy the file into the profile folder for the edition and game you play, for
example `rerelease/id1/`. Then either name it `autoexec.cfg`, or run
`exec name.cfg` from the console or from your `autoexec.cfg`. `exec` looks in
the profile first, then in the game data.

Read it first: many configs from the 1990s and 2000s set things that no longer
exist or now mean something different (below). Unknown variables are harmless
and only print `Unknown command`.

## Settings that are new or behave differently

### LuminousQuake settings

Options → **Luminous Options** sets `lq_texture_pack`, `lq_pack_textures`, `lq_pack_bumps`, `lq_pack_parallax`, `lq_pack_brightness`,
`lq_shadows`, `lq_panel_shadows`, `lq_shadow_free`, the tone shoulder and white
point, and both glow gains, each taking effect at once (for comparisons;
**Reset sliders** restores the parallax depth, tone and glow defaults).

| Setting | Default | Meaning |
| --- | --- | --- |
| `lq_renderer` | 0 | 0 Software, 1 Classic GPU, 2 Luminous; applies at `vid_restart` |
| `lq_gamma` | 2.2 | Display gamma for all three renderers (Options → **Gamma**, 1.6–2.8). Higher darkens the mid-tones, lower lifts them; 2.2 shows Quake's palette as drawn. `gamma` (Brightness) still works alongside it; in Luminous it bends the finished frame, as software Quake's does, so it lifts dim areas most |
| `lq_hdr` | 1 | GPU renderers: HDR output (macOS extended dynamic range) where the display is in HDR mode; 0 never. Video Options → **HDR** and **HDR peak** set it and `lq_hdr_peak` |
| `lq_hdr_test` | 0 | 1 replaces the 3D view with an HDR test pattern: SDR white along the top, bands at 0.5–10× it below. Where the bands stop changing is your display's real peak, the value for `lq_hdr_peak` |
| `lq_hdr_peak` | 0 | Caps HDR highlights at this many times SDR white; 0 uses the headroom macOS reports. Set it when bright highlights clip. First check the monitor's own menu: some, like the Acer PM161W, ship with HDR off there and clip everything above white |
| `vid_width`, `vid_height` | 640, 480 | Render resolution; `vid_restart` applies |
| `vid_fullscreen` | 0 | Replaces `vid_mode`; `vid_togglefullscreen` switches |
| `vid_vsync` | 1 | Frames follow the display refresh |
| `host_maxfps` | 0 | Optional render cap; 0 means none. Physics stays at 72 Hz regardless |
| `gpu_filter` | 0 | 0 crisp, 1 smooth texture filtering (GPU renderers) |
| `gpu_mipmaps` | 0 | 0 off, 1 nearest level, 2 trilinear |
| `gpu_anisotropy` | 1 | 1 (off), 2, 4, 8 or 16; needs mipmaps |
| `gpu_msaa` | 0 | 0 off, 2 or 4× anti-aliasing |
| `r_wateralpha` | 1 | Liquid opacity in the GPU renderers (GLQuake's name) |
| `r_lerpmodels`, `r_lerpmove` | 1, 1 | Smooth animation (FitzQuake's names) |
| `r_fluorescent` | 1 | How Luminous's fluorescent tubes appear on camera, 0–4 |
| `lq_particles` | 0 | Solid particles in Luminous: droplets and smoke as shaded spheres |
| `lq_torch_smoke` | 1 | Smoke rising from torches and flames (Luminous with `lq_particles 1`) |
| `lq_shadow_faint`, `lq_shadow_dominance` | 4, 0.05 | When Luminous skips shadow filtering |
| `lq_shadow_detail` | 64 | Brightness at which a light filters its shadow with all 16 samples; 8 from a quarter of it, 4 below (0: always 16) |
| `lq_cull` | 1 | 0 draws every face in the PVS, including those behind the camera or off screen (for comparisons) |
| `lq_shadows` | 1 | 0 lights Luminous without shadow cubes or shadow filtering (for comparisons) |
| `lq_panel_shadows` | 1 | Experimental: neighbouring pieces of a lit panel share one shadow cube, drawn from their centre with the panel as a wide emitter; 0 gives each piece its own |
| `lq_shadow_free` | 1 | Skip shadow lookups proved to find no blocker; 0 looks every one up (for comparisons) |
| `lq_texture_pack` | auto | Luminous: the install's folder holding a community texture pack, such as `rygel_ultra`; `auto` (or empty) takes the first folder whose `textures/` has `*_norm` maps, `none` none. Only its textures are used; changing it reloads the map (saved) |
| `lq_pack_textures` | 1 | Luminous: the texture pack's colour and glow in place of the 8-bit textures (saved) |
| `lq_pack_bumps` | 1 | Luminous: the pack's normal maps shape direct light (saved) |
| `lq_pack_parallax` | 0.04 | Luminous: parallax depth from the pack's normal-map height, as a fraction of the texture's size (DarkPlaces' offset-mapping scale); 0 off (saved) |
| `lq_pack_brightness` | 1 | Luminous: 1 scales each pack texture to its original's mean brightness (maps were lit for those); 0 shows the pack as made (saved) |
| `lq_tone_shoulder` | 0.8 | Luminous: linear light below this is shown unchanged; above it highlights roll off (0.1–0.95) |
| `lq_tone_white` | 6 | Luminous: light that reaches the display's ceiling (SDR white, or the HDR headroom); on HDR at least twice the headroom (1.5–64) |
| `lq_emission` | 3 | Luminous: how many times their palette colour fullbright texels emit, for the world and models (1–16) |
| `lq_sprite_emission` | 3 | As `lq_emission`, for sprites such as explosions (1–16) |
| `lq_scene_cache_mb` | 0 | Memory for keeping the last two maps' built scenes, so a restart skips rebuilding; 0 is a quarter of free RAM, -1 turns it off |
| `lq_workers` | −1 | Worker threads; −1 picks from the CPU |
| `lq_clock_mode` | 0 | Tick-clock timing policy ([frame timing](FRAME_TIMING.md)) |
| `in_nograb` | 0 | Replaces `_windowed_mouse`; 1 never captures the mouse |
| `freelook` | 1 | Mouse look always on (the **Mouselook** option) |
| `keys_wasd`, `keys_legacy` | (commands) | Switch key layouts; see [key layouts](#key-layouts) |
| `sv_protocol` | 666 | 15 (original), 666 (FitzQuake) or 999 (RMQ); next map |
| `max_edicts` | 8192 | Entities per level under 666/999; protocol 15 keeps 600 |

Commands: `setpos x y z [pitch yaw]` (single-player only, moves you in
noclip), `fog density [r g b]` (stored, not yet drawn), `vid_restart`,
`vid_togglefullscreen`, `lq_clock_report`, `lq_performance_report`.

### Classic settings that no longer exist

| Classic setting | Use instead |
| --- | --- |
| `vid_mode`, `_vid_default_mode`, `_vid_wait_override`, `vid_wait`, `vid_nopageflip` | `vid_width`, `vid_height`, `vid_fullscreen`, `vid_vsync` |
| `_windowed_mouse` | `in_nograb` (inverted) |
| `gl_texturemode` | `gpu_filter`, `gpu_mipmaps`, `gpu_anisotropy` |
| `gl_picmip`, `gl_playermip`, `gl_max_size` | None: textures are always full size |
| `gl_flashblend`, `gl_polyblend`, `gl_ztrick`, `gl_keeptjunctions`, `gl_subdivide_size`, `gl_clear`, `r_shadows`, `r_mirroralpha`, `r_novis`, `r_dynamic` | None |
| `cl_maxfps` (QuakeSpasm) | `host_maxfps` |
| `vid_gamma`, `vid_contrast` (QuakeSpasm) | `gamma` |
| `scr_conalpha`, `scr_sbaralpha`, `r_particles`, `r_useportalculling` | None yet |

`gl_cshiftpercent` (screen flashes) is accepted but has no effect. Only
GLQuake's view blend reads it (`V_CalcBlend`, `WinQuake/view.c:481`). This
engine tints the palette as Software Quake does, always at full strength.

### Classic settings that now mean something different

| Setting | What changed |
| --- | --- |
| `r_maxsurfs`, `r_maxedges` | Now default to 32768 and 65536 (were 800 and 2400). Old configs that raise them to a few thousand now **lower** them, and large maps lose geometry in Software. Delete those lines. |
| `-heapsize` | Now defaults to 512 MB, in KB as in WinQuake. `-mem` (DOS, MB) is not supported. |
| `-zone` | Now defaults to 4 MB (was 48 KB). |
| `-particles` | Now defaults to 16384 (was 2048). |
| `-surfcachesize` | Software only; the default now starts at 8 MB. |
| `-sndspeed` | Now defaults to 44100 (was 11025). |
| `_snd_mixahead` | Still the mixing lead time; the SDL audio thread doesn't need old DOS-era values like 0.2. |
| `loadas8bit` | Leave at 0; 8-bit sound only saved memory in 1996. |
| `gamma` | Applied through the palette, as in WinQuake (1 is darkest; 0.5–0.7 brighter). |
| `host_framerate` | Still slow motion; it does not cap frame rate. |
| `sys_ticrate` | Dedicated servers only. |
| `+mlook` in `autoexec.cfg` | Not needed: `freelook 1` is the default. |
| Aliases with `wait` | `wait` still waits one 72 Hz tick, so scripted timing is unchanged. |

## Profiles carried over from older builds

The old shared profile was moved to `rerelease/`, and it kept whatever
bindings it had saved. If retail's `default.cfg` had overwritten them, `a`
looks up and `d` flies up. On October 18, 2026 the author's own moved profile
was repaired: `a` strafes left, `d` strafes right, the right mouse button was
unbound and F11 switches fullscreen. The original is kept beside it as
`config.cfg.before-wasd-2026-10-18`.
