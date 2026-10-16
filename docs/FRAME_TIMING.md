# Frame timing and VSync

Status as of October 15, 2026: implemented and built. Automated checks pass,
but the gameplay and display checks under **Pending verification** have not yet
run.

## Why it changed

WinQuake's `Host_FilterTime` skipped the whole host frame until 1/72 s had
passed. That limit pinned rendering to 72 fps as well as the simulation.
On 60, 120, 144 and 240 Hz displays, 72 fps gives uneven frame pacing.
Before this change, `vid_vsync` defaulted to 0 (immediate presentation), which
can tear when the game presents straight to the display in fullscreen.
NetQuake physics and QuakeC think timing depend on `host_frametime`, so the
simulation has to keep its 72 Hz cadence.

## How a host frame now runs

`Host_Frame` (WinQuake/host.c) splits each frame into two cadences:

- **Every frame**: key events, console commands, view turning
  (`CL_AccumulateInput`: keyboard turning and mouse look), reading server
  messages, entity interpolation, rendering and sound.
- **Every 72 Hz tick**: the client move message (`CL_SendCmd`), the local
  server (`Host_ServerFrame`, with `host_frametime` exactly 1/72 s) and the
  release of a pending `wait`.

`Host_CountTicks` adds each frame's time to an accumulator and returns how many
whole 1/72 s ticks are due. A frame can run zero, one or several ticks: zero is
common above 72 fps, and several happen when a slow frame needs to catch up.
Frame times are still clamped to 0.1 s, so after a long hitch the simulation
loses time as it always has rather than spiralling. `host_framerate` still
fixes each frame's time; the ticks then follow that fixed rate.

### Input

Mouse look and keyboard turning update `cl.viewangles` every frame. Mouse
movement that moves the player (strafe, or forward without mouse look) is summed
in `cl_pendingmove` and added to the next move message. Each move therefore
covers the same 1/72 s of input it always did. Button presses between ticks
keep their impulse bits until `CL_SendMove` sends them, so a tap or a
mouse-wheel jump shorter than a tick still reaches the server.

### `wait`

`wait` now holds the rest of the command buffer until the next tick has been
sent (`Cbuf_Waited`), rather than for one rendered frame. At 72 fps the two
were the same, so scripts such as `+jump; wait; -jump` still put each half
in its own move.

### Interpolation in local games

WinQuake turned off entity interpolation whenever a local server was active,
because the client and server ran in lockstep. Frames now land between ticks,
so `CL_LerpPoint` places them between the last two server messages using
`Host_TickFraction()` (the accumulator divided by the tick). This keeps
`cl.time` moving smoothly with real time. Positions lag the newest tick by up
to one tick (about 14 ms, 7 ms on average); view angles never lag.
`cl_nolerp 1` still snaps to the latest message. Remote servers and demo
playback use the original interpolation path unchanged.

## Pacing and presentation

| Cvar | Default | Meaning |
| --- | --- | --- |
| `vid_vsync` | `1` | VSync presentation (`SDL_GPU_PRESENTMODE_VSYNC` for Classic/Enhanced, `SDL_SetRenderVSync` for Software). `0` is immediate presentation, for benchmarking or lower latency. Applies with `vid_restart`. |
| `host_maxfps` | `0` | Optional render cap, clamped to 10–1000. `0` means none: VSync paces frames, with an internal 1000 fps ceiling. Saved in the profile. |

Existing profiles that saved `vid_vsync "0"` keep it; change it in Video
Options or with `vid_vsync 1; vid_restart`. Metal has no mailbox mode, and
unsupported present modes still report a fallback through
`SDL_WindowSupportsGPUPresentMode`. A dedicated server has nothing to draw
and still wakes once per tick. `timedemo` stays uncapped and reads one demo
message per frame, as before.

## Verified

- `ctest --test-dir build`: 21/21 pass.
- Headless `timedemo demo1` and `demo2` on shareware 1.01 run 969 and 985
  frames, the same as the previous build.

## Pending verification

These should run soon; until they do, treat the change as provisional.

1. **Display-rate rendering with VSync.** In a normal game, confirm the client
   presents at the display refresh (60/120/144/240 Hz) with `lq_profile 1` and
   `lq_profile_report`, in Software, Classic and Enhanced.
2. **Movement and physics against the previous build.** On shareware E1M1, script
   walk-forward and jump traces (`edict 1` once per tick, separated by `wait`).
   Compare them with the pre-change binary run at `host_framerate 0.0138889`:
   trajectories and jump peak should match. Also check plats and doors by hand.
   Build the comparison binary from commit 79c78d9, and run the script from a
   temporary basedir that links `pak0.pak` (the command line is too short to
   hold the script).
3. **Recorded demo round trip.** Record a demo with the new build, then confirm it
   plays back the same and at the right speed.
4. **Feel.** Check mouse look, keyboard turning, `+mlook` strafing and
   `wait`-based binds by hand, and check for tearing in fullscreen with VSync on
   and off.
5. **Remote and listen servers.** Confirm a remote client sends moves at 72 Hz
   and interpolates as before, and that a listen server's own client looks
   smooth.
