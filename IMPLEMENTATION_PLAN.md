# LuminousQuake implementation plan

Status: Classic GPU feature implementation checkpoint reached, October 14, 2026.
Phases 1–2 are implemented. Classic GPU now submits world, models, moving
brushes, sprites and particles to the GPU, alongside the Software comparison
mode. A Finder-launchable Mac app and isolated profile writes are implemented.
See [Classic GPU checkpoint](docs/CLASSIC_GPU_CHECKPOINT.md). General CPU jobs,
broader content/platform validation, remaining Phase 3A delivery checks and
Enhanced rendering remain pending.

Enhanced development has begun with shared world/model normals and an owned
BSP light inventory; see [Enhanced foundations](docs/ENHANCED_RENDERER.md).
The standalone viewer now renders them as falloff-correct, soft-shadowed direct
light; Enhanced mode in the game remains disabled.

Classic GPU texture filtering now includes saved mipmap and anisotropy options.
See the [texture-filtering checkpoint](docs/CLASSIC_GPU_CHECKPOINT.md) for the
palette-aware approach, validation and limitations. Rerelease fog and broader
campaign/platform validation remain separate follow-up work.

Immediate compatibility priority: resilience when users select rerelease data;
see [rerelease QuakeC comparison](docs/RERELEASE_COMPARISON.md).
The installed program requires numeric builtins 79/80 which are not implemented;
short gameplay smoke passes do not establish complete rerelease compatibility.
Parameterized printing/gib fixes are implemented with generated regression
coverage; broader gameplay validation remains pending. Implement and test
ending/local-sound services and add a load-time capability audit before claiming campaign support
or rebuilding installed game code from the newer sources.

Updated: October 14, 2026

## Purpose

Build a cross-platform engine that plays the original 1996 Quake using unchanged original PAKs while providing optional dynamic lighting, shadows and improved rendering. Deliver a binary that can live beside classic Quake binaries in an existing installation. Preserve Quake's gameplay, proportions, and atmosphere, including the original choices about what lighting reveals and conceals.

Enhanced game data is a separate project. This repository builds the engine and supports optional asset overlays; it does not require or produce texture upscales, replacement geometry or modified retail PAKs. Software, Classic GPU and Enhanced must all work with original assets alone. Enhanced rendering must provide useful improvements without a replacement-asset pack.

The original campaign is the central development and validation target. Widely distributed rerelease data
is an expected compatibility case: recognize its requirements, support the
bounded engine services needed to play supported content, and report unsupported
requirements clearly before entering gameplay where they can be detected at
load time. Do not defer known incompatibilities until combat or an ending crashes.
Rerelease visual improvements are within scope wherever they improve the look
of the game, just as useful techniques from ioquake3 and Doom 3 are candidates.
Evaluate their engine and asset requirements individually, preserve the Classic
appearance option, and keep any enhanced-data production in the separate asset
project. Visual support is not contingent on adopting rerelease gameplay.

This does not commit the project to all rerelease campaigns, bots, multiplayer
modes or gameplay changes. Never replace the user's game code or assets to make
an installation fit the engine, and advertise only implemented extensions.

Preserve existing classic multiplayer behavior as engine changes are made.
Playing rerelease data locally and joining rerelease servers are separate
compatibility questions. Rerelease server interoperability remains unverified
and requires its own investigation of protocol, gameplay and service requirements
before a support commitment. Do not assume that loading its QuakeC is sufficient,
or change original-campaign behavior merely to pursue server compatibility.

The repository begins with id Software's Quake source. Modernize it incrementally rather than adopting Tenebrae as the engine foundation. Prefer ioquake3 implementations where they provide equivalent functionality; selectively adapt Doom 3/dhewm3 rendering techniques. Evaluate Tenebrae solutions individually when they remain useful.

## Sources and their roles

| Source | Role |
| --- | --- |
| [id Software Quake](https://github.com/id-Software/Quake) | Original behavior, game interfaces, asset formats, initial engine, and GLQuake rendering reference |
| [ioquake3](https://github.com/ioquake/ioq3) | Build and platform infrastructure, input, utilities, renderer boundaries, and selected performance improvements |
| [id Software Doom 3](https://github.com/id-Software/DOOM-3) | Lighting interactions, materials, shadow generation, and renderer organization |
| [dhewm3](https://github.com/dhewm/dhewm3) | Maintained adaptations of Doom 3, portable audio/platform examples, and rendering fixes |
| Tenebrae CVS archive | Historical reference and candidate algorithms; no default dependency |
| [id Software Quake tools](https://github.com/id-Software/Quake-Tools) | Original lighting compiler and asset-format interpretation |
| [FTE/FTEQCC](https://github.com/fte-team/fteqw) | Candidate tooling and QuakeC extension reference if later needed |

For every imported component, record the upstream commit, original paths, local adaptations, and license/attribution requirements. Check compatibility of the actual files before integrating them. Prefer bounded imports over bringing entire donor engines into the build.

## Constraints and working assumptions

- Support macOS, Windows, and Linux from the beginning. Native Apple Silicon is the first locally tested target; automated builds on other systems do not establish runtime correctness.
- Use the available M1 Max hardware for development: native arm64 dependencies, parallel builds, and substantial working sets where useful.
- Retain original Quake content support. Replacement assets are optional overlays, not a requirement for playing the game.
- Install alongside existing binaries without replacing them or rewriting original PAKs. Discover the installation independently of the process working directory; keep `-basedir` as an explicit override.
- The Mac release is a native arm64 `LuminousQuake.app`, double-clickable from Finder beside `id1`. Find existing PAKs relative to the app, or offer a folder picker when they cannot be found. No Terminal command is required for normal launch.
- Keep LuminousQuake settings, saves and recorded demos in its own user-data location so testing does not overwrite classic-engine files. Read game/mod assets from the selected installation and retain original file formats. Provide explicit migration/import rather than silently modifying existing settings or saves.
- An unverified local Quake archive is available for smoke tests. Original disc data remains inaccessible in storage. Generated fixtures and synthetic scenes remain the reproducible test baseline.
- GtkRadiant has already been ported to Apple Silicon and is available for asset work. Validate its Quake 1 game configuration and compiler workflow separately from the engine.
- Do not assume compiled retail BSPs include editable map sources. New authored geometry and lighting overrides need their own reproducible workflow.
- Preserve the original QuakeC `progs.dat` interface. The repository's `qw-qc` directory is QuakeWorld game code; it is not an interchangeable replacement for original single-player game code.
- Prefer a shared renderer across platforms. A platform-specific acceleration path must have an equivalent supported path elsewhere.

## Architecture rules

1. Give each service one owner: filesystem, memory allocation, commands/configuration, timing, and entity state.
2. Adapt donor components at explicit boundaries. Similar names and common ancestry do not establish equivalent behavior.
3. Keep gameplay entities distinct from renderer objects. Translate simulation state into scene submissions rather than exposing interpreter memory to the renderer.
4. Preserve original file encodings and VM value widths while making native code safe for 64-bit addresses and alignment.
5. Preserve simulation behavior when improving clocks, input delivery, and presentation pacing.
6. Preserve three renderer choices: Software, Classic GPU with original baked-lighting appearance, and Enhanced with runtime lighting/material techniques. Classic GPU describes appearance and can use Metal/Vulkan. All three share gameplay and original-content support; unavailable choices must be clearly identified.
7. Measure performance before adopting optimizations. Record correctness and scene-level results alongside timing.
8. Use a bounded worker pool for independent engine work. Keep QuakeC, gameplay,
   network state and legacy allocators under their existing owner; workers receive
   owned snapshots and write to disjoint outputs. Preserve a single-thread fallback.

## Phase 0: inventory and compatibility baseline

### Work

- Identify the original NetQuake/GLQuake client and dedicated-server sources used by the initial build. Keep QuakeWorld distinct.
- Inventory platform dependencies, assembly paths, memory/pointer assumptions, asset loaders, and renderer/global-state coupling.
- Record source revisions for each candidate donor and document decisions in the repository.
- Define a small set of compatibility scenarios: movement, collision, triggers, monsters, weapon behavior, light styles, saves, and demos.
- Establish diagnostics for asset loading, frame timing, errors, and resource ownership.
- Create generated format fixtures and a synthetic rendering scene; keep proprietary game data outside the repository.

### Exit criteria

- The build scope and subsystem boundaries are documented.
- Initial fixtures are reproducible and do not depend on retail assets.
- Original-game comparisons are explicitly marked pending until data is available.

## Phase 1: portable native build and correctness fixes

Implementation: see [Phase 1 notes](docs/PHASE_1.md) and [build instructions](README.md). The initial client target is a headless bootstrap; SDL window/input and audio remain Phase 2.

### Work

- Add a CMake build for the selected client and dedicated-server targets, following useful ioquake3 conventions.
- Build native arm64 on macOS and establish Windows/Linux build jobs in CI when a hosted remote is available.
- Replace necessary x86 assembly paths with portable implementations; retain optimized alternatives only where validated.
- Fix pointer truncation, allocation/alignment errors, incorrect format specifiers, and compiler incompatibilities required for the build.
- Preserve 32-bit VM cells, file-format widths, and serialization conventions. Audit conversions rather than widening all values globally.
- Introduce useful compiler warnings and debug sanitizer configurations where supported. Record deferred findings.

### Exit criteria

- Native macOS targets build reproducibly.
- Startup and headless diagnostics can run with explicit, understandable outcomes when game data is absent.
- CI configuration covers Windows/Linux; actual passing jobs are required before claiming those builds work.
- Focused tests cover corrected binary-layout, allocation, and conversion problems.

## Phase 2: platform, window, input, and basic audio output

Implemented; see [Phase 2 validation and limitations](docs/PHASE_2.md).

### Work

- Adapt ioquake3's SDL platform/window/input approach to Quake's current interfaces.
- Replace the selected legacy backends with shared handling for mouse capture, key events, text entry, focus, fullscreen, resizing, and display dimensions.
- Use a monotonic clock and appropriate presentation pacing without replacing Quake's simulation scheduling.
- Initially retain Quake's mixer and provide portable output, so sound does not block the baseline. Evaluate OpenAL spatialization later.
- Handle initialization, shutdown, and restart failures cleanly.

### Exit criteria

- A synthetic scene opens and responds correctly to input on macOS.
- Focus changes, resizing, fullscreen, and shutdown are exercised locally.
- Headless operation does not require a graphics context or audio device.
- With game data, mouse behavior and frame scheduling are compared against the original baseline.

## Phase 3: asset access and renderer boundary

Initial slice implemented: [SDL3 GPU world viewer](docs/PHASE_3.md), renderer-owned
geometry/textures/lightmap data, embedded SPIR-V/MSL shaders and native Metal proof.
The software client remains the playable comparison build and its shared
platform layer has migrated to SDL3. Renderer choice and map/restart lifecycle
are integrated. Explicit GPU world/actor/brush/sprite/particle submissions and
scaled UI coverage have replaced the normal mixed-rendering bridge. Map changes,
save/load, demos and renderer switching have native smoke coverage. Broader
compatibility and optional asset lookup remain open; the viewer is a separate
diagnostic tool.

### Work

- Introduce a filesystem interface retaining Quake PAK support, loose-file lookup, mod directories, and search precedence.
- Define optional replacement-asset and material lookup without changing original asset identity or texture coordinates.
- Keep original BSP, MDL, sprite, palette, and texture interpretation available behind loaders.
- Define renderer-facing world geometry, animated model instances, materials, lights, effects, camera state, and UI submissions.
- Use ioquake3's renderer interface as a design reference, extending it for persistent lights and shadow/material settings.
- Establish resource ownership and lifetimes for persistent level assets, transient frame data, and GPU objects.
- Select and document the graphics API/context and shader requirements. Prove the basic approach on Mac and ensure a supported Windows/Linux path before extensive backend work.

### Exit criteria

- The baseline renderer consumes explicit submissions through the new boundary.
- Renderer resources do not depend on QuakeC memory layout.
- Asset precedence and replacement fallback are tested with generated fixtures.
- An unchanged asset can be loaded and displayed through the boundary.

## Phase 3A: drop-in installation and Mac application delivery

This is the next implementation milestone, before expanding renderer features.

### Work

- Separate the read-only game installation from LuminousQuake's writable user data. Route configuration, saves and recorded demos through that policy while retaining PAK/loose-file/mod search precedence and original save/demo formats.
- Resolve the installation from an explicit `-basedir`, a previously selected installation, or the executable/app location. On macOS use the directory containing `LuminousQuake.app`, not `Contents/MacOS` or the shell's current directory. Support a normal `id1/pak0.pak` installation; use `pak1.pak` when present and preserve shareware behavior when it is absent.
- Add a Finder launch flow: validate discovered data, offer a native installation-folder picker if needed, remember a successful choice, and show useful errors for missing/incompatible files. Cancellation must exit cleanly. A missing installation must not strand the user at an invisible console.
- Build a CMake macOS app bundle with application metadata, an owned icon, embedded SDL3 runtime, and relative library paths. Bundle engine resources and shaders, but no proprietary game data. Preserve the current CLI executable/arguments for development and advanced use.
- Keep renderer choice accessible in the game UI. Software and Classic GPU remain usable alongside each other; Enhanced stays explicitly pending until its original-asset implementation is ready.
- Provide packaging/install instructions for a release that coexists with classic binaries. Define equivalent executable-relative discovery and separate user-data locations on Windows/Linux; retain their builds even while Mac receives the first locally validated GUI package.
- Establish signing/notarization requirements for a publicly distributed Mac build separately from local bundle testing. Track other-platform runtime validation and distribution requirements explicitly.

### Exit criteria

- Double-clicking the app beside an existing `id1` launches the game without Terminal, regardless of the inherited working directory.
- Launching elsewhere offers a folder picker and remembers the selected installation; missing data and cancellation have clear, usable outcomes.
- Generated installations verify PAK discovery, mod precedence and separate writes. Real-content checks confirm gameplay, save/load and demo use without changing installation files.
- Original PAKs and classic-engine configuration/saves remain unchanged. No retail data is included in the application or release archive.
- A packaged native arm64 app runs with its embedded SDL3 library, without requiring Homebrew on the target Mac. Software/Classic GPU selection and launch focus work in the bundle.
- Windows/Linux build/package status and untested runtime behavior are documented.

## Phase 4: buffered rendering and original-asset material support

### Work

- Move CPU rasterization and selected GLQuake algorithms to vertex/index buffers and shaders incrementally.
- Add animation interpolation, material inputs, texture upload/mipmapping, and clear color-space handling.
- Preserve sky, water, transparency, emissive palette behavior, sprites, and UI during migration.
- Provide conservative original-asset material fallbacks and optional replacement lookup without changing surface scale, texture alignment or gameplay dimensions.
- Keep texture upscaling, authored material packs, replacement geometry and their offline production/validation pipeline in the separate game-data project. Engine tests use generated overlay fixtures.
- Evaluate optional overlays for compatibility and fallback behavior; their absence must not prevent any renderer mode from running original content.
- Treat any inferred normal/specular information conservatively; painted highlights and shadows must not automatically become physical surface relief.

### Exit criteria

- Synthetic geometry and animated fixtures render correctly through buffered paths.
- Once data is available, representative original levels, models, and effects remain recognizable and operational.
- Replacement assets can be disabled and missing replacements fall back to originals.
- Record CPU/GPU timing and memory use on named scenes; make no unsupported performance claims.

## Phase 4A: CPU jobs and measured parallelism

Multithreading is an explicit engine requirement, introduced alongside the GPU
migration and before the costlier Enhanced lighting work. The first bounded
worker pool and measured conversion jobs are implemented. SDL audio also runs
through its callback path; rendering simulation and QuakeC remain serial.

### Work and order

See [profiling method](docs/PROFILING.md) for the controlled workload matrix
and the evidence required before parallelizing a stage.

1. CPU stage timings are implemented for input/commands, networking, simulation,
   client processing, scene preparation, UI/compositing, presentation/submission,
   audio and the total frame. They are opt-in through `lq_profile`; the report
   explicitly distinguishes CPU presentation/submission time from GPU execution.
   Add GPU timing where supported before making GPU performance claims.
2. A persistent, bounded SDL worker pool now provides range jobs,
   completion barriers, orderly shutdown and a single-thread mode. Avoid creating
   threads per frame. Automatic sizing reserves main/driver/OS capacity and caps
   at eight; measurements selected six workers on the ten-core M1 Max. The
   archived `lq_workers` override accepts `-1` for automatic, `0` for serial and
   1–8 for an explicit count. Keep Apple scheduling refinements optional.
3. The first pool workload covers UI downsampling/compositing and Classic GPU
   palette-to-RGBA overlay conversion in disjoint rows. Palette, input and
   coverage data remain fixed until the completion barrier, and the main thread
   participates. The serial path remains the correctness/performance reference.
   Remove temporary compositor jobs when explicit GPU UI submissions replace
   that bridge. Software presentation conversion remains a candidate only after
   its separate measurements justify the change.
4. Move CPU-side texture conversion and mip preparation into jobs using owned
   asset buffers. Workers do not call the Quake hunk/cache allocators or mutate
   model state. The render owner commits completed resources and performs GPU
   uploads. Bound queued work and memory; discard obsolete results by map/resource
   generation and drain dependent jobs before freeing their inputs.
5. After explicit render snapshots exist, parallelize visibility/draw preparation
   and, for Enhanced mode, light influence and shadow-caster preparation. Merge
   results in a stable order. Keep GPU submission and window operations on their
   designated owner initially. Move particles only when ownership is similarly
   explicit.

The duplicate full software world render has been removed from normal Classic
GPU frames. Do not reintroduce it as parallel work or parallelize the original
global-state rasterizer, QuakeC VM or
gameplay simulation without a separate compatibility and ownership design.

### Exit criteria

- Generated fixtures produce identical CPU conversion output in serial and
  parallel modes, including odd job counts, small inputs and palette changes.
- Map changes, video restart and quit drain/cancel jobs without stale accesses,
  deadlocks or resource leaks; race checking covers the isolated worker code.
- Named scenes are compared at 800×600, 1920×1080 and the supported maximum,
  recording median and tail frame times, stage costs, worker counts and memory.
  Retain each optimization only when measured benefits justify its overhead.
- Gameplay, demos, save/load, UI and original-content compatibility remain
  unchanged. Windows/Linux build coverage and actual runtime validation are
  reported separately from native arm64 results.

## Phase 5: dynamic lighting, shadows, and atmosphere

First foundation slices implemented: renderer-owned light descriptions from
retained BSP entity text and world-space normals for world/brush/MDL geometry.
See [Enhanced foundations](docs/ENHANCED_RENDERER.md). The standalone viewer now
renders direct light with compiler-compatible falloff (all LIGHT/ericw-tools
`delay` formulas, `wait`, `_range`/`_dist`), mangle/target spotlights and
reversed-depth cube shadow maps with PCSS penumbrae from an emitter radius.
A controlled six-axis occluder scene proves moving shadow boundaries, cached
static shadows, range cutoff and penumbrae by readback. Indirect light, actor
lighting/shadows and material upgrades remain pending, so the in-game Enhanced
choice remains unavailable. Classic GPU services remain shared unless a specific
Enhanced implementation supersedes them.

The static-light reconstruction approach is documented in
[Light reconstruction](docs/LIGHT_RECONSTRUCTION.md): entity light scalars and
the final BSP samples are evidence for a fitted direct/indirect model, not
separate authored power and range fields.
The first E1M3 sample/source comparison is in
[The Necropolis lighting analysis](docs/NECROPOLIS_LIGHTING_ANALYSIS.md).

### Work

- Capture map lighting descriptions before QuakeC spawn functions can discard ordinary light entities.
- Store renderer-owned light descriptions, including origin, brightness, available direction/target information, and style relationships.
- Connect switchable and animated Quake light styles to runtime illumination.
- Introduce Doom 3/dhewm3-inspired light/surface relationships, normal and specular mapping, shadow generation, and culling as separate changes.
- Select shadow techniques based on artifact behavior, cost, and portability; do not assume the historical technique is automatically best.
- Keep baked lightmaps as a comparison/reference dataset, rather than multiplying them into the new direct-lighting path and double-counting illumination.
- Reconstruct static-light parameters from the original compiler prior and BSP
  samples. Record uncertainty, retain only identifiable fits, and derive any
  ambient/residual data outside original game files.
- Calibrate illumination against original focal points, shadowed passages, silhouettes, and navigation cues. Document intentional departures.
- Provide optional, inspectable per-map lighting overrides for cases where surviving entities are insufficient.
- Establish an original-PAK-only Enhanced mode before relying on optional overrides or material packs. Keep authored enhancement data in the separate asset project.

### Exit criteria

- Moving lights and geometry produce coherent lighting and shadows in the synthetic scene.
- Tests cover light toggles/styles, occlusion, shadow edge cases, and material fallback.
- An original level runs with runtime lighting, original gameplay, and side-by-side/reference captures.
- Document remaining atmosphere mismatches and budgets before extending to more levels.

## Phase 6: audio, utilities, and measured optimization

### Work

- Evaluate ioquake3/dhewm3 OpenAL Soft adaptations against Quake's attenuation, loops, channel replacement, music, and environmental behavior.
- Modernize commands, cvars, console history, and configuration selectively. Preserve aliases, quoting, bindings, `wait`, and script ordering.
- Improve memory management where profiling or integration identifies a concrete need, preserving explicit lifetime rules.
- Optimize visibility, light culling, batching, asset preparation, and frame submission from measured bottlenecks.
- Parallelize suitable independent work while preserving simulation ordering and predictable resource ownership.

### Exit criteria

- Behavioral comparisons accompany each replacement.
- Performance results name the hardware, build configuration, scene, and enabled features.
- Improvements do not regress the compatibility scenarios or platform builds.

## QuakeC and gameplay policy

Keep the original interpreter interface, builtin behavior, physics, collision, networking, saves, and demo interpretation stable through the first rendering milestones. Fix native correctness problems as necessary, but separate those fixes from intentional behavior changes.

QuakeC is not responsible for expensive rendering work. Introduce optional, discoverable engine extensions only when scripted control is needed. Evaluate modern compiler tooling independently from extended bytecode. Replacing Q1 gameplay execution with Q3 QVM or Doom 3 scripting is outside the initial plan.

Raising entity/model/sound limits requires coordinated review of allocation, VM access, network encoding, and save/demo behavior. Do not treat larger constants as a complete implementation.

## Validation strategy

- **Before retail data:** generated loader fixtures, headless checks, synthetic lights/materials/animation, and platform lifecycle tests.
- **After retail data:** original single-player scenarios, representative maps and effects, save/load, demo playback, and multiplayer behavior as affected changes are introduced.
- **Visual checks:** repeatable cameras/captures comparing original lighting with the new path. Exact image identity is not the goal for intentional rendering changes.
- **Cross-platform checks:** automated compilation and suitable headless tests first; real input, audio, graphics, and gameplay testing when hardware/testers are available. Track untested combinations explicitly.
- **Performance checks:** fixed scenes, comparable settings, CPU and GPU timings where available, frame-time variation, and memory use.
- **Installation/package checks:** executable/app-relative discovery, explicit overrides, folder-picker cancellation/persistence, separate writable user data, unchanged installation files, and bundled runtime dependencies. Use generated PAKs for reproducible checks and local original content for gameplay validation.

Each phase should produce a reviewable change with its source provenance, validation evidence, known limitations, and updated status in this plan. Avoid expanding testing or importing unrelated systems without a concrete reason.

## Next delivery and outstanding decisions

The portable software game, SDL3 platform layer and interim Classic GPU gameplay bridge are implemented. The next delivery is Phase 3A: an engine binary usable alongside an existing Quake installation and a Finder-launchable Mac app. First establish installation discovery and independent user-data writes, then package the app and validate launch/data selection.

After that delivery, replace the mixed renderer bridge with explicit GPU actor/brush/effect/UI submissions and complete the Classic GPU baseline. Then implement Enhanced runtime lighting using original assets, with optional enhancement packs produced separately. Software remains a supported renderer. Authentic original-disc comparisons and broader platform runtime testing remain pending.

Decisions to resolve during implementation:

- Graphics API decided: SDL3 GPU, with Metal and Vulkan shader formats initially. Add DXIL generation before enabling D3D12; validate Windows/Linux runtime behavior.
- Exact donor revisions and the scope of each adaptation.
- Runtime lighting attenuation, ambient treatment, and shadow technique.
- Material metadata format and replacement-asset precedence.
- Final user-data layout, import behavior and Finder installation-selection UX.
- Mac release signing/notarization and Windows/Linux distribution packaging.
- Available Windows/Linux runtime testing and hosted CI setup.
- Whether later goals include protocol extensions, larger maps, new gameplay, or additional game-format support.

These decisions should follow small demonstrations and measured comparisons, rather than a wholesale engine merger.
