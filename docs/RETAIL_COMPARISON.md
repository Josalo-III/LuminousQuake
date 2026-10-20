# Retail data check — October 16, 2026

Until now, LuminousQuake was developed against the local `quake-complete/`
installation, whose `id1` is the rerelease (one repacked 179 MB `pak0.pak`, maps
rebaked with ericw-tools). This checkpoint repeats the data-driven checks on the
GOG "Quake – The Offering" 2.0.0.6 installer, extracted with `innoextract` into
the ignored `quake-retail/` folder. Its `id1/PAK0.PAK` (MD5 `5906e599…`) and
`PAK1.PAK` (MD5 `d76b3e56…`) are the v1.06 retail packs, which is the original
compatibility baseline. The packs ship in uppercase, which is how uppercase pack
discovery came to be added.

## Results

| Check | Retail result |
| --- | --- |
| `ctest` (synthetic fixtures; data-independent) | 21/21 pass |
| `tests/retail_smoke.py` (record, save/load, playback, capture) | Pass |
| `timedemo demo1` under Software, Classic GPU and Luminous | 969 frames each, no errors |
| Scene import, all 32 campaign maps (`start`, E1–E4, `end`) | All load; 0 unresolved lights |
| Fluorescent fixtures, accent spotlights | No rerelease-only dependency; see below |

Software renderer captures at the spawn point match the Classic GPU viewer on
both installs (for example, `e1m1` retail 26.1 vs 25.7 mean luma, `e4m4` 11.9 vs
11.9). Two exceptions are measurement artifacts, not renderer differences:

- `e3m1`: the corridor ahead is lit by a style-10 `light_fluorospark`, and a
  capture can land on a dark beat. Sampled over 0.3–2.9 s, the alcove reads 6.8
  or 33.3.
- `e1m8`: the viewer spawns at the first `info_player_start` in entity order and
  the game spawns elsewhere, so the views differ.

## Entity data differs; parsing holds

Retail light entities use only `light`, `angle`, `style`, `target`, `targetname`
and `spawnflags`. Across the same maps, the rerelease adds 20–90% more lights
(e2m6: 238 → 461) and the keys `_color` (2,421 lights), `delay`/`wait`, `mangle`,
`_surface`, `_sun` and `_project_texture`. It also adds worldspawn `_bounce 16`,
`_bouncescale .5`, `_dirt`, `_sunlight` and `fog`. When a key is absent, the
scene loader falls back to the original `LIGHT` defaults (300, linear, white,
range scale 0.5), so retail maps import cleanly.

- `fluorescent.c` treats a plain light within 24 units of a tube as part of its
  fixture. The rerelease places such beam spotlights; retail has none, so retail
  tubes flicker on their own.
- `LQ_LightIsAccent` applies only to spotlights with non-linear falloff
  (`delay` 2/3). Retail's target-aimed spotlights are linear and keep full gain.
- Retail has no sun, fog or `.lit` files. The rerelease's `dm*.lit` and extra
  maps (`dm7`, `dm8`, test maps) are absent.

## Calibration: Luminous does not track the install's darkness

Luminous derives light from entities rather than from the bake. Its output is
therefore nearly the same on both installs, while the two Classic references
differ: the rerelease bake, with bounce and dirt, is brighter than the 1996 one.
Mean luma (and percentage of near-black pixels) at the spawn point:

| Map | Retail Classic | Retail Luminous | Rerelease Classic | Rerelease Luminous |
| --- | ---: | ---: | ---: | ---: |
| e2m3 | 10.6 (42.7%) | 12.9 (30.0%) | 14.2 (33.4%) | 12.2 (35.4%) |
| e2m4 | 14.8 (16.4%) | 16.6 (12.2%) | 25.0 (6.1%) | 15.8 (14.5%) |
| e2m7 | 28.2 (17.1%) | 35.8 (8.6%) | 30.6 (26.0%) | 32.6 (10.1%) |
| e3m5 | 9.9 (46.4%) | 11.7 (31.8%) | 14.4 (25.2%) | 11.2 (36.8%) |
| e4m4 | 10.9 (72.0%) | 13.8 (63.5%) | 30.5 (20.1%) | 26.8 (45.5%) |

Across the 32 maps, Luminous/Classic averages 1.17 on retail and 0.95 on
the rerelease. Bounce causes the retail lift. Retail worldspawns have no
`_bouncescale`, so `bounce_scale` defaults to 1. That is twice the rerelease's
`.5`, and the first bounce layer measures roughly twice as bright (mean 5.0 vs
2.7). With bounce disabled, retail Luminous matches retail Classic closely:

| Map | Classic | Luminous | Bounce gain 0.5 | No bounce |
| --- | ---: | ---: | ---: | ---: |
| e1m4 | 13.6 (40.8%) | 17.8 (33.2%) | 15.7 (38.8%) | 12.4 (47.2%) |
| e2m3 | 10.6 (42.7%) | 12.9 (30.0%) | 12.0 (33.8%) | 10.5 (42.9%) |
| e2m7 | 28.2 (17.1%) | 35.8 (8.6%) | 33.7 (9.5%) | 29.1 (13.9%) |
| e3m5 | 9.9 (46.4%) | 11.7 (31.8%) | 11.0 (37.7%) | 10.0 (46.1%) |
| e4m3 | 10.7 (56.6%) | 12.6 (46.3%) | 11.8 (50.6%) | 10.7 (55.1%) |

The original `LIGHT` tool never bounced light. The darkness of the retail
campaign, the primary target, was authored without indirect light, and Luminous
bounce filled those spaces at the strength tuned against the rerelease's ericw
bake.

## Resolution: iterated bounce, quarter scale without a baked bounce

Luminous bounce was a single gather. It now repeats the gather, reflecting each
bounce off the texture color and scaling it by `_bouncescale`, until a bounce
adds under 1% of the first or the map's `_bounce` count is reached. That is
ericw-tools' model; id maps converge in three or four bounces. On its own the
change barely moves the spawn views (retail ratio 1.17 → 1.19, rerelease 0.95 →
0.96), because each added bounce is reduced by color and scale. Retail still
lifted authored darkness (mean near-black 31.9% → 25.3%; e2m3 43% → 29%, e3m5
46% → 30%).

A map with neither `_bounce` nor `_bouncescale` was baked without radiosity, so
it now bounces at 0.25 per reflection. Rerelease maps keep their own `.5`. Over
all 32 retail maps:

| Retail, 32 maps | Luminous / Classic | Near-black, Classic | Near-black, Luminous |
| --- | ---: | ---: | ---: |
| Single bounce, scale 1 | 1.17 | 31.9% | 26.1% |
| Iterated, scale 1 | 1.19 | 31.9% | 25.3% |
| Iterated, scale 0.25 | 1.06 | 31.9% | 30.4% |

At 0.25, most maps fall within 10% of Classic. e2m7 (1.16) is the brightest
genuine outlier. e3m1 (1.86) is the flicker-timing artifact described above.
e1m5 (0.90) and e1m8 (0.85) now render slightly darker than the original bake.

The [Necropolis analysis](NECROPOLIS_LIGHTING_ANALYSIS.md) used the rerelease
E1M3 (289 lights, ericw spotlights). Retail E1M3 has 227 lights and no
`delay`, `mangle` or `_color` keys, so those fit results do not transfer to it
directly.

## Reproduce

```sh
innoextract -d quake-retail setup_quake_the_offering_2.0.0.6.exe
python3 tests/retail_smoke.py ./build/luminousquake "$PWD/quake-retail"
./build/luminousquake-gpu -basedir "$PWD/quake-retail" -map e2m3 --luminous --bounce-scale 1
```
