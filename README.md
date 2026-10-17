# LuminousQuake

A cross-platform Quake engine project exploring dynamic lighting, shadows,
improved materials, and faithful asset improvements. See the
[implementation plan](IMPLEMENTATION_PLAN.md).

## Classic GPU feature implementation checkpoint

The Classic GPU feature implementation milestone is reached as of October 14,
2026, including mipmaps, anisotropic filtering and saved video options. Broader
campaign/platform validation and rerelease fog remain open. The Phase 4A engine
worker pool now handles measured row conversion work and Luminous bounce
gathering; Luminous rendering is experimental.

Luminous (`lq_renderer 2`) is now an **experimental** playable choice. It lights
the world only from runtime lights:
- original LIGHT/ericw-tools falloff formulas, spotlights and live styles;
- cached cube shadow maps with emitter-sized soft penumbrae;
- static-world bounce gathered at map load from those same lights;
- weapon light: moving rockets, muzzle flashes and explosions, plus light
  distributed along lightning beams and shared lights for spark collections;
- an irradiance probe grid that lights monsters and the view weapon from those
  lights plus bounce, and carries weapon light onto nearby surfaces each frame.

Luminous particles vary in shape and scale; blood, smoke and bubbles receive scene
lighting, while sparks and fire remain self-lit. Classic GPU is the lighting
comparison reference. Darkness comes from falloff and occlusion, never from the
baked lightmap.
Monsters, items and moving doors cast Luminous shadows from their live positions.
The standalone viewer (`luminousquake-gpu --luminous`) has tuning flags. See
[Luminous renderer notes](docs/LUMINOUS_RENDERER.md) for commands, evidence and
next steps.

See [profiling method](docs/PROFILING.md) for the Instruments-led measurement
method. CPU timing is opt-in: set `lq_profile 1`, then use `lq_profile_report` in the
console. A timed command-line run can append `--profile-report`, for example
`+lq_profile 1 +map e1m1 --run-seconds 20 --profile-warmup-seconds 5 --profile-report`. The report measures
CPU stages only; GPU execution is not inferred from command submission.

`lq_workers -1` selects the measured automatic worker count, `lq_workers 0`
forces the serial reference path, and values from 1 through 8 select an explicit
pool size. The setting is archived and takes effect on the next range job. The
pool currently accelerates high-resolution UI scaling/compositing and the
Classic GPU palette-overlay conversion; gameplay and GPU submission remain on
their owning thread.

The playable `luminousquake` executable supports **Software** and **Classic GPU**
through Options. Classic GPU draws the world, animated models, moving brushes,
sprites and particles on Metal/Vulkan, with original baked lighting, animated
textures and Classic dynamic lights. Normal Classic GPU frames skip software
3D rasterization. The original palette UI is drawn into a scaled canvas and
submitted with explicit transparency coverage. Exact GLQuake equivalence and
full-campaign compatibility remain unverified.

From the repository root:

```sh
./build/luminousquake -basedir "$PWD/quake-complete/Quake" +lq_renderer 1 +map e1m1
```

Use `+lq_renderer 0` for Software. Options saves the choice for the next launch;
the console command `vid_restart` also applies a change in the current game.
**Luminous** (`+lq_renderer 2`) is experimental; see above. Legacy debug
views use a software frame; failed GPU initialization/map uploads fall back to
Software with a console explanation. See the current
[Classic GPU checkpoint](docs/CLASSIC_GPU_CHECKPOINT.md).

## GPU world viewer: first Phase 3 milestone

The experimental `luminousquake-gpu` target draws BSP triangles and palette-indexed
textures/lightmaps through SDL3 GPU. It currently provides a free-flight world
viewer; actors, moving brush models, effects, UI and gameplay integration remain
next steps. The playable `luminousquake` target uses SDL3 with software rendering.
See [Phase 3 scope and validation](docs/PHASE_3.md).

On Apple Silicon:

```sh
brew install sdl3
cmake -S . -B build-gpu -DCMAKE_BUILD_TYPE=Debug -DLQ_BUILD_GPU=ON -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/sdl3
cmake --build build-gpu --parallel 10
./build-gpu/luminousquake-gpu -basedir "$PWD/quake-complete/Quake" -map e1m1
```

WASD flies; E/Q or Space/Control moves vertically; Shift moves faster. Mouse/trackpad
movement looks around on both axes. F1 releases/recaptures the pointer, F11 changes
fullscreen, and Escape exits. This viewer has no collision or gameplay simulation.
For repeatable captures add `--fixed-camera --run-seconds 4 --screenshot /tmp/quake-gpu.bmp`.
`--scene-info` converts/validates a map without opening a window.

`./build-gpu/luminousquake-gpu --gpu-test` validates GPU geometry, depth, palette
sampling, resizing and readback without game data. Configure `-DLQ_GPU_TESTS=ON`
to include that window-system/GPU-dependent check in CTest. Ordinary scene/import
checks run without a GPU. `-DLQ_BUILD_CLIENT=OFF -DLQ_BUILD_GPU=ON` builds the
SDL3 viewer without the playable client; headless-only builds require no SDL library.

MSL and Vulkan SPIR-V shaders are embedded; runtime Qt/shader tools are not needed.
To regenerate them, install `glslang` and `spirv-cross`, then run
`python3 tools/build_shaders.py`. SDL3 selects Metal on the tested Mac; Vulkan
shaders support the other initial backend. D3D12 needs a future DXIL shader build.

## Playable software client: Phase 2

The SDL3 client presents Quake's original software renderer and retains the
original gameplay, QuakeC interpreter and sound mixer. Dynamic lighting upgrades
and a modern 3D graphics backend are later phases. The headless client and
dedicated server retain null video/input/audio backends and need no SDL.

Requirements: C11 compiler, CMake 3.20+, zlib development files, SDL3 3.4+ for the windowed client,
and Python 3 for tests (`-DBUILD_TESTING=OFF` removes the Python requirement).
On Apple Silicon use native arm64 dependencies:

```sh
brew install sdl3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/sdl3
cmake --build build --parallel 10
ctest --test-dir build --output-on-failure
./build/luminousquake --platform-demo
```

The asset-free demo supports mouse look, **T** for a generated tone, **F11**
for fullscreen, **Escape** to exit and the grave key for the console.
It demonstrates platform handling, not the Quake BSP renderer.

For the local archive, from the repository root:

```sh
./build/luminousquake -basedir "$PWD/quake-complete/Quake" +map e1m1
```

`-basedir PATH` expects `PATH/id1/`; original installations typically use
`pak0.pak` and `pak1.pak`. DOS-era and GOG installations name them `PAK0.PAK`
and `PAK1.PAK`, which are also found on case-sensitive filesystems. The local `quake-complete/` archive is ignored by Git
and has unverified provenance/content. It is a smoke-test sample, not the
original 1996 compatibility baseline. Extension warnings from its game code
remain visible. See [Phase 2 notes](docs/PHASE_2.md).

### Mac app (no Terminal needed)

The build produces `build/LuminousQuake.app`, a self-contained app that
carries its own copy of SDL3. Drag it into a Quake folder (the one containing
`id1`) and double-click it. It plays the game in the folder it sits in. Placed
anywhere else, it asks you to choose a Quake folder and remembers that
choice. Hold Option while launching to choose a different folder. The game
data is only read, never changed; settings and saves go to
`~/Library/Application Support/LuminousQuake`. The app in `build/` is signed
ad hoc for the Mac that built it.

### Mac release for other Macs

`tools/release_mac.sh` builds the version to share. It is universal (Apple
silicon and Intel), runs on macOS 11 or later, and carries its own SDL3, built
from the pinned 3.4.18 source. The script signs the app with the hardened
runtime, notarizes and staples it, and wraps it in a signed, notarized disk
image with a short read-me: `build-mac-release/LuminousQuake-VERSION.dmg`.

It needs two things set up once, both on the Apple developer account:

1. A **Developer ID Application** certificate. In Xcode → Settings → Accounts,
   select the team, choose Manage Certificates, then + → Developer ID
   Application. Only the account holder can create one.
2. A notary profile in the keychain, using an app-specific password from
   account.apple.com:

```sh
xcrun notarytool store-credentials luminousquake --apple-id YOUR_APPLE_ID --team-id YOUR_TEAM_ID
```

Then:

```sh
tools/release_mac.sh --identity "Developer ID Application: YOUR NAME (TEAMID)" --notary-profile luminousquake
```

`--skip-notarize` builds and signs without notarizing; any signing identity
works for a trial. The script fails if either architecture is missing or the
app still links a library outside the bundle. With notarization it ends by
checking the app and disk image with Gatekeeper (`spctl`).

LuminousQuake targets the original game. Rerelease installations are an expected
compatibility case, with support still incomplete: known missing engine services
are tracked in the [rerelease comparison](docs/RERELEASE_COMPARISON.md). Checks
against v1.06 retail data are recorded in the [retail comparison](docs/RETAIL_COMPARISON.md). The plan
is to support the necessary services and detect unsupported requirements early,
while preserving the installed game data. Rerelease visual improvements are
within scope, with the original campaign central; full rerelease feature parity
is not required. Compatibility with rerelease multiplayer servers is unverified
and is a separate question from playing their game data locally.

Default controls: WASD movement, mouse look, left mouse attack, Space jump,
Shift run, number keys for weapons, Escape menu, grave key console, F11
fullscreen. Existing Quake configuration scripts override these defaults.
Options includes **Mouselook** immediately after **Invert Mouse**. It defaults
to on and is saved as `freelook`; held `+mlook` bindings remain available when
it is off.
Options includes a saved **Renderer** preference: **Software** (original CPU
renderer), **Classic GPU** (GLQuake-style appearance with baked lightmaps), and
**Luminous** (Tenebrae-inspired lighting, shadows and materials). Left/right
cycle in either direction; Enter advances. `lq_renderer` stores 0, 1 or 2.
Video Options' **Tube camera** (`r_fluorescent`, Luminous only) picks how
fluorescent tubes record: **eye** (0, steady), **film 24p** (1, the default:
slow breathing), **NTSC 1/60** (2, in sync, steady), **NTSC 1/250** (3, deep
slow breathing) or **PAL 25p** (4, a 5 Hz flutter). Failing tubes drop out and
restrike under every camera.
Classic GPU is now available as the feature implementation checkpoint described above;
Luminous is experimental (runtime lights, soft shadows, bounce and weapon light). Restart the game or use `vid_restart` to apply the
saved choice. The experimental GPU viewer remains a separate `luminousquake-gpu`
executable. Classic GPU describes appearance and can use Metal/Vulkan.
Options → Video Options selects resolution, horizontal FOV (10–170 degrees),
fullscreen, vsync, GPU filtering (crisp/smooth), GPU anti-aliasing (off/2×/4× MSAA),
mipmaps (off/nearest level/trilinear), and anisotropy (off/2×/4×/8×/16×).
Anisotropy requires mipmaps; trilinear includes smooth filtering. Both new options default to off and preserve the
existing appearance until enabled. For smoother distant textures, try smooth
filtering, trilinear mipmaps and 4× anisotropy; higher anisotropy costs more shader
work. Sky, masked sprites and UI retain their existing crisp sampling.
`gpu_mipmaps` stores 0/1/2 and `gpu_anisotropy` stores 1/2/4/8/16 (1 means off).
GPU water (opaque/80%/60%/40%) makes water, slime and lava translucent in Classic GPU
and Luminous, saved as GLQuake's `r_wateralpha` (1 is opaque). Original maps were not
compiled to see through liquids, so each frame also draws the leaves visible from the
liquid (or, underwater, the air) leaves in view; no re-vised maps are needed.
In Luminous, a translucent liquid is lit like any surface instead of drawn full-bright,
catching light in proportion to its opacity; the rest passes through, so faces under it
receive (1 − opacity) of light from lights on the other side of the surface.
Unsupported GPU settings report a fallback. Apply activates the staged settings and saves them in
the LuminousQuake profile. Resolution presets include 16:9 and 16:10 widescreen.
The view uses square pixels so widescreen geometry keeps its proportions.

Menus, console, HUD and crosshair automatically scale in powers of two while
the 3D view retains the selected resolution. Doubling both resolution dimensions
doubles UI pixel size. The reference is 800×600 with 2× UI pixels;
640×480 uses 1×, 1600×1200 uses 4×, and 1920×1080 uses 2×.
Scaling preserves at least a 400×300 logical canvas whenever available.
Console movement uses elapsed time and a fraction of screen height, preserving
the 800×600 speed rather than slowing down with resolution.

Rerelease map hints and ending text resolve English localization keys from
`localization/loc_english.txt` in the game search paths, or from an adjacent
`QuakeEX.kpf` in the installation root. Original literal messages pass through.
The engine reads this optional game data without modifying or bundling it.

Applying settings sizes the window to the render aspect ratio. Manual window
resizing preserves render resolution and letterboxes if the proportions differ.
Fullscreen uses the display's aspect ratio, adapting render height at the
selected width within the renderer limits, so the view fills the screen.
Applying video settings writes the profile immediately; quitting also saves it.
`-width 640 -height 480` chooses the internal resolution;
`-window-width 960 -window-height 720` chooses the initial window.
`-fullscreen` / `-window` select the initial mode. Console variables
`vid_width`, `vid_height`, `vid_vsync` apply with `vid_restart`;
`vid_togglefullscreen` switches modes. Software widths must be divisible by
8, within 320–3840; heights within 200–2400. The portable C rasterizer uses
13.19 fixed-point edges to retain subpixel precision at these widths.
Sound-device failure is nonfatal; `-nosound` explicitly disables audio.
CD/music output is still absent.

### Frame timing and VSync

Rendering no longer stops at WinQuake's 72 fps. The server, QuakeC, the
client's move messages and `wait` still run in fixed 1/72 s ticks, so physics
behaves as at 72 Hz. Mouse look, keyboard turning, entity interpolation,
rendering and sound run every frame. In local games, entities are drawn
between the last two server ticks. VSync (`vid_vsync`) now defaults to on, so
frames follow the display refresh without tearing. Set `vid_vsync 0` with
`vid_restart` for benchmarks or lower latency. Profiles that already saved
`vid_vsync "0"` keep that setting. `host_maxfps` (default 0, meaning no cap)
optionally caps the render rate. Design notes and the checks still to run (frame
rate at the display rate, movement and jump traces against the previous build,
a demo round trip) are in [docs/FRAME_TIMING.md](docs/FRAME_TIMING.md).

## Headless builds and checks

```sh
cmake -S . -B build-headless -DLQ_BUILD_CLIENT=OFF
cmake --build build-headless --parallel 10
ctest --test-dir build-headless --output-on-failure
./build-headless/luminousquake-server -basedir PATH +map start
```

All builds provide `--version`, `--self-test`, and `--check-data -basedir PATH`.
The last checks required asset presence and returns 2 when files are missing;
it does not certify compatibility. `-noudp` disables UDP and `-ip ADDRESS`
selects an IPv4 interface. The loopback test needs socket-binding permission.

On Linux use an SDL3 development package or build SDL3 from its official source.
On Windows use a current Visual Studio C11 compiler and SDL3's VC development
package, passing its `cmake` directory as `-DSDL3_DIR=...`; use `--config Debug`
for builds and `-C Debug` for CTest. CI builds a pinned SDL3 SDK on each platform.
The DLL is copied beside the client executable. The CI workflow installs SDL
on all three platforms; Windows/Linux jobs have not yet run for this project.

Sanitizer build:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DLQ_SANITIZERS=ON -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/sdl3
cmake --build build-sanitize --parallel 10
ctest --test-dir build-sanitize --output-on-failure
./build-sanitize/luminousquake --platform-test --test-fullscreen
```

CTest uses SDL dummy drivers for repeatable platform tests; the last command
checks the native display/audio devices and fullscreen. Optional content smoke:

```sh
python3 tests/retail_smoke.py ./build-sanitize/luminousquake "$PWD/quake-complete/Quake"
```

It links/copies only PAKs into a temporary directory; generated configs, saves,
demos and screenshots never go into the supplied installation. Interactive
runs normally save `config.cfg` in their game directory. For bounded captures
use `--run-seconds 8 --screenshot /tmp/quake.bmp`.

## Provenance and license

Quake upstream: `bf4ac424`. SDL input/audio selectively adapt ioquake3.
See [SOURCE_PROVENANCE.md](SOURCE_PROVENANCE.md), [gnu.txt](gnu.txt), and
[id Software's release notes](readme.txt). Original notices are retained;
new engine and test code is GPL-2.0-or-later. Game assets are separate and
are not included in source control.
