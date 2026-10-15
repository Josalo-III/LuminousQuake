# Source provenance

## CPU profiling and worker jobs — October 14, 2026

- `platform/frame_timing.*`, `platform/jobs.*`, the SDL range-worker integration,
  profiling documentation and trace-summary tool are new LuminousQuake code.
- The worker implementation uses SDL3's public thread, mutex and condition APIs;
  no ioquake3, Doom 3, dhewm3 or Tenebrae worker code was copied.
- Row conversion changes adapt existing LuminousQuake UI and Classic GPU bridge
  code. QuakeC, gameplay state, window operations and GPU submission remain on
  their existing owners.

## Enhanced lighting foundations — October 14, 2026

- Inspected original compiler semantics in id Software's Quake-Tools revision
  `c0d1b91c74eb654365ac7755bc837e497caaca73`, `qutils/LIGHT/ENTITIES.C`,
  `qutils/LIGHT/ENTITIES.H`, `qutils/LIGHT/LTFACE.C` and `qutils/LIGHT/LIGHT.C`:
  scalar source input, default 300, static ray visibility, softened surface
  angle, range scaling and 8-bit output clamping. No compiler implementation
  files were imported.
- Scene light descriptions, style evaluation and normal propagation are local
  GPL-2.0-or-later adaptations of the existing renderer/Quake loader boundary.
  No Doom 3/dhewm3 or Tenebrae implementation has been imported in this slice.
- Runtime point-light selection, GLSL direct lighting and rerelease `_color`
  handling are new LuminousQuake code. The existing owned shader-generation
  pipeline produced the checked-in SPIR-V/MSL arrays; no donor shader was copied.
- October 15, 2026: falloff formulas and spotlight keys (`delay`, `wait`,
  `mangle`, `angle`, `_softangle`, `_deviance`, `_anglescale`, worldspawn
  `_range`/`_dist`) follow the documented entity semantics of ericw-tools'
  `light` compiler, checked against the keys present in local E1M3; they are
  reimplemented from documented behavior, and no ericw-tools code was copied.
  Cube shadow maps, back-face casting and PCSS penumbrae (Fernando, 2005) are
  standard published techniques written as new LuminousQuake code.

## Rerelease QuakeC comparison — October 14, 2026

- Inspected donor/reference: `https://github.com/id-Software/quake-rerelease-qc`,
  revision `634eefab09a77eb7b5f5ca7078ba3d8784a91142`; all six manifest-listed
  source sets. Repository README identifies GPLv2; source headers retain id's
  copyright and GPL notices. Reference-only checkout is in
  `/private/tmp/lq-quake-rerelease-qc`.
- Original single-player reference: `https://github.com/id-Software/Quake-Tools`,
  revision `c0d1b91c74eb654365ac7755bc837e497caaca73`, `qcc/v101qc`.
  Reference-only checkout is in `/private/tmp/lq-quake-tools`.
- No donor implementation files were copied into the engine, built, or used
  to overwrite installed game code. The read-only inventory script is original
  LuminousQuake tooling. See [comparison report](docs/RERELEASE_COMPARISON.md)
  for engine/API findings, installed-program metadata and scope limits.

## Initial engine

- Source: https://github.com/id-Software/Quake
- Upstream revision: `bf4ac424ce754894ac8f1dae6a3981954bc9852d`
- Imported by cloning the complete repository; original history and file notices retained.
- License: original GPL release; source notices specify GPL-2.0-or-later. See `gnu.txt` and `readme.txt`.
- Build selection: portable NetQuake C code under `WinQuake/`; QuakeWorld remains separate.

## Phase 1 adaptations

New `platform/` adapters and diagnostic code, CMake/test infrastructure, and the source fixes described in `docs/PHASE_1.md` were developed for LuminousQuake. New C code and fixture code use GPL-2.0-or-later. No ioquake3, Doom 3, dhewm3, FTE, or Tenebrae implementation files have been copied into this phase.

Before a future donor import, record its repository URL, exact revision, original paths, file licenses, local modifications, and validation evidence here. Confirm license compatibility of the selected files and preserve their notices.

## Phase 2 adaptations

- Donor: https://github.com/ioquake/ioq3
- Inspected revision: `87a99d18206e878a20289f05ac3c16005dcae5c3`.
- Original paths: `code/sdl/sdl_input.c`, `code/sdl/sdl_snd.c`.
- `platform/in_sdl.c` adapts SDL key translation, physical number-row bindings,
  capture/focus handling and separate text events to Quake's `Key_Event`,
  `Key_CharEvent`, commands and `IN_Move`. Q3 key IDs, event queue and VM APIs
  are not imported.
- `platform/snd_sdl.c` adapts the SDL callback/ring-output approach to Quake's
  existing `dma_t` and C mixer. The callback only consumes prepared samples;
  main-thread painting is locked and callback progress uses an absolute frame
  counter. SDL2 device APIs, focus handling and shutdown are local adaptations.
- Both donor files specify GPL-2.0-or-later. Their original copyright/license
  header is retained in the adapted files. SDL itself is an external dependency
  under its zlib license; its source is not vendored.
- `platform/vid_sdl.c`, `platform/demo_sdl.c`, the SDL interface, diagnostic
  additions, smoke script and renderer/native fixes are LuminousQuake work
  under GPL-2.0-or-later. SDL presents Quake's software framebuffer; this does
  not import the Q3 renderer.

Historical references inspected: Fruit of the Dojo Quake revision
`0cf735b` and TenebraeQuake revision `21844be` (September 21, 2013), particularly
software-framebuffer presentation, input focus handling and audio output.
No implementation from these repositories was copied into Phase 2.
Doom 3/dhewm3 rendering imports remain future work.

## Rerelease metadata compatibility

The `campaign` variable behavior was checked against id Software's
[rerelease QuakeC StartFrame](https://github.com/id-Software/quake-rerelease-qc/blob/main/quakec/world.qc):
game code reads it on the first frame and mirrors its saveable global back on
subsequent frames. LuminousQuake registers a non-archived variable with default
0. No QuakeC source or assets were imported for this correction.

Rerelease achievement message 52 was checked against `quakec/defs.qc` and
`quakec/triggers.qc` at official revision
`634eefab09a77eb7b5f5ca7078ba3d8784a91142`. A secret emits the command byte
followed by a NUL-terminated identifier. The client consumes that metadata;
no online achievement service is implemented. Packet alignment/truncation
and a temporary content-derived secret-trigger reproduction were validated.

## Phase 3 GPU foundation

The playable window/input/audio adapters migrated from SDL2 to SDL3 on October
14. SDL3's official `docs/README-migration.md` was used as the API reference.
Quake's mixer and the ioquake3-derived input/ring design remain; audio callback
submission now uses SDL3 audio streams and stream locks. No additional upstream
implementation source was copied for this migration.

- SDL3 is an external zlib-licensed dependency. The first local SDK was built
  from official tag `release-3.4.18`, commit
  `829a65d769d935c4852f8159e964312c0957260a`; the durable Homebrew arm64 SDK
  is 3.4.16. Source is not vendored. GPU builds copy the runtime library and
  its license beside the executable when the package supplies that license.
- The selected API is documented at https://wiki.libsdl.org/SDL3/CategoryGPU.
  Backend code uses SDL's GPU API; no SDL implementation source is imported.
- GLQuake reference is already present at the original Quake revision above:
  `WinQuake/gl_rsurf.c` (`BuildSurfaceDisplayList`, lightmaps), `gl_warp.c`
  (water and two-layer sky), and `gl_rmain.c` (future model integration).
  Geometry/UV conventions and sky/water algorithms are adapted into the owned
  scene adapter/shader, with id Software attribution retained. Legacy OpenGL
  calls/display lists are not compiled into the SDL3 GPU backend.
- Classic light-style preset strings correspond to original Quake world code;
  the scene retains four independent per-surface lightmap channels/styles.
- `renderer/`, GPU viewer and tests are GPL-2.0-or-later. The backend receives
  no Quake/QuakeC types or asset-allocator handles. All shader source is in
  `renderer/shaders`; `tools/build_shaders.py` generates embedded SPIR-V/MSL.
  Local tools: glslang 16.6.0, SPIRV-Cross 1.4.357.0, and spirv-val for validation.
  These tools are build dependencies only; their implementations are not vendored.
- No game data, generated retail-derived maps, or screenshots are tracked.
