# Experimental local rollback

Status: implementation candidate, not merge-qualified. Base: upstream
`e45ad4a384fa78c4c3cd9d828bad39453c2c42b6`. No release/default promotion.

The experiment removes peer-coordinated BEGIN/SEAL/BASELINE/POST exchanges from
ordinary input corrections. `local_rollback.h` owns a local input timeline and
replays through host snapshot, simulation and presentation callbacks. The
existing `rb_driver.h` episode implementation remains in the same library for
comparison. Hosts choose the driver after immutable startup agreement.
The episode path also fixes complete pad-row comparison and refuses a persistent
two-seat confirmed chain fork after its existing settle/correction checks.
The A/B baseline includes these correctness fixes.

## Correctness boundary

- Rows are fixed-width host-defined byte strings, 1–32 bytes. Every byte is
  compared, including right sticks, triggers and optional relative input.
- Session canonicalization runs before publication. Received rows are never
  quantized differently. A host predictor defaults to hold-last; a relative
  mouse predictor should clear only the relative delta fields.
- A snapshot at T contains state before T. On a mismatch the driver loads
  the previous confirmed prefix's exclusive frontier. Every input before
  that anchor is already real and has been simulated correctly. A later
  mismatch cannot select a snapshot containing an earlier unresolved guess.
- A replay can still predict newer missing rows. Its snapshots remain
  ineligible as anchors until their entire input prefix is confirmed.
- Optional consumption masks attest which seats the simulation observed.
  Merely failing to poll a pad is insufficient if another simulation-visible
  buffer retained it; hosts must include those observations too.
- Only confirmed post-tick digests enter the background hash chain. Missing
  commits are requested again. Confirmed mismatches stop the local driver
  with a stable refusal; replay and snapshot failures also stop explicitly.
  Verification does not stall live admission. Exhausting retained verification
  history is an explicit failure, never a silently healed gap.
- Inline and incremental replay use identical publish/finish semantics. Replay
  suppresses repeated audio and presentation through host callbacks.

## Startup and wire isolation

Both A/B modes call `rnet_experiment_profiles_begin` before transport startup.
The mode is folded into the existing build/content qualification namespace;
peers proposing different modes refuse startup. CPU/backend/profile capabilities
still use the existing fixed agreement. No mode changes during a match.

This branch speaks wire 4 and exposes
`RNET_EXPERIMENT_LOBBY_VERSION`. A host must use that experiment tag plus its
exact build/content identity in its lobby version. Wire 3 release peers cannot
link. A constant alone does not change an existing host's lobby announcement.

Local mode disables host-authoritative HELLO/DELAY_SYNC adoption. Each seat's
session stamps its own samples at T+D. Published rows are immutable. Increasing
D fills newly exposed slots; decreasing D drains previously published input
before fresh samples resume. Hosts must display the resulting delay and reserve
enough snapshots. `rnet_local_control_preview` computes the pre-match delay and
affordable depth from measured live/replay costs, transit and jitter. Simulation
cadence and available host frame budget are separate inputs. Half RTT is an
estimate, not a measured one-way path.

`rnet_local_control_begin` enables a controller before sampling. It updates
execution costs (including snapshot/digest) from live/replay work, reserves the
fixed load/drop/resim-bracket cost once per correction, and uses the
worst occupied peer's sequence-echo RTT and jitter. Hosts with their own input
transport supply the equivalent observation callback. Costs must be seeded
from the built host, including a nonzero rewind/setup seed; zero-duration clock
samples never establish free replay or rewind.
Every interval (60 live ticks by default), two stable target observations permit
a one-tick delay change. Raising and lowering use the same rule. Missing peer
measurements prevent changes. Zero affordable depth waits for real input instead
of inventing a replay budget. Hosts still reserve the full configured snapshot
depth. A slow live tick exceeding its frame budget is a host coverage/performance
gap; increasing D cannot make that computation fit.

## Observations

The query-only telemetry ring retains 36,000 completed live ticks in Release.
Replay work is attributed to the next live tick. Reports expose overwritten
history, stall time, corrections, replay work and peer probes. The headline
input latency measures **sample to simulation-input publication**, excluding
USB, scanout and display latency. No samples means no measured latency; the
report includes the sample count. Episode-mode timing currently uses its
millisecond host clock and assumes 16,667 microseconds per tick in rate reports.
Use `rnet_local_report` / `rnet_rb_driver_report` for the final match report:
these include pending corrections after the last live tick without adding a
synthetic tick. The generic ring report covers completed records only.
Episode IDs in this history are match-local serials; skipped serials preserve
episodes that began and aborted between two completed live ticks.

Sequence-stamped echo requests/replies travel on the actual session transport.
RTT is measured with one peer's clock; jitter is an EMA of successive RTT
differences. Unanswered probes are counted lost when their 32-entry retention
window is reused. Half RTT is only a one-way estimate. Path reports distinguish
direct, host relay and TURN; no TURN-specific delay floor is used by local mode.

The simulator supports receive profiles `same-region`, `cross-country`,
`transatlantic`, `transpacific`, and `wifi`. Explicit latency/jitter/loss
environment values override profile defaults. `RNET_SIM_DUPLICATE_PCT`,
`RNET_SIM_BURST_PCT`, `RNET_SIM_BURST_PACKETS` and `RNET_SIM_KBPS` add duplication,
burst loss and a serialized bandwidth cap (decimal kilobits/second).
Independent `RNET_SIM_SEND_*` values impair the outbound direction. Set only
the intended sides: send and receive impairments add together. Queue overflow
invalidates timing evidence and is reported loudly.
The existing ICE hub bypasses receive simulation, and hub-forwarded datagrams
do not pass through the new outbound queue. These controls are qualified here
for direct peer sessions, not as a complete per-pair hub network model.

## Qualification and remaining work

The initial candidate is limited to the shared library. Scripted analog traces
are not recorded human racer/FPS input, and library fixtures are not built-game
performance evidence. The merge gate remains lower long-distance input latency,
no worse stalls/visible corrections, no near-link fighting-game regression,
zero new desyncs, and minimum-hardware frame budget.

Still required before promotion:

1. Integrate the budget controller/preview with actual built-host measurements
   and lobby controls; qualify its policy on real games and minimum hardware.
2. Real racer/FPS A/B workloads and full match reports, packet-capture probe
   checks, host diagnostics-mod wiring and durable report export.
3. Shared dirty-page snapshots and each heavy core's qualified replay path.
4. Per-pair mesh transport and measured regional TURN selection. Existing
   host relay remains functional; this candidate does not claim W5 complete.
5. Consumer branches/submodule pins for all supported engines and recomp-ui,
   cross-OS runs, long soaks and individual tester acceptance.
6. The independently judged reliable message-channel/GBA distributed-link
   side track. It is not implemented by this branch.

Upstream baseline: Windows MinGW Release with ICE, all 37 CTests passed.
Candidate results belong in the experimental branch's validation record; do
not treat the baseline result as validation of new source.
