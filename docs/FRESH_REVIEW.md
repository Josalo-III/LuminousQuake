# Fresh Mac app review — October 16, 2026

This pass inspected the running Mac app, the launcher, input, frame timing,
presentation, and the available checks. Existing uncommitted work was preserved.

## Fixed: opening the console hides the GPU world

In Classic GPU and Luminous, opening the half-height console made the entire
palette overlay opaque. The console appeared correctly, but the uncovered game
area below it became black. Console drawing already supplies explicit coverage,
so only frames without a captured scene need a fully opaque overlay.

`tests/console_smoke.py` reproduces the issue using a temporary profile and PAK
symlinks. It loads E1M1, waits for sign-on and settling, pauses, opens the console,
then checks a captured band between the console and HUD. Before the fix, retail
Classic had 0% visible pixels there and the check failed. After the fix:

| Data | Classic GPU | Luminous |
| --- | --- | --- |
| Shareware | Pass, 82.0% visible | Pass, 82.1% visible |
| Retail | Pass, 82.0% visible | Pass, 82.1% visible |
| Rerelease | Pass, 79.5% visible | Pass, 86.2% visible |

A captured Luminous frame was also visually inspected: the console, uncovered
world, weapon and HUD are present. These percentages detect blank compositing;
they are not lighting calibration targets.

Run against any installation with a real display/GPU:

```sh
python3 tests/console_smoke.py build/LuminousQuake.app/Contents/MacOS/LuminousQuake quake-shareware --renderer classic
python3 tests/console_smoke.py build/LuminousQuake.app/Contents/MacOS/LuminousQuake quake-retail --renderer luminous
python3 tests/console_smoke.py build/LuminousQuake.app/Contents/MacOS/LuminousQuake quake-complete/Quake --renderer luminous
```

The test reads installation PAKs without changing them, and writes only to a
temporary directory (plus an optional explicit `--capture` path). It is opt-in
because normal CI does not have the user's game data or a native GPU display.

## Other validation

- Rebuilt the native Mac app and refreshed its ad hoc signature.
- All 21 CTest checks passed after the change, including GPU readback and UDP.
  Restricted sandbox execution cannot supply the native display or sockets;
  validation ran with those permissions available.
- The packaged app passed `tests/retail_smoke.py` on retail data in Classic GPU:
  movement/attack commands, save/load, demo recording/playback, renderer switches,
  capture, and saved video settings across a process restart.
- Launcher source inspection confirms that holding Option during an interactive
  app launch discards automatic folder selection and opens the picker. The picker
  accepts the installation root or its `id1` subfolder, with lowercase or uppercase
  PAK names. The physical Option-launch gesture was not automated in this pass;
  the six content runs used explicit installation paths.

## Confirmed remaining rerelease gap

A fresh read-only inventory of `progs.dat` inside each local base PAK found no
unsupported builtin declarations in shareware or retail. The rerelease declares
`finaleFinished` (79) and `localsound` (80), beyond the current engine table.
Calling either still raises a VM error. This matches the previously documented
[rerelease comparison](RERELEASE_COMPARISON.md); this pass did not implement them
or exercise those call sites. Their presence does not prove every ordinary play
session reaches them, but the engine cannot claim full rerelease support yet.

The console matrix establishes startup and this rendering behavior, not complete
campaign, ending, multiplayer, or expansion compatibility. The already-running
interactive app must be quit and reopened to load the rebuilt executable.

## Follow-up: SDL packaging and relocation

The older `build-release`, `build-gpu`, and `build-sanitize` app executables still
had absolute Homebrew SDL load commands despite containing the library. The
freshly rebuilt `build` app used its embedded SDL in a dyld trace. Packaging
scripts were not link dependencies, allowing old bundles to remain stale when
only packaging logic changed.

Packaging now writes an explicit
`@executable_path/../Frameworks/libSDL3.0.dylib` dependency and checks every Mach-O
architecture of the executable and embedded dylibs for dependencies or search
paths outside the bundle and macOS system libraries. Both packaging scripts are
tracked link dependencies. The release script uses the same dependency check.

The new `mac-bundle-relocation` check copies the app to a different folder with
spaces, inspects the actual dyld load trace, and confirms removing the copied SDL
makes launch fail rather than falling back to Homebrew. It passed on the rebuilt
native app and universal app in `build-mac-release/build/LuminousQuake.app`.
Both the executable and SDL in the universal app contain `arm64` and `x86_64`.
The universal build uses the existing source-built SDL SDK, not Homebrew SDL.
These rebuilt bundles are signed ad hoc; the older signed DMG/ZIP artifacts
were not republished or replaced by this pass.

## Follow-up: Episode 1 slipgate crash

The reported Quake Complete route (New Game, Normal difficulty, Episode 1)
failed during loading with `i >= cl.maxclients`. Forced catch-up frames reproduced
the exact error in a headless `start` → `e1m1` transition before the fix.

Multiple server ticks can queue snapshots before the next client receive pass.
`SV_SpawnServer` then clears the client state, including `cl.maxclients`, while
an old-level entity update is still queued. The parser subsequently treats that
update's player colormap as invalid against the cleared scoreboard.

`SV_SendReconnect` now consumes pending loopback-client messages before freeing
the old level. Message draining is separated from clock advancement and entity
interpolation, so this boundary check does not add simulation or rendering steps.
Remote clients and game-controlled slipgate/respawn rules retain their behavior.

Validation: repeated `start`/`e1m1` changes and a same-level restart at seven
simulation ticks per frame pass on shareware, retail and rerelease. The rerelease
case also passes AddressSanitizer/UndefinedBehaviorSanitizer. The reported route
was walked in the Mac app through Normal and the Episode 1 slipgate to E1M1.
Run the opt-in stress check with:

```sh
python3 tests/level_transition_smoke.py build/luminousquake-headless quake-complete/Quake
```
