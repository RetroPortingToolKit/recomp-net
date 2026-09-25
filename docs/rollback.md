# Rollback mode

recomp-net carries delay-sync (`RNetSession`) and, beside it, a shared
rollback stack that hosts opt into without changing delay-sync behaviour. (This
file used to say rollback lived on a `feat/rollback` branch; it is on `main`,
where `rollback_episode_test` was failing, so the sentence was stale.)

## Layers

| Layer | Status | Location |
|-------|--------|----------|
| Portable input contract | Landed | [`include/recomp_net/input_contract.h`](../include/recomp_net/input_contract.h), [`src/input/rnet_input_contract.c`](../src/input/rnet_input_contract.c) |
| Rollback episode orchestration | Landed | [`include/recomp_net/rollback.h`](../include/recomp_net/rollback.h), [`src/rollback/rnet_rollback.c`](../src/rollback/rnet_rollback.c) |
| Rollback wire protocol | Landed | `RNET_PKT_RB_*` (opcodes 20–25) in [`src/protocol/rnet_protocol.{h,c}`](../src/protocol/rnet_protocol.h) |
| Admission scheduler | Moved from retcomm-rbengine | [`include/recomp_net/sched.h`](../include/recomp_net/sched.h), [`src/sched/rnet_sched.c`](../src/sched/rnet_sched.c) |
| Input history (invent / promote) | Moved from retcomm-rbengine | [`include/recomp_net/input_hist.h`](../include/recomp_net/input_hist.h), [`src/input/rnet_input_hist.c`](../src/input/rnet_input_hist.c) |
| Hash-confirm watermark | Moved from retcomm-rbengine | [`include/recomp_net/hash_confirm.h`](../include/recomp_net/hash_confirm.h), [`src/rollback/rnet_hash_confirm.c`](../src/rollback/rnet_hash_confirm.c) |
| RB_POST tip filter | Moved from retcomm-rbengine | [`include/recomp_net/rb_post.h`](../include/recomp_net/rb_post.h) |
| **Episode driver** | Lifted from snesrecomp (2026-09-24) | [`include/recomp_net/rb_driver.h`](../include/recomp_net/rb_driver.h), [`src/rollback/rnet_rb_driver.c`](../src/rollback/rnet_rb_driver.c) |

The four moved modules were MotK host policy lifted into retcomm-rbengine. Every
decision they make is about peers, so they belong beside the session and the
episode FSM. retcomm-rbengine keeps what works with netplay compiled out — the
snapshot ring and a monotonic clock — and neither library depends on the other.

## Rollback wire protocol

Additive, non-colliding opcode range for rollback-mode sessions. Delay-sync
hosts never emit or parse these. Seal rows are fixed 7-byte frames
(`RNetRbWireFrame`: buttons 2 + sticks 2 + source + predicted + valid) with the
tick derived from `row_begin + index`.

| Opcode | Packet | Payload |
|--------|--------|---------|
| 20 | `RB_SYNC` | correction tuple `(epoch, mismatch, load, target, slot, op, flags)` |
| 21 | `RB_SEAL_ROWS` | peer-authority sealed rows chunk (tuple + rows) |
| 22 | `RB_BASELINE` | post-load digests (master + 3 partitions) for the baseline gate |
| 23 | `RB_POST` | post-replay digests + match flag (commit / deepen / abort) |
| 24 | `RB_FRAME_COMMIT` | state/master-hash watermark agreement token |
| 25 | `RB_RESOLVED` | resolved-through / shared frontier advertise |

`RB_SYNC` carries an op code (the byte historically named `initiator`) and a
flags byte (`recomp_net/session.h`):

- `OP_BEGIN` — open or tip-extend an episode. Flags are
  initiator-authoritative episode attributes the follower adopts verbatim:
  `FLAG_LIGHT_TIP` (episode class; keeps baseline burst counts symmetric) and
  `FLAG_REREPLAY` (tip-extend requires re-replay from load — the follower
  mirrors the initiator's decision instead of re-deriving from its own tip).
- `OP_NACK` — follower refuses/cannot follow. The target field carries the
  follower's confirmed frontier so the initiator demotes its watermark to a
  mutually provable tick instead of guessing `load-1`.
- `OP_ABORT` — sender tore its episode down. The mismatch field carries the
  shared `RNET_RB_ABORT_CLASS_*` cooldown class, the load field the sender's
  realign tick; the receiver mirrors teardown + cooldown so both peers re-arm
  on the same schedule.

Hosts should partition epoch ids by initiator slot
(`epoch = counter << k | slot`) so concurrent dual initiation never collides
and a deterministic tie-break (lower initiator slot wins; the loser yields and
follows) can derive any episode's initiator from its id. The episode driver
uses `k = RNET_RB_EPOCH_SLOT_BITS` (3, all eight seats):
`rnet_rb_epoch_make` / `rnet_rb_epoch_initiator`.

`RB_SEAL_ROWS` carries at most `RNET_RB_SEAL_ROWS_CHUNK_MAX` (24) rows; the
send path truncates a larger chunk, so a host that chunks wider posts a partial
span and waits forever for the rest. `row_begin` is an OFFSET into the sealed
span and the `mismatch` field carries the seal base (the LOAD tick) -- sending
ticks in either posts nothing. `rnet_session_rb_last_take_from` names the
sender of the rb_* message just taken, which an episode with more than one
peer needs to count answers per peer.

Hosts map their existing wire onto these when aligning transports (BattleShip's
soak-hardened `SYNETPEER_*` format stays authoritative for live matches); new
recomp-net rollback hosts may use `RNET_PKT_RB_*` natively. Transport/ICE
unification is a follow-up — the delay-sync ICE path is unchanged.

## Rollback episode orchestration

`RNetRbSession` holds the episode state (`Live → SealInputs →
AwaitingBaseline → Replay → Verify → TipHold → Commit|Abort`), the correction
tuple, the sealed input table, and the resolved-through (shared frontier)
watermark. It is PASSIVE: `rnet_rb_set_phase` validates nothing, the event
queue has no producer inside the library, and the only host callback it ever
invokes is `get_input_row`. What runs an episode is the driver below; a host
that does not use it must do all of that itself.

TipHold is the stage after a POST match: the sealed rows stay open for
`tip_runway` ticks while Live runs, so a late edge can tip-extend the same
episode instead of opening a second one. `rnet_rb_enter_tip_hold` IS the
post-match transition (it promotes the sealed rows) and requires Verify --
calling `rnet_rb_on_post_match` first moves the phase to Commit and makes it
fail on every episode (measured on SNES: 0 tip-hold entries in 281 episodes).
`on_post_match` is the other branch, never a preceding step.

### Tip-extend / edge coalesce

`rnet_rb_extend_target` grows `target_tick` and appends seal rows so a late
wire edge (typical digital press then release) stays in the **same** episode
instead of a second seal/baseline handshake. Verify→extend drops to Replay;
TipHold→extend **stays TipHold** (host invent-caps Live past tip and
schedules a short rereplay only when sim already invented past the prior
tip). `rnet_rb_resign_slot_range` refreshes sealed rows after the host
promotes wire into history. `apply_peer_seal_rows` accepts
`target >= corr.target` and auto-extends; it also refuses to let a
**predicted** peer row overwrite a non-predicted sealed row (mask bit is
still credited) so tip-extend invent-idle exports cannot clobber a
wire-promoted resign. `rnet_rb_suggest_target` adds
`cfg.tip_seal_slack` past live tip when opening an episode.
`resolved_through` survives `session_reset`.

### Light tip

When `(target - load) <= cfg.light_tip_max_depth` (0 → the library default
`RNET_RB_LIGHT_TIP_MAX_DEPTH`, 16) and `load` is at/after `resolved_through`,
`begin_episode` sets `RNET_RB_CORR_LIGHT_TIP`. Hosts may skip the ready-ACK
RTT (digests still compared) and shrink baseline bursts.

Hosts that widen `cfg.tip_runway` for TipHold coalescing (see above) should
also raise `cfg.light_tip_max_depth` to match — a coalesced episode's
eventual depth (load..target after one or more `tip_extend` calls) can
approach `tip_runway`, and if that exceeds the light-tip ceiling the episode
silently falls back to the full ready-ACK round trip even though nothing
actually went wrong. A MotK soak with `tip_runway=24` and the untouched
library default (`16`) lost the light-tip fast path on ~19% of episodes for
exactly this reason before `light_tip_max_depth` was added and set to match.
`rnet_rb_is_light_tip_candidate_ex(load, target, resolved_through, max_depth)`
lets a host precompute the flag with an explicit ceiling before a session
exists (or before `corr` is populated); `rnet_rb_is_light_tip_candidate` is a
thin wrapper using the plain library default for hosts that don't override
`light_tip_max_depth`.

`rnet_rb_demote_resolved_through(s, tick)` pulls the shared frontier down
when a follow-NACK refuses a unilateral tip. `rnet_rb_set_peer_convergence`
only advances, and `session_reset` keeps `resolved_through` — without an
explicit demote the refused tip stays as the library watermark and the next
light-tip / host HC advance reopens it. Hosts that demote their own agreed
watermark on NACK should demote the library watermark in the same step.

`RNetRollbackVTable`: `rnet_rb_create` refuses a vtable without `load_state`,
`advance_sim` or `get_input_row`. Of the six callbacks the core itself calls
only `get_input_row`; `save_state`, `load_state`, `advance_sim`,
`state_digest` and `hash_confirm_through` exist for the host's (or the
driver's) own replay loop and are never invoked from inside the library.
(This section and `rollback.h` previously listed save/load/advance/digest as
required and called by the library; neither was true.) Host stick gates ride
through `stick_gates` and feed `rnet_rb_decide_stick_replace`.

Minimal host loop during an episode, if not using the driver. Seal from the
LOAD tick: the replay publishes a sealed row for every tick it re-runs, and
`load..mismatch-1` are among them -- sealing from the mismatch leaves the
first replayed ticks without a row.

```c
rnet_rb_begin_episode(s, &corr);                 /* mismatch identified */
rnet_rb_seal_inputs(s, corr.load_tick, corr.target_tick, corr.slot);
/* exchange peer seal rows via host transport: rnet_rb_export_seal_rows_chunk /
 * rnet_rb_apply_peer_seal_rows until rnet_rb_all_peer_seal_rows_complete */
rnet_rb_set_phase(s, nRNetRbPhaseAwaitingBaseline);
vt.load_state(vt.ctx, corr.load_tick);
for (t = corr.load_tick; t <= corr.target_tick; ++t)
    vt.advance_sim(vt.ctx, t);                   /* reads sealed rows */
/* compare vt.state_digest against peer; then, on a match: */
if (!rnet_rb_enter_tip_hold(s))                  /* TipHold, rows promoted */
    rnet_rb_on_post_match(s);                    /* runway 0: straight to Commit */
/* on a mismatch: rnet_rb_on_post_diverge(s) and abort */
```

The core is transport-agnostic; the driver speaks `RNET_PKT_RB_*` through
`RNetSession`.

## Episode driver

`recomp_net/rb_driver.h` is the code that runs an episode, lifted out of
snesrecomp's `snes_netplay_rb.c` (36d6ce5) by owner ruling on 2026-09-24 so
every engine binds one driver instead of writing its own. It owns live admit
(local tip, remote rows, hold-last invent, the scheduler and every gate it
asks for), reconcile (late wire against predicted history), the whole episode
FSM (open, follow, dual-initiation arbitration, NACK, seal-row exchange,
baseline gate, replay, POST verify, commit, tip-hold, tip-extend, abort
classes and mirrored cooldowns, a watchdog on every stage that waits on a
peer), snapshot policy (interval, floor, the baseline fork cap), the
FRAME_COMMIT hash chain, the boot-digest gate, the mod-set and identity
handshakes, lockstep degrade, the advisory chain-stall report, and a cold
reset on every start.

Two rules it adds over what snesrecomp shipped, each found by
`tests/rb_driver_test.c`'s toy engine (which folds every seat's row into its
state, so a correction that never lands is a measurable divergence):

- **A correction is owed until a replay covering it completes.** Reconcile
  promotes the true row the moment it decides to rewind; if the episode never
  replays it (NACK, abort before the load, watchdog, dual-initiation yield, a
  stage that cannot take it, the initiator cooldown) the tick is re-opened
  later ("RB correction retried"), or declared "RB correction LOST" once no
  snapshot reaches it. A completed replay pays it even if the episode then
  fails to commit: the span was re-run on authoritative rows.
- **An abort after the baseline load restores the live tip** ("RB tip
  restored"), instead of leaving the engine on the load tick while sim stays
  at the old tip.

### Integrating a host (n64lle, psxrecomp, the next engine)

The host implements `RNetRbHost`; `start()` names any required callback left
NULL and refuses.

| Callback | What the host supplies |
|---|---|
| `snap_save/load/has/oldest/drop_after` | Snapshot storage keyed by tick. Key T = the state BEFORE tick T runs. The host owns the bytes (rbengine's ring works; recomp-net does not depend on it). |
| `publish(tick, rows, slots, replay)` | The rows every seat simulates at `tick`, sanitized. Called for every live tick and every replayed tick. |
| `run_tick(tick)` | INLINE shape only: run one tick on the rows last published. |
| `resim_begin / resim_end` | Suppress presentation and audio production for a replay (NETPLAY.md §1). |
| `digest_master` / `digest_parts` | Simulation-state digest (FRAME_COMMIT, POST) and master + three named partitions (BASELINE); `cfg.part_names` names them. The digested domain must equal the snapshotted domain. |
| `decode_sample` / `sanitize_row` / `neutral_row` | Pad layout: wire bytes to a row, force a row legal, and what "nothing held" is per seat. Active-high pads (SNES, N64) and active-low (PSX) differ; the driver assumes neither. |
| `admit_sample` (opt.) | Side data riding the pad bytes, from the sample a LIVE admit used. |
| `boot_digest_noted` (opt.) | Log what explains a boot mismatch (partitions, frame counters). |
| `request_return_to_lobby` | End the match (boot fork, mod-set refusal). |
| `log` (opt.), `now_ms` | Line sink (NULL = stderr) and a monotonic clock. |

`RNetRbDriverConfig` takes live pointers to the session, seat, seat count,
delay and prediction exactly as the scheduler bridge does, plus the replay
shape, partition names, the store's depth (reported only), a log tag, and an
environment alias (knobs are `RNET_RB_<NAME>`; `<alias>_<NAME>` is read first,
so snesrecomp's `SNES_RB_*` names keep working).

The loop is the same in both shapes:

```c
for (;;) {
    RNetRbAdmit a = rnet_rb_driver_poll_admit(d);
    if (a != RNET_RB_ADMIT_STALL) {
        run_one_tick();                  /* the SAME function, live or replay */
        rnet_rb_driver_finish_frame(d);
    }
    present_and_pump();                  /* the held frame during a replay */
}
```

- **INLINE** (`RNET_RB_REPLAY_INLINE`): the driver loads the baseline and
  runs the whole replay inside one `poll_admit` through `host->run_tick`;
  `poll_admit` only ever returns STALL or LIVE. Right when a tick is cheap and
  returns (SNES `RtlRunFrame`).
- **INCREMENTAL** (`RNET_RB_REPLAY_INCREMENTAL`): each `poll_admit` during a
  replay publishes ONE replayed tick and returns `RNET_RB_ADMIT_REPLAY`; the
  host runs it with its live per-tick function, calls `finish_frame`, and may
  pump, present and service audio before the next poll. No FRAME_COMMIT is
  sent for a replayed tick. Right when a tick is expensive or must run in the
  host's own loop -- an N64 field. `run_tick` may be NULL.

Neither shape drains episode wire mid-replay, so both see the same messages
at the same point of an episode; `rb_driver_test` runs a mixed pair (one peer
inline, one incremental) to hold that. The session keeps receiving meanwhile;
a queue that fills is logged (`rnet_session_rb_ctrl_dropped`).

The scheduler is process-global (`rnet_sched_bind`): one started driver per
process.

### Coordinated stop (for harnesses)

A two-process harness that kills both peers at a deadline cannot tell an
episode in flight at the kill from a lost one, so its ledger fails on an
end-of-run race (snesrecomp's runway-4 cell: 1 of 1 in a sweep, 4 of 4 clean
on repeat). `rnet_rb_driver_request_quiesce` makes the stop exact instead of
tolerated:

- from the request on, this peer opens no episode (a mispredict found while
  draining logs `RB drain: correction not opened` and is counted), no local
  tip-extend, and the validation injector stops. It still follows a peer's
  BEGIN, and every open episode finishes or aborts with its usual line;
- once idle it sends `RNET_RB_SYNC_OP_QUIESCE` ("I will open no more
  episodes, and have none open"), re-sent every 50 ms; a peer that receives
  one starts draining too, so asking either side drains the match;
- `rnet_rb_driver_quiesce_state` reaches `RNET_RB_QUIESCE_DRAINED` when this
  peer is idle and holds every peer's marker (`RB quiesced ... Opened here: I
  as initiator, F as follower`). Nothing either side opened can then be
  unanswered, so `initiated - followed - refused == 0` with no loss. The
  host keeps running live ticks until then and exits after;
- bounded: `RNET_RB_QUIESCE_TIMED_OUT` after `RNET_RB_QUIESCE_TIMEOUT_MS`
  with a line naming what was outstanding (a vanished peer, or one that
  predates the op -- it ignores the unknown op, so the drain ends on the
  bound rather than hanging).

The trigger is the host's: snesrecomp maps SIGUSR1 to it and
`tools/rb_loopback.sh` sends SIGUSR1 to both peers at the deadline, then
grades the ledger only if both logged `RB quiesced`. A port of that harness
(n64lle) needs the same two pieces: a host trigger that calls
`request_quiesce`, and an exit once the state is DRAINED or TIMED_OUT.

Tip-hold now logs every exit with how long it actually held:
`RB tip-hold ended epoch=E held=N ticks (runway R) — <why>` (runway spent,
every peer committed, tip-extend, aborted, peer aborted, yielded, watchdog
expired). The runway is a ceiling, not a duration.

### More than two peers

Built, not exercised. Epochs carry the initiator's seat, dual initiation reads
the winner from the epoch, and BASELINE / POST / COMMIT are tracked per peer
(`cfg.occupied_mask` names the seats that answer). Still two-peer in shape:
the mod-set handshake settles on the first ack (the session keeps only the
latest), the hash chain keeps one peer ring (every peer's FRAME_COMMIT lands in
it), and the boot-digest gate reads that ring. No multi-peer harness exists;
do not claim N-player rollback until one does.

## Portable input contract

Pure decision core (no engine includes) for "published row vs late authoritative
row → rewind or promote?". All game-specific behavior enters through
`RNetInputContractParams` (numeric thresholds) and `RNetInputContractHostGates`
(optional host callbacks queried lazily in decision order).

Master entry point:

```c
RNetInputContractDecision d = rnet_input_contract_stick_replace_decide(
    &published, &wire, completed_sim, &params, &gates);
if (rnet_input_contract_decision_is_rewind(d)) { /* queue rollback */ }
```

Host-binding guidance (master-hash + savestate recomp host): bind only
`hash_confirm_promote` (= "peer master hash agreed through tick") and leave the
rest NULL (portable defaults).

## Invariants (soak-derived; do not relax without a new soak)

- Predicted rows never get a bare deadband promote — only `hash_confirm` or a
  host protect.
- Release always rewinds on both completed-sim and runway paths.
- Dash-gate X disagree blocks all same-intent promotes.
- `hash_confirm_promote` fails closed when NULL.

## Admission scheduler

`rnet_sched_bind` takes live pointers into the host session (`RNetSchedBridge`)
and game-specific gates (`RNetSchedGates`). Minimum for non-media digital
titles (SNES / NES):

| Gate | Required? | Notes |
|------|-----------|--------|
| `now_ms` | **yes** | Monotonic; retcomm-rbengine's `rbe_mono_ms` is suitable |
| `rtt_ms` | recommended | ICE/POST sample; 0 = synth from D |
| `episode_active` | recommended | 1 during Seal/Replay/Verify |
| `tip_holding` | recommended | TipHold Live invent-cap |
| media / lockstep | optional | MotK FMV only |

NULL media gates → never lockstep-stall invent; auto-D always samples.

Env knobs keep the `RBE_` names they had in retcomm-rbengine so existing tuning
and soak scripts keep working; MotK-era `PSX_RB_*` /
`PSX_NETPLAY_CROSS_OS_PACING_DIAG` names are honoured when the `RBE_*` one is
unset.

| Variable | Effect |
|----------|--------|
| `RBE_RB_ZERO_DELAY=1` | Legacy consume wire=`sim+D` (no cushion) |
| `RBE_RB_INVENT_GRACE_MS` | Floor ms before invent (default 8, clamped 0–200) |
| `RBE_RB_GAP1_GRACE_MS` | Flat gap=1 grace override |
| `RBE_RB_GAP1_INVENT=0` | Wait for tip-stale instead of gap1 invent |
| `RBE_RB_TIMESYNC=0` | Disable mispredict pacing debt |
| `RBE_RB_AUTO_DELAY=0` | Disable arrival-driven D controller |
| `RBE_RB_ADAPT_DELAY=0` | Disable pcap-freeze D bumps |
| `RBE_CROSS_OS_PACING_DIAG=1` | 1 Hz pacing diag line |

## Host guarantees for rollback mode

- Deterministic `advance_sim` for a given input set.
- Snapshot save/load at any sim tick the library requests.
- State digests suitable for agreement comparison across peers.
- Single-thread session ownership (same as delay-sync).

## Not in this library

- Automatch / lobby server (see `recomp-net-server`).
- Game-specific pad layouts, snapshots, or determinism fixes.
- Delay-sync behavior changes (existing `RNetSession` is untouched).
