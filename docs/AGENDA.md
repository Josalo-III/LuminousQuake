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

Next up. The engine has the 1996 limits: network protocol 15, 600 entities
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

### 2. Frame blending for models

Already in Phase 4 of the plan ("animation interpolation"), and still not in
`renderer/`. Blend between MDL poses and smooth moving brushes and entity
origins, so monsters don't step at 72 Hz and above. Classic and Luminous both
use it; Software keeps the original stepping unless we decide otherwise.

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
- **HDR.** Light accumulates in a floating-point target, then exposure and
  tone mapping bring it to the display. Find out whether macOS extended dynamic
  range (EDR) displays can show the headroom directly.
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
- A bounce/probe cache, so map load doesn't redo the gather.
- Staged [light reconstruction](LIGHT_RECONSTRUCTION.md).
- Refinement of the in-game path against Classic GPU.

### 6. Single-player optimization

Make single-player fast and smooth on the target Macs before any online work
starts: measured with the [profiling method](PROFILING.md), against fixed
scenes and frame-time goals.

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
