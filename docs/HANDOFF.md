# Session handoff — October 17, 2026

For the next working session. The standing priorities are in
[AGENDA.md](AGENDA.md); this page covers only what is in flight.

## State of the tree

- Branch `master`. The last commit, `b832e07` (frame blending, pushed), took the
  flame mesh animation out and kept the 20 Hz torch-light rule.
- **Everything below is uncommitted**, 17 files. All 23 tests pass
  (`ctest --test-dir build`) and `build/LuminousQuake.app` is current.
- The user has not yet checked the shadow or sound changes in play. Commit only
  when they ask.

## Uncommitted work

### Crash fix and crash log
- **Fix** (`WinQuake/snd_path.c`): the sound code's list of doors and lifts
  kept entity pointers. An entity that lost its model before the list was
  rebuilt crashed `SND_Reach`, reading the hull of a NULL model; the crash
  report showed address 0x138. Each mover now records its model when the list
  is built and is skipped if the model has changed. The user reports the
  crashes are gone.
- **Crash log** (`platform/sys_portable.c`): a fatal signal appends a backtrace
  to `~/Library/Logs/LuminousQuake/crash.log` (on other systems,
  `~/.local/state/luminousquake`). `LQ_NO_CRASH_LOG=1` disables it. The
  handler has never fired for real; it is untested.

### Sound through doors (`snd_path.c`, `snd_dma.c`, `sound.h`, README)
- Closed doors and lifts **muffle** sound to 35% (`SND_MUFFLE`). Solid walls
  still shut it off. A second path search ignores movers; a sound reachable
  only that way is muffled.
- **Fades over 0.3 s** (`SND_FADE`), reaching full muffling exactly as a door
  finishes closing and lifting over the first 0.3 s of opening. A door's seal
  comes from how far it is from its spawn (closed) position and its measured
  speed.
- A mover never blocks **its own noise**. Lift and door sounds come from inside
  their brush, and many had been going missing.
- Door bounds are taken live, not from the last 0.1 s search.
- `LQ_SoundPathDistance` gained an `*open` gain output. The command that lists
  playing sounds now prints it.
- The user's design intent: a closed door suppresses what is beyond it, an open
  door suppresses nothing, and changes fade rather than cut. In the Necropolis,
  the crusher machinery should stay audible, muffled, after the door closes.

### Luminous shadows and culling (renderer/, world.frag, docs)
Measured on E1M3's start (1080p, 2× MSAA, VSync off): **76 → about 110 fps**.
- **Casters at rest** (bounds unchanged from the last frame) no longer trigger
  cube redraws.
- **Static copy**: each shadow cube has a static-only twin (`shadow_static`,
  about 180 MB). A face redrawn for moving casters copies the static face and
  draws only the movers over it.
- **Per-face culling**: static casters are split into per-surface ranges with
  bounds (`caster_ranges`). A face draws only surfaces within the light's range
  and that face's frustum. This gave most of the speed-up: the old code drew
  the whole map into every redrawn face.
- **Spotlight cones**: `LQ_SpotReachesBox` (scene.c) limits a spotlight's
  surfaces to its blurred cone, and `BeamFaces` (gpu_sdl.c) limits its cube
  faces; faces outside the beam are cleared. This matters for E1M3's 32
  no-falloff sunbeams.
- **Falloff cutoff**: inverse and inverse-square lights stop at 1 brightness
  unit (`LQ_LIGHT_FADE_LEVEL`) or at a higher ericw-tools `_gate` (worldspawn or
  per light), and the shader tapers them to zero over the last fifth of the
  range. Linear lights are unchanged; no-falloff lights are regional and keep
  the full extent.
- **Shadow cutoffs**: no shadow filtering for a light below `lq_shadow_faint`
  (default 4 brightness units), or below `lq_shadow_dominance` (default 5%) of
  the brightest light at that point after its own shadow. Both are console
  variables, with matching viewer flags `--shadow-faint` and
  `--shadow-dominance`. On E1M3 they changed at most 3 of 255 levels.
- Cones, falloff and the cutoffs added only about 1–3 fps at E1M3's start,
  within run-to-run variation. They should matter more on light-dense maps and
  on Arcane Dimensions, where about 40% of the lights are inverse-square.

## How to test without trouble

- **Viewer screenshots**: `build/luminousquake-gpu -basedir quake-complete/Quake
  -map e1m3 --luminous --fixed-camera --run-seconds 3 --screenshot out.bmp`.
  - Without `--run-seconds` the window waits for someone to close it.
  - Without `--luminous` it shows Classic lighting.
  - Add a `--no-shadows` control shot to prove the setting under test is in
    effect.
- **Don't compare at E1M1's start**: its failing fluorescent tube flickers at
  random.
- **Game runs** always use `-userdir <scratch>`, so they never write the user's
  profile, plus `--run-seconds`. For timing: `+vid_vsync 0 +lq_profile 1
  --profile-warmup-seconds 8 --profile-report`.
- **Metal traces**: `xcrun xctrace record --template 'Metal System Trace'`. Read
  the `metal-gpu-intervals` table, which is exported as XML. Afterwards, check
  `ps` for leftover suspended LuminousQuake processes.
- macOS has no `timeout` command; use `perl -e 'alarm(120);exec(@ARGV)' …`.
- **Before running anything that opens windows**, tell the user and batch the
  runs. Earlier, open viewer windows interrupted their play.

## Open threads, in suggested order

1. Check the shadow and sound changes in play, then commit. Suggested split:
   the crash fix and log; sound muffling; shadows and culling.
2. Profile again: is the frame now limited by the GPU or the CPU? The menu and
   HUD layer (UI/compositing) costs about 2 ms of CPU a frame.
3. Run-time CPU/GPU balancing, an agreed principle in AGENDA.md. At 1080p the
   CPU did about 4 ms of work a frame and the GPU about 12 ms, before the
   speed-up.
4. Sound loose ends:
   - audio spikes of up to 40 ms with VSync off, and a 9 ms first-search spike;
   - muffling is volume only, with no high-frequency filter;
   - doors that start the level open get the fade backwards;
   - 35% is a starting value for the user to tune.
5. Agenda item 1, protocol 666, BSP2 and raised limits. Arcane Dimensions is
   installed in `quake-complete/Quake/ad`; 9 of its 284 maps are BSP2.
6. The rest of the agenda's Not-yet-done list:
   - HDR and blending translucent surfaces in linear light;
   - entity 72's light reach not matching the map's leaves;
   - material packs;
   - the Luminous work list;
   - the rerelease items;
   - platform testing and signing.

## Reference

- `reference/dhewm3` (git-ignored): Doom 3's renderer, the guide for culling.
  See its light scissors and static/entity shadow split
  (`r_useLightScissors`, `r_useInteractionScissors`).
- `reference/darkplaces` (git-ignored): the material-pack conventions, written
  up in [MATERIAL_PACKS.md](MATERIAL_PACKS.md).
- The user's id1 is the 2021 rerelease, relit with ericw-tools. It uses
  no-falloff spotlights (delay 3) and inverse-square lights (delay 2 and 5).
