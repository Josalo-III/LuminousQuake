# Independent 72 Hz clock experiment

October 17, 2026. Native arm64 benchmark on Joseph's M1 Max MacBook Pro.

The initial modes in standalone `tools/tick_clock_bench.c` compare two schedules:

- `direct`: the consumer sleeps until each absolute 72 Hz deadline itself.
- `threaded`: a separate timing thread sleeps until each deadline, publishes a
  sequence number, and signals a consumer through a condition variable.

Both schedules use `mach_absolute_time` and `mach_wait_until`. Deadlines are
computed from a fixed epoch rather than from the previous wakeup. The timer
sleeps; it does not busy-wait. Synthetic CPU load and consumer work deliberately
busy-wait or calculate to model occupied cores.

The benchmark separates deadline-to-clock delay, clock-to-consumer delay, and
deadline-to-consumer delay. Consumer sequence numbers retain every tick even
when condition notifications merge. Thus a consumer stall cannot be mistaken
for lost clock ticks or for clock drift.

## Reproduce

```sh
clang -O2 -std=c11 -Wall -Wextra -Werror -pthread tools/tick_clock_bench.c -o /tmp/lq-clock-bench
/tmp/lq-clock-bench direct 6 0 1000 0
/tmp/lq-clock-bench threaded 6 0 1000 0
/tmp/lq-clock-bench threaded 6 8 1000 20000
/tmp/lq-clock-bench threaded 6 8 1000 0 interactive
/tmp/lq-clock-bench threaded 6 8 1000 0 precise
```

Arguments after the mode are duration in seconds, CPU load threads, consumer
work in microseconds per tick, and an additional consumer stall in microseconds
once every 72 ticks. The optional `interactive` mode sets user-interactive QoS;
`precise` also requests thread latency QoS tier zero. Neither requests real-time
scheduling. Scheduling-policy calls must succeed or the trial fails.

Results are emitted as JSON. CPU times are measured per thread; percentages
refer to one core, not the whole machine. Each six-second trial contains 432
ticks. Trials run sequentially, and paired trials reverse execution order on
the second repetition. The latency-tier trials use four seconds (288 ticks)
per run; inherited-priority and interactive-priority trials use six seconds.

The synthetic matrix includes idle operation, 1 ms of consumer work per tick,
eight busy load threads, and a 20 ms consumer stall once per second. A separate
pair runs alongside the real engine playing retail `demo1` headlessly, without
synthetic load. This exercises the native engine and software renderer, not a
Metal display/presentation workload.

These are short scheduling experiments in a desktop session, not real-time
guarantees or a full engine integration test. No runtime threading change is
enabled by this benchmark.

## Measurements

42 trials completed, covering 16,416 ticks. Every trial recorded both a clock
wakeup and consumer start for every scheduled tick. This verifies backlog
handling in this harness, not a guarantee that deadlines are always met.

For the independent timing thread:

| Timing policy | Idle clock wakeup median, range across repetitions | Timing-thread CPU, range across trials |
| --- | ---: | ---: |
| Inherited | 2.68–2.78 ms late | 0.110–0.143% of one core |
| Interactive | 2.780–2.783 ms late | 0.066–0.139% of one core |
| Interactive + latency tier zero | 1.012–1.013 ms late | 0.112–0.150% of one core |

Interactive priority alone did not materially improve idle wakeup timing in
these runs. Requesting latency tier zero reduced the observed median, but did
not eliminate scheduling outliers. The exact source of all wakeup delays was
not profiled; these measurements do not prove a particular kernel mechanism.

The real-demo comparison used interactive priority, 1 ms consumer work, and
no synthetic load or imposed consumer hitches:

| Metric | Consumer sleeps directly | Independent timing thread |
| --- | ---: | ---: |
| Clock wakeup median lateness | 2.572 ms | 2.764 ms |
| Consumer start p99 lateness | 3.224 ms | 3.568 ms |
| Clock-to-consumer median | 0.333 microseconds | 6.792 microseconds |
| Clock-to-consumer p99 | 0.792 microseconds | 151.250 microseconds |
| Clock-to-consumer maximum | 24.375 microseconds | 960.417 microseconds |

The clock-to-consumer measurement includes publication, synchronization, and
consumer scheduling. During imposed stalls it also includes time waiting for
the occupied consumer, so it must not be described as pure notification cost.

Under synthetic CPU contention, the extra scheduling hop had a much larger
tail in some trials. The worst independent clock wakeup was 48.075 ms late
and the worst consumer start was 49.994 ms late, both in an interactive-priority
trial with eight load threads and imposed consumer stalls. With latency tier
zero, the worst independent clock wakeup was 13.466 ms late and consumer start
16.600 ms late. These are observed maxima in short trials, not bounds.

## Design implications

The independent publisher is inexpensive on the CPU, but adds synchronization
and a second opportunity for scheduling delay. It did not demonstrate an
intrinsic latency advantage over a consumer sleeping on the same absolute
deadlines. Its value is isolating publication from consumer work.

Keep the monotonic epoch and tick period authoritative: a consumer should be
able to calculate the current tick directly, even if the publisher is late.
Notifications can wake consumers without being the only source of truth for
elapsed ticks. Preserve sequence numbers and an explicit catch-up policy.

Before enabling this in the engine, compare integrated input and simulation
timing under actual Metal presentation and rendering stalls. The standalone
measurement establishes cost and scheduling risks, not gameplay equivalence.

Raw local measurements and runner scripts are under `/tmp/lq-clock-audit/`:
`results.json`, `qos-results.json`, `precise-results.json`, and the combined
`all-results.json`. These temporary artifacts are not required to build the
benchmark from the checked-in source.

## Continuous atomic publication, without consumer notification

The follow-up measures the publish-and-reference design directly:

- `publish-sleep`: the publisher sleeps between deadlines and stores the latest
  due tick atomically. It does not signal consumers.
- `publish-spin`: after loading/initialization, the publisher continuously checks
  monotonic time and stores a tick only when another deadline has passed. It
  does not deliberately sleep, yield, or signal during the measured interval.

In both modes, a consumer references the atomic counter in its independently
paced 240 Hz loop. Its own pacing sleep is part of the harness, not a wait on
the clock. Consequently these results contain no condition-variable handoff.
The publisher derives elapsed ticks from absolute deadlines even after
preemption, retaining the original timeline rather than counting wakeups.

```sh
/tmp/lq-clock-bench publish-sleep 4 0 0 0 interactive
/tmp/lq-clock-bench publish-spin 4 0 0 0 interactive
/tmp/lq-clock-bench publish-sleep 4 8 0 0 interactive
/tmp/lq-clock-bench publish-spin 4 8 0 0 interactive
```

Each case ran twice, reversing mode order on the second pass: eight trials,
2,304 ticks total. Both modes used interactive QoS with no additional latency
tier request. These follow-up trials used synthetic contention, not concurrent
demo playback.

| Publisher | Clock CPU, fraction of one core | Idle publication p99 | Loaded publication p99 |
| --- | ---: | ---: | ---: |
| Sleeping, atomic publication only | 0.038–0.055% | 3.409–3.449 ms | 3.294–3.352 ms |
| Continuously running, atomic publication only | 97.976–99.820% | 2.875–5.208 microseconds | 3.125–41.500 microseconds |

The worst continuous-publisher delay was 3.740 ms in the second loaded run;
remaining awake does not prevent OS preemption. No stale ticks were observed
in its 3,840 consumer checks across four runs, but those checks sample only at
240 Hz and do not rule out delays between reads. The independent per-tick
publication timestamps are what expose the preemption outlier.

The CPU tradeoff is approximately one continuously occupied core versus a
small fraction of a core. This is not a measurement of watts, battery life,
or the effect on an integrated GPU renderer. Cores on this Mac are heterogeneous,
so a fraction of one core should not be translated directly into the same
fraction of whole-machine performance.

The harness also batches 1,000 reads per consumer frame to amortize measurement
overhead. Observed per-read wall costs were about 0.6–5.6 ns for the atomic
counter and 13–39 ns for reading the OS clock and deriving the tick. Scheduling
and core placement can affect these short measurements, particularly under
load. The consumer does not need this oversampling in normal engine use.

Thus keeping a publisher awake substantially improves typical publication
freshness, at the cost of roughly an entire core. The act of publishing 72
atomic updates per second is inexpensive; continuously checking for the next
update consumes the CPU. If the purpose is only to make time available to
consumers, directly deriving the tick from the monotonic clock is also cheap
and avoids depending on a possibly stale publication.

Raw follow-up data: `/tmp/lq-clock-audit/publication-results.json`.

## Real-time scheduling and early wake

October 17, 2026, same Mac. The benchmark gained a `realtime` scheduling option
and a `publish-early` mode. `realtime` gives only the clock thread Mach
time-constraint scheduling, declaring a 1/72 s period, a computation of the
early margin plus 50 µs and a constraint of the margin plus 1 ms. Consumers keep
interactive QoS. `publish-early` sleeps until `early_us` before each deadline,
then spins to it and publishes.

```sh
/tmp/lq-clock-bench publish-sleep 4 0 0 0 interactive
/tmp/lq-clock-bench publish-sleep 4 0 0 0 realtime
/tmp/lq-clock-bench publish-early 4 0 0 0 realtime 500
/tmp/lq-clock-bench publish-early 4 0 0 0 realtime 2000
/tmp/lq-clock-bench publish-spin 4 0 0 0 interactive
```

Each configuration ran once idle and once with eight busy load threads (288
ticks per run). Publication lateness is measured from each deadline:

| Clock | CPU, one core | Idle p99 | Idle max | Loaded p99 | Loaded max |
| --- | ---: | ---: | ---: | ---: | ---: |
| Sleep, interactive QoS | 0.04–0.06% | 4,781 µs | 5,382 µs | 3,335 µs | 3,865 µs |
| Sleep, real-time | 0.04–0.07% | 34.0 µs | 50.2 µs | 13.2 µs | 14.2 µs |
| Real-time, 0.5 ms early | 3.6% | 3.6 µs | 4.3 µs | 3.0 µs | 3.3 µs |
| Real-time, 2 ms early | 14.4% | 3.9 µs | 30.3 µs | 3.0 µs | 3.0 µs |
| Continuous spin, interactive QoS | 98.6–99.7% | 9.0 µs | 15.4 µs | 3.1 µs | 82.2 µs |

Real-time scheduling alone removed the millisecond wake delays. Early wake
under real-time scheduling then matched or beat continuous spinning, including
its worst case under load, at a small fraction of its CPU, and was never
demoted. In these short runs the 2 ms margin showed no advantage over 0.5 ms;
the engine still uses 2 ms with headroom as the conservative choice. These
are observed maxima in short trials, not bounds.
