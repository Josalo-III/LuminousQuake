# Phase 1: portable native bootstrap

Historical milestone; the current SDL client and validation are described in
[Phase 2](PHASE_2.md).

Implemented locally on October 13, 2026. Native arm64 validation uses Apple Clang on the M1 Max. Windows/Linux jobs are configured but have not run against a project-owned remote. Retail gameplay remains unvalidated pending original game data.

## Build scope

`quake_core` builds the original NetQuake client/server, QuakeC interpreter, loaders, and portable C software renderer. `luminousquake` supplies the client entry point; `luminousquake-server` supplies the same bootstrap with dedicated mode enabled automatically. Neither needs a window, graphics context, audio device, or third-party library. QuakeWorld is outside this target selection.

Existing null video/input/audio/CD backends deliberately remain in place through Phase 1. The next phase introduces SDL window/input handling and portable sound output. The headless client target establishes compilation and loader/VM checks; it is not a usable interactive client.

## Changes

- Root CMake build, C11 compilation, optional ASan/UBSan, CTest, and a three-platform CI workflow.
- Portable startup, file access, monotonic timing, sleep, console input, and diagnostics.
- Portable IPv4 socket adapter that preserves the existing datagram API and wire protocol. Native socket handles remain separate from the engine's integer IDs. Address conversion avoids macOS sockaddr layout assumptions and Windows socket-handle truncation. Numeric subnet shorthand is preserved; `-ip` makes bind/advertised interface selection explicit.
- Pointer-width-safe alignment/address calculations, `offsetof` for structure recovery and cache sizing, and portable C paths even on 32-bit x86.
- QuakeC entity strides aligned for native headers while retaining 32-bit VM cells. Bytecode lump ranges are checked; unaligned typed lumps are copied to aligned memory rather than rejected solely for host alignment.
- Unsigned bit assembly for endian/message operations, bounded float-message reads, bounded basename extraction, and a guard against drawing the disc icon before its asset exists.
- Standard C console logging and an explicit string format when sending a level title.

The platform adapter is new code around Quake's existing interfaces. Phase 1 does not transplant ioquake3's Q3-specific event loop or change simulation scheduling.

## Checks

CTest runs seven cases:

1. IPv4 loopback send/receive and Quake subnet shorthand.
2. Client version/startup diagnostics.
3. Client native layout, allocation/reuse, pointer recovery, basename, address, endian/message, and timing checks.
4. Client generated-data integration checks.
5. Server version/startup diagnostics.
6. Server native core checks.
7. Server generated-data integration checks.

Generated-data tests exercise QuakeC v6 arithmetic and entity load/store, an entity field count that requires native stride padding, aligned and unaligned bytecode lumps, malformed header/range rejection, missing-data errors, loose-file and PAK discovery, and an actual dedicated `Host_Init`/`Host_Frame` followed by command-driven shutdown. Fixtures are created in temporary directories and contain no retail assets.

Native Debug and Release builds and the ASan/UBSan build are checked before delivery. A local socket test needs to run outside a sandbox that prohibits binding sockets. Successful fixture tests demonstrate these paths, not complete Quake compatibility or rendering correctness.

## Remaining limitations

- No SDL window/input, audible sound, OpenGL backend, enhanced assets, or dynamic lighting yet.
- Original game levels, saves, demos, movement, monsters, and real multiplayer sessions require the retail-data baseline and subsequent checks.
- The legacy low-level `-record`/`-playback` VCR mechanism serializes host structures and pointer-derived session identifiers. The portable bootstrap rejects these flags until that format is addressed. This mechanism is separate from ordinary `.dem` recording/playback commands, whose compatibility has not yet been validated.
- The filesystem retains legacy path and asset limits. Startup checks common base/mod argument lengths, but the general filesystem and all content parsers have not undergone a complete bounds audit.
- Existing style, sign-conversion, and unused-variable warnings remain visible. The entire historical codebase is not treated as warning-free. More renderer paths will be exercised once scenes/assets are available.
- IPv4 transport is implemented. Automatic interface selection uses local hostname resolution with a loopback fallback; select `-ip` explicitly if the hostname does not resolve to the desired LAN interface. Broadcast discovery and remote multiplayer need real-network testing.
- MSVC/Windows and Linux runtime results are pending. The configured GitHub Actions workflow will run when this branch is hosted on a project-owned repository; do not push project work to id Software's upstream remote.

## Next milestone

Phase 2: retain the native build and checks while replacing null backends with SDL window/input support and basic portable audio output. Add a synthetic scene and lifecycle/input checks before using retail data to establish the playable compatibility baseline.
