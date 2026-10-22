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
- **Translucent blending in linear light — done October 18, 2026**, with the
  float frame below. Liquids, smoke and sprites blend there, before tone
  mapping. Entity alpha is still not drawn (item 1). Screen tints stay a palette
  wash, as in Software: kept October 20
  ([blending and tints](LUMINOUS_RENDERER.md#colour-linear-light-at-32-bits)).
- **HDR — first version October 18, 2026.** The GPU renderers draw into a
  16-bit float frame. Luminous writes linear light there with an eight-times
  ceiling, and a tone-mapping pass brings it to the display: unchanged below
  0.8, then a hue-keeping shoulder to white (SDR) or to the display's headroom
  (HDR). The swapchain switches to extended-range linear light whenever the
  window's display is in HDR mode (`lq_hdr`). A **Gamma** option for all three
  renderers sits after Brightness (`lq_gamma`). Measured on the user's Acer
  PM161W: 2.1× headroom (4.2× potential), over USB-C or HDMI. Still
  to come: exposure and tuning
  the shoulder and white point by eye on HDR. Added the same day: Video
  Options → **HDR** and **HDR peak** (`lq_hdr_peak` caps highlights where a
  display is told more headroom than it shows, as mirrored displays are), an
  in-game test pattern (`lq_hdr_test`), and smoke blended in linear light.
  Smoke (October 19) keeps the ragged cutout that broke up its spheres:
  eroded holes and a frayed silhouette. Inside that edge it is blended by how
  much smoke the view crosses: densest through the middle (70%), thinning
  smoothly toward the rim and the holes, and thinner as it ages.
  HDR highlights are visible in game on the Acer PM161W once its own HDR mode
  is switched on in the monitor's menu (it ships off); the earlier clipping
  was that, not the engine (and that switch, not the cable, was why USB-C
  showed no HDR at first). Its peak, found with `lq_hdr_test`, is 2×: the 3,
  4, 6 and 10× bands look the same. That matches the 2.09× headroom macOS
  reports, so the default cap ("as reported") is right and `lq_hdr_peak`
  isn't needed. Next: tune the shoulder and white point by eye. **Started
  October 20:** `lq_tone_shoulder` (0.8), `lq_tone_white` (6), `lq_emission`
  (3) and a separate `lq_sprite_emission` (3) for explosions and other sprites
  now set these live, for choosing new defaults on the Acer.
- This comes before material packs, because specular and normal-mapped light
  look wrong when computed in gamma space.

### 4. Community material packs

Normal, height, gloss and glow maps from community packs, with relief shadows
from the height values and specular from gloss maps. Design and the DarkPlaces
conventions are in [material packs](MATERIAL_PACKS.md). The test data (QRP,
Rygel) goes in an ignored game directory.

**First stage done October 20:** high-res colour (with glow) and bump maps
from loose pack files, on solid world surfaces in Luminous, switched on
Options → Luminous Options. Decided: parallax and relief shadows, the latter
from the strongest light only; compression by measurement. October 21:
brightness matching (Pack brightness), pack colours following Quake's
screen tints, the bump frame's sign, parallax (Parallax depth) with each
texture's mean height held at the face, and Quake's `gamma` applied to the
finished Luminous frame, as software Quake does. Texture packs read straight from a `.pk3` beside the app or in `id1`
(October 21). Still to come: gloss specular, relief shadows, a
parallax fade with distance, liquids, sky, models and `_bump` maps
([status](MATERIAL_PACKS.md#implemented-first-stage-october-20)).

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
- **Done October 19: camera culling** (`renderer/gpu_sdl.c`, `LQ_GPURender`).
  The main pass skips world faces whose plane faces away from the camera or
  whose bounds lie outside the view, and frame geometry (actors, brush models,
  particles, sprites) outside the view; the view weapon is always drawn.
  Shadow passes are unchanged, so off-screen casters still cast. In the viewer
  (no PVS, 5176 faces), E1M3 views drew 23 and 86 faces with images identical
  but for 2–6 edge pixels; `lq_cull 0` and the viewer's `--no-cull` turn it
  off. Measured gain: 0.05–0.13 ms a frame (table below).
- **Measured October 19: where Luminous frame time goes.** `timedemo` of the
  rerelease's demo1 (E1M3) and demo2, `build-profile` (RelWithDebInfo), Luminous,
  1920×1080 window, VSync off, HDR display, M1 Max. Medians of 3–6 interleaved
  runs; run-to-run spread under 1% except shadows off (up to 3%):

  | Setting | demo1 fps (ms) | demo2 fps (ms) |
  | --- | ---: | ---: |
  | Normal | 93.0 (10.75) | 112.2 (8.91) |
  | `lq_cull 0` | 92.6 (10.80) | 110.6 (9.04) |
  | Cubes drawn, no filtering (`lq_shadow_faint 1000000`, `lq_shadow_dominance 0`) | 160.0 (6.25) | 167.3 (5.98) |
  | `lq_shadows 0` | 205.8 (4.86) | 217.3 (4.60) |

  Shadow filtering costs 4.5 ms (demo1) and 2.9 ms (demo2) a frame, 42% and
  33% of the frame; shadow cubes about 1.4 ms in both; everything else about
  4.7 ms. Filtering is the first target (review item 1).
- **Done October 19: shadow filtering follows brightness**
  (`renderer/shaders/world.frag`, `shadowFactor`). A light whose unshadowed
  contribution at a point reaches `lq_shadow_detail` (64; 128 is normal
  brightness) takes all 16 blocker and 16 comparison samples, from a quarter
  of that 8, below it 4. Bright and medium lights also stop after their four
  rim samples when all four are occluded: inside a thick shadow. Lit or mixed
  rims always take the full set, so thin shadows survive; a thin gap of light
  inside a thick shadow is the case given up. Same benchmark as above:

  | Setting | demo1 fps (ms) | demo2 fps (ms) |
  | --- | ---: | ---: |
  | Before (table above) | 93.0 (10.75) | 112.2 (8.91) |
  | Thick-shadow exit only (`lq_shadow_detail 0`) | 96.7 (10.34) | 112.7 (8.87) |
  | Brightness-scaled sampling (default) | 123.4 (8.10) | 140.8 (7.10) |

  Filtering now costs about 1.9 ms (demo1) and 1.1 ms (demo2), down from 4.5
  and 2.9. Viewer shots of five map starts against the old shader: 4–20% of
  pixels change, nearly all by under 8 of 255 levels; visible only zoomed, as
  slightly grainier dim penumbrae and a few lit specks where a dim light took
  4 samples. Before rows ran earlier the same day, not interleaved.
- **Overheating, October 18.** Idle monsters' blended poses shift their bounds
  a little every frame, and the shadow cache counted that as movement: on
  E1M3, about 45 lights redrew 109 cube faces every frame with the player
  standing still. A caster now counts as moving only after drifting 2 units
  from where its shadow was last drawn: 12 faces a frame, 84 → 120 fps (the
  VSync cap). What remains is the main scene pass, about 11 ms a frame at
  2560×1920 on the M1 Max; at 120 Hz it holds the GPU at its maximum clock,
  while at 60 fps (`host_maxfps 60`) the GPU stays at its medium clock.
  `developer 1` prints the per-frame shadow work once a second. Capping the
  frame rate is not the fix; the cost is.
- **Lights that reach nothing on screen, October 19.** A map light was kept
  whenever its reach overlapped the view's PVS; the on-screen test ran only
  over the slot budget. At E1M3's start that was 70 lights, each shaded and
  shadowed. Now a light is kept only if it lights a PVS surface inside the
  selection view (a little wider than the screen): 24 there, with the frame
  unchanged (0.06% of pixels differ, from smoke). Uncapped, without VSync:
  159 → 191 fps at 640×480, 123 → 143 fps at 1280×960. The rest is about 6 ms
  a frame that doesn't scale with resolution; the CPU spends 2.1 ms preparing
  the scene and 2.5 ms submitting it. Next: profile that fixed cost.
  **Revised the same day:** dropping those lights made performance collapse
  in play. Those fixed-camera measurements missed it: a light that left the
  view lost its slot and came back in another, so its whole shadow cube was
  redrawn whenever the player looked around (a room of 48 lights turned
  through two circles: 55 full cube rebuilds, against none before). Reachable
  lights now keep their slots again; those lighting nothing in view are
  marked offscreen, and the shadow pass leaves their cubes untouched until
  they light something in view, then redraws what casters moved through
  meanwhile. Ranking by view applies only over the slot budget, since
  ranking by it under budget moved lights between shadow tiers at every
  turn. Not yet measured in game.
- **E1M1's start, October 19.** The "crash" there is thermal: uncapped, the
  frame rate falls from ~100 to ~20 fps within 20 s while every count (lights,
  shadow redraws, draws) stays constant, so the GPU is throttling, and
  run-to-run comparisons swing 2x. 79 lights reach surfaces on screen there;
  about 45 are ericw-tools `_surface` emitters (one per ~128-unit piece of
  each tlight/slipside panel: 12 on the slipgate sides, 15 on one lamp
  strip), each a shadowed point light. 5.6 lights shade an on-screen surface
  on average, 24 at most. `lq_lights` lists the kept lights and what they
  touch. Undecided: merge neighbouring emitters of one panel into one light,
  or keep emitters unshadowed (the rerelease's lighting is otherwise kept as
  authored, item 1a). A lead to check: the pre-HDR build measured higher in
  interleaved runs (32 against 11-24 fps).
  **Trying since October 20: panel shadows** (`lq_panel_shadows`). Pieces keep
  their own light but share one shadow cube per group of up to four, drawn
  from the group's centre ([panel shadows](LUMINOUS_RENDERER.md#classic-and-rerelease-data)).
  On E1M1 that is 55 cubes for 148 pieces. Its look in game and its cost are
  not yet judged or measured.
- **Done October 20: CPU-side review fixes and finer light lists.** Review
  items 3, 5 and 8 ([performance review](PERFORMANCE_REVIEW.md)): the overlay
  texture is reused, the swapchain is waited for after recording, cube
  rebuilds classify static casters once, and caster rest matching is linear.
  Each world face drawn now gets the light that is brightest everywhere on it
  when one is (`KnownLead` in `renderer/gpu_sdl.c`, from conservative bounds
  of every light over the face), and the shader skips its search for the
  brightest light there; the image is unchanged. The `developer 1` readout
  shows how many faces this covers. The user reported play "seems better";
  not yet measured.
- **Background queue, October 20** (`platform/background_sdl.c`). One
  low-priority thread for work that may finish frames later. Results return
  on the main thread once a frame; a map change or video restart cancels the
  map's tasks before its scene is cached or freed. First task: after each
  Luminous map load, count the light-face pairs whose shadow lookup provably
  finds nothing (`LQ_SceneCountShadowFree`), printed to the console.
  **Done October 20: those lookups are skipped** (`lq_shadow_free`, default
  1). The task keeps the clear pairs as bits per face and hands them to the
  loaded scene. Each frame a slot's bits count only while its light stands at
  its source (not a moved flame) and no actor or moving brush lies within its
  reach. Not yet measured; the next performance round should compare
  `lq_shadow_free 0` and `1`.
- **CPU or GPU, October 20.** What suits each: per-pixel and per-texel work
  stays on the GPU; per-light, per-face and per-entity work with branches
  suits the CPU; work that may arrive frames late suits idle cores. Large
  per-frame uploads cost more on discrete GPUs than on shared memory. Runtime
  balancing mostly adjusts rates (refreshes per frame, workers) rather than
  moving tasks; gameplay-light probes are the first task with both paths.
  It needs a GPU load signal: SDL 3.4 has no GPU timers, but the swapchain
  wait now measures GPU backlog.
- **Recent maps stay loaded.** Decided October 17: keep the built scenes of
  the last two maps in memory, under a size limit. A map restart after a
  death, frequent on Nightmare, then reuses them instead of rebuilding. The
  expensive part is Luminous's bounce (27 s on `ad_tfuma`); the BSP reload
  itself is quick. Key the cache on the map file and the settings that shape
  the scene. Not the whole game, since texture upgrades make that too large.
  This is item 5's bounce/probe cache, held in RAM first.
  **Done October 19** (`platform/render_bridge.c`): the last two maps' scenes
  are kept, keyed by game folder, map name, a hash of geometry, entities and
  lightmaps, the renderer and the fluorescent camera. A restart, or a return
  to a map, reuses the scene (bounce skipped; the frame matches a fresh
  build). `lq_scene_cache_mb` caps it: 0 (default) is a quarter of free RAM
  when first needed, the agenda's 50% available split 50/50; -1 is off. The
  RAM slider below will set it.
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
- **Sound mixing on its own thread, with the door checks.** Proposed by the
  user October 19 ([handoff](HANDOFF.md), open thread 5) and missed from this
  page until October 22. The open-air path search (`WinQuake/snd_path.c`) runs
  on the main thread right before mixing, so a slow search or a slow frame
  starves the 0.1 s mix-ahead. Reported October 21 as audio skipping on E1M1's
  catwalk to the nailgun. That spot does about 5× the search work of the
  spawn, by an offline count (not timed). Mixing comes first on the
  thread. The search fills the time left, nearest cells first, because a
  distant sound's late update matters least.
  **Built October 22, not yet played:** a sound thread (`platform/snd_sdl.c`)
  mixes every 5 ms, then searches for up to 3 ms. Sounds keep the last
  finished search until the next is done. The main thread hands over sounds
  and, once a frame, the listener and where each door and lift is. Loaded
  sounds stay in memory of their own, outside Quake's cache, for the session.
  `snd_trace` and `soundpaths` print a frame late, through the main thread.
  Only sealing movers within the search's reach are traced, and the muffle
  pass traces none.

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
  geometry. Relief comes only from material packs. An October 20 experiment,
  [AI generated normals](AI_GENERATED_NORMALS.md), inferred relief and
  lighting-free paint for one texture by hand-guided fitting. It is not in the
  engine and doesn't change this decision.
- **Quake 2, Quake 3 and Half-Life map formats, CSQC, and effects such as motion
  blur**, unless a specific mod we support needs one.

## Open items carried from earlier work

- **Done October 19: mouse look dead until a click** (reported October 18).
  Cause: when the game started fullscreen, macOS slid it into a fullscreen
  Space and then sent the app no "mouse moved" events at all, in either
  axis, until a click in the window. Focus, relative mode and the held
  cursor were all correct. A logged Dock launch showed zero motion events
  until the first click and 22–62 a second right after it. Starting windowed
  and fullscreen without a Space both worked with no click. Fix (a400ee7):
  `VID_Init` sets `SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES` to 0 on macOS.
  Confirmed by the user from a Dock launch. Ruled out on the way: the
  per-edition profiles (1e88477 had the bug), app activation, and SDL's
  "window moving" state. The October 19 capture experiments (5c04a7c:
  re-capturing on window events, `LQ_MacSettleWindowMove`, `input.log`) were
  then removed.

These were already recorded elsewhere. They are listed here so they appear in
every Not-yet-done list.

- Rerelease compatibility: numeric builtins 79/80, ending and local-sound
  services, and a load-time capability audit
  ([rerelease comparison](RERELEASE_COMPARISON.md)).
- Rerelease fog.
- Remaining Phase 3A delivery checks; broader campaign validation.
- Windows and Linux runtime testing; DXIL generation before D3D12.
- Mac signing and notarization; Windows/Linux packaging.
