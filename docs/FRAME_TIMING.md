# Frame timing and VSync

Status as of October 17, 2026: implemented and built. Automated checks and the
scripted movement comparison pass; the checks under **Pending verification** have not yet
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

### Real-time clock

The SDL client starts an independent 72 Hz clock thread after platform
initialization. It is the master timeline: it does nothing but wait for each
tick's deadline and publish the tick number atomically, and `Host_CountTicks`
takes its ticks from that publication. Consumers are never woken or blocked by
it. Only timing data is shared; simulation and legacy game state stay on the
main thread.

On macOS the clock runs under Mach time-constraint scheduling, the class audio
threads use, declaring a 1/72 s period, a computation of the early-wake margin
plus 50 µs, and a constraint of the margin plus 1 ms. The kernel schedules it
ahead of ordinary threads and wakes it on time as long as it stays within that
budget. It sleeps until the margin before each deadline, then spins the rest of
the way and publishes. Windows and Linux ask SDL for time-critical priority
(MMCSS, or SCHED_RR directly or through rtkit) and otherwise run as an ordinary
thread on the same schedule. `lq_clock_mode` is saved in the user config:

| Value | Policy |
| --- | --- |
| `0` (default) | Automatic early wake: 2 ms before each deadline with CPU headroom, 0.5 ms otherwise, under real-time scheduling. |
| `1` | Real-time, waking at the deadline with no early margin. |
| `2` | Continuous spinning while gameplay is active. A thread that never sleeps would overrun a real-time budget and be demoted, so this mode uses the standard scheduling policy. |

The 2 ms margin requires at least eight logical CPUs, a worker budget that
leaves room for the main thread, clock and two spare CPUs, and the shared
resource monitor reporting headroom. The automatic six workers on the ten-core
M1 Max meet this requirement. CPU utilization is sampled every 500 ms by
`platform/performance_sdl.c` (performance measures sample at most twice a
second); two consecutive samples at or above 85% mean constrained (1 s), four
at or below 65% mean headroom (2 s), and unknown utilization takes
the 0.5 ms margin. Menus, console and pauses wake at the deadline in every
mode. Margin and policy change only between ticks and never reset the epoch or
tick sequence.

The game uses the published tick while it is current or the next tick is
moments away. If the clock ever falls more than a whole tick behind real time,
the reader takes the current tick from the monotonic clock instead and counts
a fallback, so a preempted clock cannot hold the game back indefinitely. The
interpolation fraction is measured from the start of the tick in use.

`lq_clock_report` prints the mode, whether real-time scheduling is held, the
margin, publications 250 µs or more after their deadline (`late`), the worst
lateness, ticks published together after a stall (`skipped`), reader fallbacks,
sampled load, core count, mode switches and the current tick. Startup waits for
the clock to become ready; shutdown joins it before SDL teardown. On
initialization failure, and in the non-SDL headless/server targets, the
original synchronous accumulator remains available.

Normal wall-clock catch-up is bounded to eight ticks per rendered frame after a
long hitch. `host_framerate` and timedemos retain their existing virtual frame-time
accumulator. That override continually rebases the real-clock cursor, avoiding
a backlog when returning to normal wall-clock operation.

The clock test (`tests/tick_clock.c`) covers margin selection under pressure
and recovery, published-tick authority, unavailable load sampling, low-core
capacity, worker-budget changes, inactivity, explicit modes, real-time
scheduling, timeline continuity, restart and shutdown.
`tests/clock_content_smoke.py` exercises all three modes with real PAKs and the
actual SDL client. On retail E1M1 (October 17, 2026) all three modes reported
no late publications, skipped ticks or fallbacks, with a worst lateness of
42 µs. Over 12 s at 60 fps the client used 3.1 s of CPU waking at the
deadline, 4.2 s with automatic early wake and 12.1 s spinning. See
[clock measurements](CLOCK_THREAD_EXPERIMENT.md) for wake timing under load.

### Tick and frame work

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
Each due tick runs in full, as one 72 Hz WinQuake frame did: the commands a
previous tick's `wait` released, one move message, one server frame, then the
release of `wait`. A catch-up frame therefore keeps scripts and movement on the
same ticks as an unhurried one.
Frame times are still clamped to 0.1 s, so after a long hitch the simulation
loses time as it always has rather than spiralling. `host_framerate` still
fixes each frame's time; the ticks then follow that fixed rate.

Before a local server changes levels or restarts, `SV_SendReconnect` drains
pending local snapshots while the old models and scoreboard still exist. A
catch-up frame may otherwise clear those resources before the client consumes
an earlier tick's entity updates, producing `i >= cl.maxclients`. This drain
does not advance client time or relink entities; normal frame interpolation
retains its existing cadence. `tests/level_transition_smoke.py` forces catch-up
ticks through repeated transitions and a restart using an isolated game profile.

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
| `vid_vsync` | `1` | VSync presentation (`SDL_GPU_PRESENTMODE_VSYNC` for Classic/Luminous, `SDL_SetRenderVSync` for Software). `0` is immediate presentation, for benchmarking or lower latency. Applies with `vid_restart`. |
| `host_maxfps` | `0` | Optional render cap, clamped to 10–1000. `0` means none: VSync paces frames, with an internal 1000 fps ceiling. Saved in the profile. |

Existing profiles that saved `vid_vsync "0"` keep it; change it in Video
Options or with `vid_vsync 1; vid_restart`. Metal has no mailbox mode, and
unsupported present modes still report a fallback through
`SDL_WindowSupportsGPUPresentMode`. A dedicated server has nothing to draw
and still wakes once per tick. `timedemo` stays uncapped and reads one demo
message per frame, as before.

## Verified

- `ctest --test-dir build`: 23/23 pass, including the real-time clock thread test.
- Clock policy changes pass real-time E1M1 checks in Software and Classic GPU.
- Native CPU pressure triggers automatic wait-mode fallback and subsequent recovery.
- Retail save/load, demo recording/playback, and repeated level changes/restart pass.
- Headless `timedemo demo1` and `demo2` on shareware 1.01 run 969 and 985
  frames, the same as the previous build.
- Movement and jump traces on shareware E1M1, October 16, 2026: 315 ticks of
  `edict 1` (settle, walk forward down the stairs, running jump, standing
  jump), one per `wait`, compared with commit 79c78d9 run at
  `host_framerate 0.0138889`. The free-running build matches every tick
  exactly; both jumps peak at the same height (+29.3 and +43.7 units) on the
  same tick. The first run found that a catch-up frame ran several server
  ticks behind a single move and a single `wait`, so a script drifted ahead
  of the old build; each tick now runs in full (see above).

## Pending verification

These should run soon; until they do, treat the change as provisional.

1. **Display-rate rendering with VSync.** In a normal game, confirm the client
   presents at the display refresh (60/120/144/240 Hz) with `lq_profile 1` and
   `lq_profile_report`, in Software, Classic and Luminous.
2. **Plats and doors by hand.** The scripted movement comparison passed (see
   *Verified*); riding plats and opening doors still needs a hand check.
3. **Recorded demo round trip.** Record a demo with the new build, then confirm it
   plays back the same and at the right speed.
4. **Feel.** Check mouse look, keyboard turning, `+mlook` strafing and
   `wait`-based binds by hand, and check for tearing in fullscreen with VSync on
   and off.
5. **Remote and listen servers.** Confirm a remote client sends moves at 72 Hz
   and interpolates as before, and that a listen server's own client looks
   smooth.
