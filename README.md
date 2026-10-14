# LuminousQuake

A cross-platform Quake engine project exploring dynamic lighting, shadows,
improved materials, and faithful asset improvements. See the
[implementation plan](IMPLEMENTATION_PLAN.md).

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

Requirements: C11 compiler, CMake 3.20+, SDL3 3.2+ for the windowed client,
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
`pak0.pak` and `pak1.pak`. The local `quake-complete/` archive is ignored by Git
and has unverified provenance/content. It is a smoke-test sample, not the
original 1996 compatibility baseline. Extension warnings from its game code
remain visible. See [Phase 2 notes](docs/PHASE_2.md).

Default controls: WASD movement, mouse look, left mouse attack, Space jump,
Shift run, number keys for weapons, Escape menu, grave key console, F11
fullscreen. Existing Quake configuration scripts override these defaults.
Options includes a saved **Renderer** preference: **Software** (original CPU
renderer), **Classic GPU** (GLQuake-style appearance with baked lightmaps), and
**Enhanced** (Tenebrae-inspired lighting, shadows and materials). Left/right
cycle in either direction; Enter advances. `lq_renderer` stores 0, 1 or 2.
The menu identifies GPU gameplay integration as pending. Those preferences take
effect after restart once their backends are connected to the playable client;
today the experimental GPU viewer remains the separate `luminousquake-gpu`
executable. Classic GPU describes appearance and can use Metal/Vulkan.
The old video-options page
is not an SDL settings UI; use console commands instead.

Window resizing preserves the software render resolution and aspect ratio.
`-width 640 -height 480` chooses the internal resolution;
`-window-width 960 -window-height 720` chooses the initial window.
`-fullscreen` / `-window` select the initial mode. Console variables
`vid_width`, `vid_height`, `vid_vsync` apply with `vid_restart`;
`vid_togglefullscreen` switches modes. Software widths must be divisible by
8, within 320–1280; heights within 200–1024. These are legacy renderer limits.
Sound-device failure is nonfatal; `-nosound` explicitly disables audio.
CD/music output is still absent.

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
