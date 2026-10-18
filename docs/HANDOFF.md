# Session handoff — October 18, 2026

For the next working session. The standing priorities are in
[AGENDA.md](AGENDA.md); this page covers only what is in flight.

## State of the tree

- Branch `master`. Everything below is committed on top of `1e88477`; not
  pushed.
- All 23 tests pass, and `build/LuminousQuake.app` is current.

## What this round added

**Modern content (agenda item 1)**
- BSP2 and 2PSB maps load (`model.c`, `bspfile.h`), with 32-bit in-memory
  structures.
- Protocols 15, 666 and 999 on both client and server (`sv_protocol`, default
  666).
- Raised limits throughout:
  - edicts (`max_edicts`), models, sounds, statics and efrags;
  - message sizes, with the signon sent in pieces;
  - the lightmap atlas (4096 wide for big maps);
  - Software surfaces and edges;
  - 512 MB heap (`-heapsize`).
- Arcane Dimensions starts in all three renderers. All nine of its BSP2 maps
  sign on headless.
- Original id1 demos play, and old saves load. Details are in AGENDA item 1
  and in [LUMINOUS_RENDERER.md](LUMINOUS_RENDERER.md).

**Other 1996 hardware limits**
- Raised: pak files, QuakeC stack, zone, cached pictures, particles, Software
  clipping and surface cache, alias vertices, console, command-line
  arguments.
- Simultaneous sounds raised from 8 to 64.

**Editions and configuration**
- Each release gets its own profile, `<root>/<edition>/<game>`, where the
  edition is `shareware`, `retail` or `rerelease` (detected by `COM_Edition`).
- The old shared profile moves to `rerelease/` on first rerelease run.
- New profiles start on WASD, with the mouse wheel for next and previous
  weapon: a missing `config.cfg` is replaced by the layout in `keys.c`.
- `keys_legacy` loads id's 1996 layout in any edition; `keys_wasd` switches
  back.
- An unbound `` ` ``/`~` still opens the console.
- [CONFIGURATION.md](CONFIGURATION.md) documents where configs live, the load
  order, `config.cfg` against `autoexec.cfg`, and obsolete classic settings.

**Testing aid**
- `setpos x y z [pitch yaw]` (single-player cheat) places the player in noclip
  for renderer comparisons.

**Luminous effects**
- Rerelease torch accent spotlights follow their flame's strength (fixed in
  place). They keep a 20% flat core and an 80% penumbra.
- Smoke is shaded spheres:
  - sizes skewed small;
  - eaten away by a noise cutout, with ragged silhouette fringes;
  - grenade and rocket smoke capped at Software particle size.
- Torches and flames smoke (`lq_torch_smoke`):
  - the plume rises fastest at its centre;
  - more puffs while the flame's light is falling;
  - puffs attract one another into overlapping clumps.

## Not yet done

- Lighting against retail: Luminous matches retail's bake when given retail's
  map. The rerelease's relit lights are what make E1M3 brighter (its light
  values, the gold-key light). Undecided: use retail's lights when the retail
  paks are present, or tone the rerelease down.
- `luminousquake-gpu --accent-gain 0` stops during startup without a message.
- Never checked in play:
  - the 64-channel sound;
  - the console-key safeguard;
  - trail smoke at length.
- AD: entity alpha, fog and skyboxes are received but not drawn.
  `U_LERPFINISH` isn't used by frame blending yet; ask before wiring it in.
- Luminous bounce takes 27 s at load on `ad_tfuma`, and its light-reach table
  overflows.
- See-through smoke waits for blending in linear light (the HDR work).

## How to test without trouble

- **Scratch game folders link pak files only.** Write test `autoexec.cfg`
  files as new regular files. A linked install config was once overwritten
  through a symlink.
- **In-game shots**: in the scratch `id1/autoexec.cfg`, set `unbindall`,
  `lq_renderer N` and `map e1m3`. Then about 30 `wait` lines, then `god`,
  `notarget` and `setpos …`. Run `build/luminousquake -basedir <scratch>
  -userdir <scratch>/ud_<unique> -nosound -nomouse -width 1280 -height 960
  --run-seconds 14 --screenshot out.bmp`. `-nomouse` stops a real mouse
  turning the view. Give separate impulses separate frames (`wait`s), since
  only one impulse is sent per frame.
- **Headless**: `build/luminousquake-headless … --run-seconds 8 +developer 1
  +map <name>`. Long command lists go in a `.cfg`, because the command line is
  limited to 256 arguments.
- **Viewer** (id1 only): `build/luminousquake-gpu -basedir … -map e1m3
  --luminous --fixed-camera --run-seconds 3 --camera "x y z pitch yaw"
  --screenshot out.bmp`.
- Use a new scratch folder per run; deleting folders with `rm -rf` is blocked.
- **Before running anything that opens windows**, tell the user and batch the
  runs.

## Open threads, in suggested order

1. Choose the retail-baseline lighting approach.
2. Draw what AD sends: entity alpha (with linear-light blending), fog,
   skyboxes.
3. The RAM slider, scene caching for the last two maps, and memory-scaled
   lighting (AGENDA item 6).
4. Luminous load time and light reach on large maps.
5. Profile again, then threading (a mixing thread with the door checks, a
   loading thread) and CPU/GPU balancing.
6. Sound loose ends:
   - audio spikes;
   - no high-frequency muffling;
   - doors that start open;
   - tuning the 35% muffle level.
7. The rest of the agenda.

## Reference

- QuakeSpasm's `protocol.h`, `sv_main.c` and `cl_parse.c` are the reference
  for protocols 666 and 999; `reference/darkplaces` doesn't implement them.
- Retail data is in `quake-retail` (GOG 1.06 paks). The GOG installer is in
  `~/Downloads` (`innoextract`) if an original file needs restoring.
