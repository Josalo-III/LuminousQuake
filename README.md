# LuminousQuake

A cross-platform Quake engine project exploring dynamic lighting, shadows, enhanced materials, and faithful asset improvements. The starting source is id Software's Quake GPL release. See [the implementation plan](IMPLEMENTATION_PLAN.md) for the intended component selection and sequence.

## Current build: Phase 1

The native C client bootstrap and dedicated-server targets build without SDL, OpenGL, or audio dependencies. They currently use Quake's null video, input, and sound backends. **The client is headless; windowed play and audio output are Phase 2 work.** The dedicated server retains Quake's game and datagram protocols.

Requirements: a C11 compiler, CMake 3.20+, and Python 3 for the tests. Disable tests with `-DBUILD_TESTING=OFF` to build without Python. Use native arm64 tools on Apple Silicon; the default build follows the toolchain's architecture.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 10
ctest --test-dir build --output-on-failure
./build/luminousquake --version
./build/luminousquake-server --version
```

On Windows, use a current Visual Studio C11-capable toolchain and add `--config Debug` to the build command and `-C Debug` to CTest. Executables are under `build/Debug/` for that generator. Adjust build parallelism to the machine.

For AddressSanitizer and UndefinedBehaviorSanitizer with Clang or GCC:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DLQ_SANITIZERS=ON
cmake --build build-sanitize --parallel 10
ctest --test-dir build-sanitize --output-on-failure
```

The UDP test uses local loopback sockets and requires permission to bind them. Fixture tests generate their own data and need no game installation. See [Phase 1 notes](docs/PHASE_1.md) for validation coverage and outstanding limitations.

## Original game data

The source repository does not include retail game assets. Place your original Quake data under `PATH/id1/` and inspect its availability with:

```sh
./build/luminousquake --check-data -basedir PATH
./build/luminousquake-server -basedir PATH
```

The diagnostic returns 0 when its required files are present and 2 when they are missing. It does not validate all content or certify a working game. The current server automatically adds `-dedicated`; use normal Quake arguments and commands to choose maps and settings. `-noudp` disables UDP, and `-ip ADDRESS` selects an IPv4 bind/advertised address.

## Provenance and license

The engine is derived from [id Software's Quake](https://github.com/id-Software/Quake), upstream commit `bf4ac424`. The complete upstream revision and adaptation record are in [SOURCE_PROVENANCE.md](SOURCE_PROVENANCE.md). Original license notices are retained; see [gnu.txt](gnu.txt) and [id Software's release notes](readme.txt). New Phase 1 C code is GPL-2.0-or-later.

ioquake3 and Doom 3/dhewm3 are intended sources for subsequent selective adaptations. No code from those projects has been imported in Phase 1. Game data is separate from the engine source.
