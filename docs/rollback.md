# Rollback mode

recomp-net ships shared rollback alongside delay-sync on `main`. Delay-sync
`RNetSession` is unchanged; rollback is layered on top of it so hosts opt in
without breaking delay-sync titles. psxrecomp (MotK) and snesrecomp run
rollback matches on these modules today.

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
| N-peer agreement (3+ seats) | Landed | per-sender `rnet_session_take_rb_*_from`, `rnet_hc_init_n`, `rnet_rb_arbitrate_begin`, `RNetRbPeerAgree` — see [N-peer rollback](#n-peer-rollback-3-seats) |

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
follows) can derive any episode's initiator from its id. The tie-break is
implemented once in the library as `rnet_rb_arbitrate_begin` (see
[N-peer rollback](#n-peer-rollback-3-seats)); the BEGIN's sender seat comes
from `rnet_session_take_rb_sync_from`.

Hosts map their existing wire onto these when aligning transports (BattleShip's
soak-hardened `SYNETPEER_*` format stays authoritative for live matches); new
recomp-net rollback hosts may use `RNET_PKT_RB_*` natively. Transport/ICE
unification is a follow-up — the delay-sync ICE path is unchanged.

## Rollback episode orchestration

`RNetRbSession` owns the episode FSM (`Live → SealInputs → AwaitingBaseline →
Replay → Verify → Commit|Abort`), the correction tuple, the sealed input table,
and the resolved-through (shared frontier) watermark. The host owns snapshots,
the deterministic sim step, state digests, and the wire transport.

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

Required `RNetRollbackVTable` callbacks: `save_state` / `load_state` /
`advance_sim` / `get_input_row` (+ `state_digest`, `hash_confirm_through`).
Host stick gates ride through `stick_gates` and feed
`rnet_rb_decide_stick_replace`.

Minimal host loop during an episode:

```c
rnet_rb_begin_episode(s, &corr);                 /* mismatch identified */
rnet_rb_seal_inputs(s, corr.mismatch_tick, corr.target_tick, corr.slot);
/* exchange peer seal rows via host transport: rnet_rb_export_seal_rows_chunk /
 * rnet_rb_apply_peer_seal_rows until rnet_rb_all_peer_seal_rows_complete */
rnet_rb_set_phase(s, nRNetRbPhaseAwaitingBaseline);
vt.load_state(vt.ctx, corr.load_tick);
for (t = corr.load_tick; t <= corr.target_tick; ++t)
    vt.advance_sim(vt.ctx, t);                   /* reads sealed rows */
/* compare vt.state_digest against peer; then: */
rnet_rb_on_post_match(s);                        /* or rnet_rb_on_post_diverge */
```

The episode FSM is transport-agnostic: hosts call these entry points from
their own packet ingress. BattleShip's `netpeer.c` does this with its own wire.
recomp-net hosts use the `RNET_PKT_RB_*` opcodes above, which RNetSession
carries over LAN, the LAN hub, the lobby relay, and ICE alike.

## N-peer rollback (3+ seats)

Delay-sync play already scales to `RNET_MAX_SLOTS` (8) seats over the lobby
relay star or the LAN hub. Rollback agreement did not: it was written for one
peer, and three things silently assumed it.

| Assumption | Effect at N > 2 | Fix |
|---|---|---|
| `RNetHashConfirm` kept one peer digest per tick | The last FRAME_COMMIT to land for a tick overwrote the others; the watermark advanced on a 1-of-N match and a diverged seat could be masked by a healthy one | Per-seat rings; a tick resolves only when every expected seat reported it equal to local |
| Rollback queues (FRAME_COMMIT, SYNC, SEAL_ROWS, BASELINE, POST, RESOLVED) had no sender | A host could not tell which seat a digest, frontier or BEGIN came from | Every entry records the sender; `*_from` takes return it |
| Peer-seal completion waited on every seat in `[0, slot_count)` | A sparse room (seats 0+2 of 4) waited forever on the empty seats | `RNetRbConfig.occupied_mask` |

None of this changes the wire: every packet already carries the sender's
`local_slot` as the first body byte, and that is what the queues record. Two
seated peers see no behaviour change; the pinning tests prove the legacy and
N-way trackers are observably identical with one peer.

### Who must agree

`rnet_expected_peer_mask(slot_count, occupied_mask, local_slot)`
(`recomp_net/config.h`) is the one definition: every occupied seat except your
own. An observer (`local_slot == slot_count`) waits on all occupied seats.
Spectators never appear — they have no seat, and their wire ids sit above the
seat range, so every N-way API ignores them. The rollback FSM
(`rnet_rb_expected_peer_mask`) and the hash-confirm tracker use the same
function, so "who must agree" cannot be computed two ways.

### Hash-confirm watermark

```c
rnet_hc_init_n(&hc, rnet_expected_peer_mask(slots, occupied, local));
/* each tick */
rnet_hc_note_local(&hc, tick, master_digest);
while (rnet_session_take_rb_frame_commit_from(s, &from, &t, &h))
    rnet_hc_note_peer_slot(&hc, from, t, h);
if (rnet_hc_peek_mismatch_slot(&hc, &t, &seat, &mine, &theirs))
    /* stop: `seat` diverged at `t` */;
```

- A tick resolves only when every expected seat reported it and every digest
  equals local. The watermark is therefore the minimum over seats.
- A mismatch from **any** seat is a stop, even before the other seats report.
  `rnet_hc_peek_mismatch_slot` names the lowest mismatching seat.
- `rnet_hc_note_peer` (no sender) is ignored by an N-way tracker, and
  `rnet_hc_note_peer_slot` is refused by a legacy one. An unattributed digest
  can never satisfy a seat.
- An N-way tracker with an empty mask never resolves (fail closed).
- `rnet_hc_set_peer_mask` handles a seat leaving mid-match. Digests are kept
  and the watermark is re-evaluated.
- `rnet_hc_peer_digest` on an N-way tracker is a consensus view that keeps an
  N=2-shaped consumer correct. It returns the first seat's digest that differs
  from local as soon as one exists. Otherwise it returns the shared digest
  once every seat has reported.
- `rnet_hc_heal_stale_gap` jumps only to a later tick that every seat reported
  equal. It refuses if any later tick shows any seat disagreeing.
- Same-tick re-delivery is latest-wins in every ring, exactly as the legacy
  cell was. A seat that re-simulates and re-sends a corrected digest therefore
  clears its own mismatch.

`rnet_hc_reset` still builds the legacy tracker, with one implicit peer fed by
`rnet_hc_note_peer`. Its behaviour is unchanged.

### Episode initiator arbitration

With two seats the initiator is "me or the other seat". With three, two seats
can open episodes concurrently and a third can be asked to follow both. The
initiator's seat is therefore recorded as part of the episode:
`rnet_rb_begin_episode_from(s, &corr, initiator_seat)`, read back with
`rnet_rb_get_initiator_slot`. Plain `rnet_rb_begin_episode` records
`local_slot` when initiating and `RNET_RB_SLOT_NONE` when following, which is
the unchanged N=2 behaviour.

For an inbound BEGIN that is not a tip-extend (`FLAG_REREPLAY` is handled
first; it is never a new episode), `rnet_rb_arbitrate_begin(s, sender, epoch)`
returns one of four answers:

| Result | When | Host does |
|---|---|---|
| `Follow` | idle (Live, or a finished Commit/Abort) | open a follow episode with `begin_episode_from(sender)` |
| `SameEpisode` | same initiator seat and epoch as the held episode | nothing (duplicate) |
| `Yield` | busy, and `sender` is lower than the current initiator (ours or the one we follow) | tear ours down locally, then follow `sender` |
| `Refuse` | busy and the current initiator outranks `sender`; initiator unknown; or `sender` is our own seat | reply `RB_SYNC NACK` so the sender aborts immediately |

Yield sends no ABORT. The lower seat's BEGIN reaches every seat, and each one
yields for itself. Every seat applies the same rule to the same facts, so the
room converges on the lowest initiating seat without another round trip.
`rollback_nway_test` pins this over 2000 random delivery orders of two
concurrent BEGINs in a four-seat room. At N=2 the rule is exactly the one hosts
already implemented by hand.

### Seal rows, frontier, BASELINE/POST

- **Seal rows.** Each seat owns its own row, so a follower collects
  SEAL_ROWS from every seat. `rnet_rb_all_peer_seal_rows_complete` waits on the
  expected mask. Set `RNetRbConfig.occupied_mask`, or call
  `rnet_rb_set_occupied_mask` when a seat leaves. The field is appended, so a
  zero-initialised config keeps the old "every seat in range" behaviour.
- **Shared frontier.** `rnet_rb_set_peer_convergence` is max-wins and is only
  correct with one peer. Feed RB_RESOLVED through
  `rnet_session_take_rb_resolved_from` into
  `rnet_rb_note_peer_resolved(s, seat, tick)`. That call keeps each seat's
  latest frontier and advances to the minimum once every seat has advertised.
  It never demotes; use `rnet_rb_demote_resolved_through` for that.
- **BASELINE / POST.** `RNetRbPeerAgree` handles one checkpoint, keyed by
  `(epoch, tick)`, over up to four digest words. Begin it with the expected
  mask, set the local words, and note each `take_rb_baseline_from` /
  `take_rb_post_from` report. The status is `Mismatch` as soon as any
  reporting seat differs; it names the seat and the word. The status is
  `Match` only when every seat has reported equal. A report with a different
  epoch or tick is rejected, and the host decides whether to hold it.

### Episode scoping

`rnet_session_set_rb_peer_slot` (single seat, PSX-Link groups) is joined by
`rnet_session_set_rb_peer_mask`, which accepts episode packets (SYNC,
BASELINE, POST, STATE_*) from a set of seats. FRAME_COMMIT, SEAL_ROWS and
RESOLVED stay session-wide. The two setters replace each other.

### Queue depth

The session's FRAME_COMMIT queue (64) and control queues (8) were sized for
one sender. They now scale by `RNET_MAX_SLOTS - 1`, so each remote seat keeps
the headroom one peer had at N=2.

### Migrating an N=2 host

1. Drain with the `*_from` takes (the legacy takes drain the same queues, so
   use one family, not both).
2. `rnet_hc_init_n(&hc, rnet_expected_peer_mask(...))` instead of
   `rnet_hc_reset`, and `rnet_hc_note_peer_slot` instead of `rnet_hc_note_peer`.
3. Set `RNetRbConfig.occupied_mask`. Use `begin_episode_from` with the BEGIN
   sender, and `rnet_rb_arbitrate_begin` in place of the hand-written
   dual-initiation rule.
4. Replace `set_peer_convergence` with `note_peer_resolved`, and single-peer
   BASELINE/POST compares with `RNetRbPeerAgree`.
5. Stop pinning `rnet_session_set_rb_peer_slot` to "the other seat". Leave it
   at -1, or use a mask.

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
