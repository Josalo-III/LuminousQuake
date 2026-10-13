# Phase 2: SDL platform client

Implemented October 13, 2026. Local validation uses native arm64 Apple Clang,
SDL2 2.32.10 from Homebrew, and the M1 Max Mac. Windows/Linux builds are
configured in CI but have not run against a project-owned remote.

## Implementation

- Three executables: SDL `luminousquake`, null-backend `luminousquake-headless`,
  and `luminousquake-server`. `LQ_BUILD_CLIENT=OFF` removes the SDL dependency.
- Quake's indexed software framebuffer is converted using its palette and
  presented through SDL2. SDL selected its Metal presentation driver locally;
  Quake's actual 3D rendering remains on the CPU. There is no modern lighting
  backend yet. Nearest sampling and aspect-preserving letterboxing are retained.
- Independent window/render dimensions, HiDPI window support, desktop fullscreen,
  configurable vsync, framebuffer/cache recreation, palette effects and disc icon.
- ioquake3-derived SDL key translation/capture handling adapted to Quake: separate
  text events, physical number-row bindings, relative mouse movement, existing
  mouse sensitivity/filter/strafe/mlook math, held-modifier counts and key releases
  on focus loss. Capture is released for menus, console, pause and loss of focus.
- Minimal default bindings support packs that omit `default.cfg`; normal
  `quake.rc`/configuration commands override them. Quake console text remains
  limited to its printable ASCII character set.
- Quake's original mixer and attenuation feed signed 16-bit stereo SDL output.
  The callback consumes a locked ring and clears consumed samples. An absolute
  callback-frame counter avoids missing multiple ring wraps. Focus loss pauses
  audio and clears it. Device initialization failures are nonfatal. Audio can be
  shut down/reopened; callback resources are released before their storage.
- Asset-free software scene, generated tone and focused platform tests. The
  scene exercises presentation/input; it is not a Quake BSP-renderer test.
- Rerelease achievement message 52 consumes its NUL-terminated identifier and
  emits a developer diagnostic, without requiring an achievement service. Unknown
  messages still fail with the command, byte offset and connection/demo source;
  network tracing bounds-checks its message-name lookup.
- Bounded content runs and BMP capture options; an opt-in content smoke script
  isolates all generated files from the supplied installation.

## Native fixes exposed by these paths

Surface-cache blocks now align to native pointer-containing headers. Sound
resampling avoids signed-negative shifts; sound initialization handles a failed
backend before dereferencing it. Portable Windows builds bypass DirectSound.
Separate text input avoids entering each character twice.

Content rendering exposed old undefined behavior: texinfo used out-of-range
row indexing; missing visibility data involved null-pointer arithmetic;
item/light masks shifted into the signed bit; texture/sprite minima and console rows
used negative shifts. Surface light interpolation now subtracts signed values
before shifting, rather than shifting wrapped unsigned differences. Extreme
edge/alias depth gradients and alias depth accumulation clamp before integer conversion; the edge sentinel
uses `INT_MAX`. These are scoped native corrections, not a complete parser or
renderer audit.

## Validation

- Native Debug, Release, and ASan/UBSan builds: 12 CTest checks each, including
  loopback, all three targets' diagnostics/generated fixtures, SDL dummy-driver
  integration, and unavailable-audio fallback.
- SDL diagnostics on the real Mac drivers: relative mouse capture and Quake
  mouse-delta math, held movement release on simulated focus loss, modifier/text
  handling, actual window resize, palette pixel readback, video restart, aligned
  surface allocation, mixer output, audio wrap/restart/callback advance and quit.
  `--test-fullscreen` additionally enters/exits actual desktop fullscreen.
  Focus/key events are injected SDL events; this is not a manual keyboard/mouse
  usability assessment. Computer-use inspection of the temporary app wrapper
  timed out, so OS-level manual interaction remains a follow-up.
- Supplied local `id1/pak0.pak`: PACK format, 1071 entries, about 171 MiB.
  Start and E1M1 were loaded and visually inspected from framebuffer captures.
  The bounded E1M1 sequence exercised movement and attack commands, sound asset
  loading, save/load and ordinary demo recording/playback. No sanitizer reports
  occurred in the final checked runs. The test does not compare these behaviors
  to an original executable, or establish complete gameplay correctness.
- A temporary E1M1-derived map placed the player inside a secret trigger. It
  reproduced the reported disconnect on command 52 before the correction and
  reported `ACH_FIND_SECRET` without disconnect or sanitizer reports afterward.
  No derived map or original game data is included in source control. Native
  diagnostics also check a following original status message stays aligned and
  a truncated achievement identifier fails cleanly.
- Asset-free dummy drivers cannot implement OS mouse capture; the test explicitly
  reports that skip and still verifies the same Quake mouse-delta math.

The supplied archive has **unverified provenance and content**. Original disc
data is still inaccessible in storage. It contains later content and game-code
references to rerelease metadata and fog fields. The `campaign` cvar is now
registered with default 0, allowing QuakeC's per-frame save-metadata sync without
console spam. It is not archived in config.cfg; QuakeC saves retain ownership of
its saved value. This does not implement rerelease campaign menus or fog; other
extension warnings remain visible. This is a compatibility sample, not an authenticated
1996 baseline. Other bundled mods/mission packs have not been validated.
`quake-complete/` is ignored by Git; no game data is tracked or copied into source.

## Remaining work

- Manual input/audio usability, physical focus changes, original frame/input
  scheduling comparisons and a fuller original-data gameplay baseline.
- Windows/Linux compile and runtime evidence, real multiplayer, additional maps,
  water/sky/effects, complete save/demo compatibility and long-duration audio.
- CD/music playback and a modern settings UI; the old menu remains present.
- General content-parser hardening and a broader legacy renderer arithmetic audit.
- Phase 3 renderer/resource boundary, then buffered graphics and runtime lighting.

SDL presentation retains Quake's original `Host_Frame` timing gate. Vsync is
optional; this phase does not redesign simulation pacing or establish performance
improvements. See README for build, launch, controls and diagnostic commands.
