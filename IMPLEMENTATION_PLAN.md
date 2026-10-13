# LuminousQuake implementation plan

Status: proposed implementation sequence  
Date: October 13, 2026

## Purpose

Build a cross-platform engine that plays the original 1996 Quake while providing dynamic lighting, shadows, improved materials, and enhanced assets. Preserve Quake's gameplay, proportions, and atmosphere, including the original choices about what lighting reveals and conceals.

The repository begins with id Software's Quake source. Modernize it incrementally rather than adopting Tenebrae as the engine foundation. Prefer ioquake3 implementations where they provide equivalent functionality; selectively adapt Doom 3/dhewm3 rendering techniques. Evaluate Tenebrae solutions individually when they remain useful.

## Sources and their roles

| Source | Role |
| --- | --- |
| [id Software Quake](https://github.com/id-Software/Quake) | Original behavior, game interfaces, asset formats, and initial engine |
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
- Original retail data will become available after retrieval from storage. Initial development uses synthetic scenes and generated fixtures.
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
6. Maintain a basic original-lighting rendering path long enough to compare new rendering against a recognizable baseline.
7. Measure performance before adopting optimizations. Record correctness and scene-level results alongside timing.

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

## Phase 4: buffered rendering and enhanced textures

### Work

- Replace immediate-mode drawing incrementally with vertex/index buffers and shaders.
- Add animation interpolation, material inputs, texture upload/mipmapping, and clear color-space handling.
- Preserve sky, water, transparency, emissive palette behavior, sprites, and UI during migration.
- Support optional upscaled textures without changing surface scale, texture alignment, or gameplay dimensions.
- Create a reproducible offline enhancement pipeline with source hashes and retained originals.
- Evaluate upscales tiled and in-engine. Compare palette, large forms, contrast, seams, and a downsampled version against the original.
- Treat inferred normal/specular information separately from texture upscaling; painted highlights and shadows must not automatically become physical surface relief.

### Exit criteria

- Synthetic geometry and animated fixtures render correctly through buffered paths.
- Once data is available, representative original levels, models, and effects remain recognizable and operational.
- Replacement assets can be disabled and missing replacements fall back to originals.
- Record CPU/GPU timing and memory use on named scenes; make no unsupported performance claims.

## Phase 5: dynamic lighting, shadows, and atmosphere

### Work

- Capture map lighting descriptions before QuakeC spawn functions can discard ordinary light entities.
- Store renderer-owned light descriptions, including origin, brightness, available direction/target information, and style relationships.
- Connect switchable and animated Quake light styles to runtime illumination.
- Introduce Doom 3/dhewm3-inspired light/surface relationships, normal and specular mapping, shadow generation, and culling as separate changes.
- Select shadow techniques based on artifact behavior, cost, and portability; do not assume the historical technique is automatically best.
- Keep baked lightmaps as a comparison/reference dataset, rather than multiplying them into the new direct-lighting path and double-counting illumination.
- Calibrate illumination against original focal points, shadowed passages, silhouettes, and navigation cues. Document intentional departures.
- Provide optional, inspectable per-map lighting overrides for cases where surviving entities are insufficient.

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

Each phase should produce a reviewable change with its source provenance, validation evidence, known limitations, and updated status in this plan. Avoid expanding testing or importing unrelated systems without a concrete reason.

## First delivery and outstanding decisions

The first delivery comprises Phases 0–2: a reproducible native build, portable platform foundation, basic sound output, and a synthetic rendering/input scene. It is not yet a validated playable engine without original data.

Once retail data is available, establish a playable compatibility baseline before removing the old rendering behavior. Then implement the renderer boundary, enhanced assets, and runtime lighting in sequence.

Decisions to resolve during implementation:

- Graphics backend/context and shader baseline across the three target platforms.
- Exact donor revisions and the scope of each adaptation.
- Runtime lighting attenuation, ambient treatment, and shadow technique.
- Material metadata format and replacement-asset precedence.
- Available Windows/Linux runtime testing and hosted CI setup.
- Whether later goals include protocol extensions, larger maps, new gameplay, or additional game-format support.

These decisions should follow small demonstrations and measured comparisons, rather than a wholesale engine merger.
