# Agenda — October 17, 2026

This is the standing priority list. It records what we have decided to build,
in order, what we have decided not to build, and what is still open from
earlier work. Every working session ends with a **Not yet done** list drawn
from this page, so no priority is dropped without anyone noticing. When an item
is finished, mark it done here with the date and a link to its evidence. When
we change our minds, move the item and say why.

The [implementation plan](../IMPLEMENTATION_PLAN.md) keeps the phase structure
and constraints. This page sets the order.

## Decided, in order

### 1. Modern content: FitzQuake protocol 666, BSP2 and raised limits

**Mostly done October 17–18, 2026.** BSP2 and 2PSB load, the
server and client speak protocols 15, 666 and 999 (`sv_protocol`, default
666), and the limits are raised. Arcane Dimensions starts `ad_tfuma` in all
three renderers. All nine of its BSP2 maps sign on headless. Original id1
demos play, and old saves load. Still open:
- entity alpha, fog and skyboxes are received but not drawn;
- `U_LERPFINISH` isn't yet used by frame blending;
- Luminous bounce on a large map takes about 27 s at load;
- AD hasn't been played by hand.

Other 1996 hardware limits were raised the same day: pak files, the QuakeC
stack, zone memory, cached pictures, particles, Software clipping and surface
cache, alias vertices, the console, command-line arguments, and 64
simultaneous sounds instead of 8. Details are in [HANDOFF.md](HANDOFF.md) and
[CONFIGURATION.md](CONFIGURATION.md).

Before this work, the engine had the 1996 limits: network protocol 15, 600 entities
(`WinQuake/quakedef.h:114`) and BSP version 29 only (`WinQuake/model.c:1164`).
Arcane Dimensions, Quoth-era maps and most content from the 2010s need more.
QuakeSpasm, ironwail and DarkPlaces all treat this as the baseline.

- BSP2 (and its `2PSB` variant): 32-bit counts and indices throughout the
  loader, and in every Luminous structure that keys off faces, leaves or
  vertices (light lattice, bounce layers, face light masks, probes).
- FitzQuake protocol 666 for both server and client. Covers its extended
  entity, model and sound fields, demo record and playback, and the svc
  messages its mods rely on. Protocol 15 stays available, so original demos
  and original multiplayer behaviour remain intact.
- Community skyboxes: the six-face `gfx/env/<name>{rt,bk,lf,ft,up,dn}`
  convention, selected by worldspawn `sky` and the `sky` console command.
  The Luminous sky already filters and draws at palette colour (October 17,
  2026; [sky and liquids](LUMINOUS_RENDERER.md)).
- Raised limits for entities, models, sounds, static entities and message
  sizes. Per the plan's QuakeC policy, each limit gets a coordinated review of
  allocation, VM access, network encoding and save/demo format.
- Done means:
  - Arcane Dimensions starts and plays a representative map in Software,
    Classic GPU and Luminous.
  - Original id1 demos still play.
  - Old saves still load.

### 1a. Rerelease lighting stays as authored

Decided October 18, 2026. Players of the rerelease keep the rerelease's own
relit lights; Luminous does not swap in retail's light list or tone the
rerelease down. The one exception is the torch accent spotlights already
changed (they follow their flame and keep a 20% flat core). Luminous already
matches retail's bake on retail data.

### 2. Frame blending for models

**Done October 17, 2026** for Classic GPU and Luminous (`platform/gpu_entities.c`),
after FitzQuake:
- `r_lerpmodels`: MDL poses blend over each 0.1 s think, or a grouped
  animation's own interval. Fire on FitzQuake's no-blend list (flames,
  braziers, long torches) reshapes rather than moves between poses (74–83% of
  its vertex motion is not an overall stretch), so its mesh keeps the authored
  poses at 10 Hz; interpolated flame shapes were tried and pulled. In Luminous
  the torch light's strength steps at 20 Hz: a third of the way to the next
  pose's value through the first half of the loop, two thirds through the
  second. Its position, and so its shadow, stays at 10 Hz. Smoother flames need
  designed intermediate poses. The saw and fist view models stay stepped.
- `r_lerpmove`: entities the server marks `U_NOLERP` (`MOVETYPE_STEP`
  monsters) ease toward the server's latest origin and angles over 0.1 s, one
  step behind; a jump over 100 units snaps as a teleport. This also removes
  NetQuake's step-back between messages during demo playback.
- In demo1, the pose blend took in-between values on about 75% of a monster's
  frames, and its drawn position moved about 0.3 units a frame where the
  server's jumped 2.5 and stepped back.

Video Options' **Smooth animation** turns both on or off. Still open:
Software keeps the original stepping; moving brushes (doors, lifts) already
interpolate between messages and are unchanged.

### 3. Luminous colour pipeline: sRGB, 32-bit and HDR

Luminous only. Classic GPU stays palette-exact.

- **Linear lighting and 32-bit colour — done October 17, 2026.** Surface
  colour is decoded to linear light, lit and filtered there, and encoded for
  the display once. Shading no longer passes through the 64-step colormap.
  Light still sums in Quake's brightness units and becomes linear once,
  keeping Classic's composition. Evidence and choices:
  [Luminous colour](LUMINOUS_RENDERER.md#colour-linear-light-at-32-bits).
- **Still open for this item:** translucent blending in linear light, which
  needs the float target below.
- **HDR — first version October 18, 2026.** The GPU renderers draw into a
  16-bit float frame. Luminous writes linear light there with an eight-times
  ceiling, and a tone-mapping pass brings it to the display: unchanged below
  0.8, then a hue-keeping shoulder to white (SDR) or to the display's headroom
  (HDR). The swapchain switches to extended-range linear light whenever the
  window's display is in HDR mode (`lq_hdr`). A **Gamma** option for all three
  renderers sits after Brightness (`lq_gamma`). Measured on the user's Acer
  PM161W over HDMI: 2.1× headroom (4.2× potential); over USB-C, no HDR. Still
  to come: exposure, blending translucent surfaces in linear light, and tuning
  the shoulder and white point by eye on HDR. Added the same day: Video
  Options → **HDR** and **HDR peak** (`lq_hdr_peak` caps highlights where a
  display is told more headroom than it shows, as mirrored displays are), an
  in-game test pattern (`lq_hdr_test`), and smoke blended in linear light.
  HDR highlights are visible in game on the Acer PM161W once its own HDR mode
  is switched on in the monitor's menu (it ships off); the earlier clipping
  was that, not the engine. Next: find its peak with `lq_hdr_test` and tune
  the shoulder and white point by eye.
- This comes before material packs, because specular and normal-mapped light
  look wrong when computed in gamma space.

### 4. Community material packs

Normal, height, gloss and glow maps from community packs, with relief shadows
from the height values and specular from gloss maps. Design and the DarkPlaces
conventions are in [material packs](MATERIAL_PACKS.md). The test data (QRP,
Rygel) goes in an ignored game directory.

### 5. The existing Luminous queue

From the [Luminous notes](LUMINOUS_RENDERER.md#next-implementation-sequence):

- Probe visibility weighting against light leaking through walls.
- A bounce/probe cache, so map load doesn't redo the gather. In RAM first,
  for the last two maps (item 6).
- Staged [light reconstruction](LIGHT_RECONSTRUCTION.md).
- Refinement of the in-game path against Classic GPU.

### 6. Single-player optimization

Make single-player fast and smooth on the target Macs before any online work
starts: measured with the [profiling method](PROFILING.md), against fixed
scenes and frame-time goals.

HDR and material-pack relief shadows both add GPU work, so this comes before
them. Principles, decided October 17, 2026:

- **Nothing draws the whole map.** Every pass, including shadow cubes, draws
  only what its view and the light's reach can see. Doom 3 (dhewm3 in
  `reference/`) is the guide: a light acts only inside its volume.
- **A light ends where its falloff fades out**, unless it is a sun or another
  regional light (the rerelease's no-falloff sunbeams). Inverse and
  inverse-square lights, about 40% of Arcane Dimensions' lights, need a cutoff,
  with ericw-tools' `_gate` honoured and a taper to zero so no ring shows.
- **A spotlight acts only inside its cone**, for lit surfaces and for shadow
  cube faces. E1M3's 32 sunbeams are no-falloff spotlights that currently count
  every surface within 16,384 units and draw all six cube faces.
- **Shadows are filtered only where they show**: none for faint lights
  (`lq_shadow_faint`) or for lights overwhelmed by a brighter one at that point
  (`lq_shadow_dominance`).
- **Done October 17:** stationary casters no longer redraw shadows, moving ones
  redraw over a cached static face, cube faces draw only in-range surfaces,
  spotlights are culled to their cones, unbounded falloffs stop at their fade
  level or `_gate`, and faint or overwhelmed lights skip shadow filtering.
  E1M3: 76 → about 110 fps uncapped.
- **Recent maps stay loaded.** Decided October 17: keep the built scenes of
  the last two maps in memory, under a size limit. A map restart after a
  death, frequent on Nightmare, then reuses them instead of rebuilding. The
  expensive part is Luminous's bounce (27 s on `ad_tfuma`); the BSP reload
  itself is quick. Key the cache on the map file and the settings that shape
  the scene. Not the whole game, since texture upgrades make that too large.
  This is item 5's bounce/probe cache, held in RAM first.
- **Lighting resolution scales with memory.** Decided October 17: a
  user-assigned memory budget for lighting (probe spacing, shadow map size,
  bounce resolution), and Luminous uses as much of it as helps. 64 GB is high
  but not unusual, and the 1996 heap gives no hint of what is available.
- **One RAM slider for both.** Decided October 17. Two options, each on by
  default: "Allocate available RAM to scene caching" and "Allocate available
  RAM to lighting complexity". A slider divides the available RAM between them
  as percentages, default 50/50, and is enabled only while both are on. With
  one option on, it gets all of the available RAM; with neither, Luminous uses
  today's fixed sizes. "Available RAM" is a data field in the same place: an
  amount of memory, prefilled at 50% of the RAM free when it is first shown,
  and limited to 100% of free RAM. The slider divides that amount. "Free"
  includes inactive and purgeable pages, which macOS reclaims on demand.
- **Balance follows the hardware.** Measured October 17 at 1080p: the CPU does
  about 4 ms of work a frame, the GPU about 12 ms. Work that can run on either
  should move to whichever is idle, chosen at run time from measured load, not
  fixed per task.

### 7. Online play, at least to the QuakeWorld standard

Postponed until single-player is optimized. We can't call the engine done
without it. The minimum is what QuakeWorld already gave players:

- client-side prediction of the player's own movement;
- delta-compressed entity updates;
- a network channel that tolerates packet loss;
- play that stays responsive at ordinary internet pings.

The QuakeWorld source in `QW/` is the reference. Whether we speak an existing
protocol for interoperability is decided when this starts.

## Decided against

- **Per-map light override files and an in-game light editor**
  (DarkPlaces `.rtlights`, `r_editlights`). These are DarkPlaces-specific. The
  last-resort map-local composition patch described in the Luminous notes
  remains.
- **Generated relief from the original 8-bit textures.** Painted shading is not
  geometry. Relief comes only from material packs.
- **Quake 2, Quake 3 and Half-Life map formats, CSQC, and effects such as motion
  blur**, unless a specific mod we support needs one.

## Open items carried from earlier work

These were already recorded elsewhere. They are listed here so they appear in
every Not-yet-done list.

- Rerelease compatibility: numeric builtins 79/80, ending and local-sound
  services, and a load-time capability audit
  ([rerelease comparison](RERELEASE_COMPARISON.md)).
- Rerelease fog.
- Remaining Phase 3A delivery checks; broader campaign validation.
- Windows and Linux runtime testing; DXIL generation before D3D12.
- Mac signing and notarization; Windows/Linux packaging.
