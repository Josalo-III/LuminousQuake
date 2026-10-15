# Profiling method

Performance changes must be driven by traces from repeatable gameplay, not by a
single frame-time counter. The in-engine `lq_profile` measurements are retained
as low-overhead CPU stage markers and a quick regression check. They do not
measure GPU execution, and they do not establish that a task should be moved to
a worker.

## Mac baseline

On the development M1 Max, collect three equivalent 20-second runs for each
renderer and resolution. Use a temporary user directory so saved settings do
not affect the normal installation. Prefer a stable recorded demo once one is
selected; until then, use the same map, start position, idle interval and input
sequence for every comparison. Record the exact command, build revision,
renderer, resolution, filtering, VSync state and scene in the result notes.

Collect these artifacts outside the repository:

1. **Time Profiler**: CPU call trees, sampled while the game is in the fixed
   scene. It identifies expensive functions and thread scheduling; it is the
   primary source for CPU changes.
2. **Metal System Trace**: command-buffer submission, GPU execution and present
   waits for Classic GPU and later Enhanced. A long CPU presentation stage may
   be a deliberate VSync wait or GPU back-pressure; only this trace distinguishes
   those cases.
3. **`lq_profile_report`**: stage-level CPU totals correlated with the trace,
   useful for detecting regressions in command processing, simulation, scene
   preparation, UI/compositing, presentation/submission and audio.

Use Instruments through `xctrace` with `Time Profiler` and `Metal System Trace`.
Its trace bundles must remain outside the source tree and are not game assets or
release artifacts. The development machine has both templates available in
Xcode 26.3.

After exporting the `time-profile` table to XML, use
`python3 tools/summarize_xctrace.py TRACE.xml` for a repeatable leaf-function and
binary summary. This avoids judging hotspots from a truncated raw XML export;
the Instruments UI remains the source for call-tree and timeline inspection.

## Controlled workload matrix

Run the same workload in each of these configurations before and after a change:

| Mode | 800x600 | 1920x1080 | Purpose |
| --- | --- | --- | --- |
| Software | yes | yes | CPU raster baseline and correctness reference |
| Classic GPU | yes | yes | Shared geometry, UI, upload and present baseline |
| Enhanced | once implemented | once implemented | Compare the added light/shadow work to Classic GPU |

Use VSync both on and off where supported. VSync-on measures a player-facing
frame budget but can hide spare CPU capacity; VSync-off exposes CPU and GPU
limits. Do not compare an idle menu trace with active gameplay, or a
software-renderer trace with a GPU-renderer trace, as evidence for a threading
decision.

## Decision rules for Phase 4A

Only parallelize a stage when Time Profiler identifies a substantial pure or
snapshot-owned CPU cost, the result can be merged in a stable order, and the
serial implementation remains available for comparison. First candidates are
palette conversion and texture/mip preparation. Keep QuakeC, simulation, legacy
global-state rasterization, SDL window work and GPU submission on their current
owners.

A worker-pool experiment must show all of the following against the same trace
matrix: lower CPU wall time, no material frame-time tail regression, no increased
presentation wait that erases the gain, byte-identical or otherwise specified
output, and clean map-change/video-restart/quit behavior. Record medians and
worst observed frame times, not only one favorable average.

## Current instrumentation

`lq_profile 1` enables CPU-stage accumulation. On macOS it also emits
Instruments signpost intervals for the same stages, allowing CPU samples and
Metal activity to be aligned with engine work. `lq_profile_report` prints the
current aggregate, and `lq_profile_reset` starts a new sample window. For a
bounded run, append `--profile-warmup-seconds N --profile-report` to exclude
startup/map loading and print the report before exit. The
report includes a total frame range plus input/commands, network, simulation,
client, scene preparation, UI/compositing, presentation/submission and audio.
GPU execution remains deliberately unreported until a backend exposes reliable
timestamps or Metal System Trace is used. Its `ms CPU/frame` is active engine
work only; the separate processed-FPS field uses wall time and includes the
engine timing gate.

## First measured worker workload

The first Phase 4A implementation parallelizes whole-row UI scaling/compositing
and Classic GPU palette-overlay conversion. Each job reads a fixed per-frame
snapshot, writes disjoint rows and completes before those inputs can change.
`lq_workers 0` preserves the serial implementation; `-1` selects automatic
sizing and 1–8 selects an explicit persistent pool size.

An optimized arm64 M1 Max comparison used `demo1`, Classic GPU, 1920×1080,
VSync off, a four-second warmup and a six-second measured interval. Three runs
at six workers were compared with three forced-serial runs:

| Measurement (median of 3) | Serial | 6 workers | Change |
| --- | ---: | ---: | ---: |
| Total CPU/frame | 4.549 ms | 3.424 ms | -24.7% |
| UI/compositing | 2.158 ms | 1.325 ms | -38.6% |
| Presentation/submission | 2.011 ms | 1.444 ms | -28.2% |
| Maximum CPU frame | 6.319 ms | 5.922 ms | -6.3% |

Four-, six- and eight-worker exploratory runs selected six for this ten-core
machine; eight did not improve the median work enough to justify its higher
observed tail. Automatic sizing therefore reserves four logical cores on
machines with more than six and caps the pool at eight. These are CPU-stage
figures, not GPU execution times, and the original 72 Hz processing gate keeps
reported wall-clock FPS near 69 in both cases. The 800×600, VSync-on and future
Enhanced comparisons remain part of the broader matrix rather than claims from
this result.
