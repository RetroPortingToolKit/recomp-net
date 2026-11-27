# Feasibility across the planned consumers

Read-only source assessment, 2026-11-26. This covers migration feasibility,
not qualified game builds. Revisions below identify inspected local checkouts;
only recomp-net and recomp-net-server were refreshed from upstream for this task.

**The existing lobby/signaling/TURN service is sufficient for the deterministic
local-rollback design across these systems.** Engines need new client adapters
and qualification. GBA mixed-cartridge discovery/admission needs an extension
to the existing lobby contract; it does not need a new gameplay relay server.

| Consumer / inspected revision | Existing foundation | Required work / principal risk | Relative first-proof cost |
| --- | --- | --- | --- |
| NES `f649ff5` | Shared episode driver, tick-keyed snapshots, incremental replay and explicit native continuation handling | Bind opaque local rows, preserve stack-discard/resume boundary, input-consumption signals, audio suppression, fixed startup mode and reports | Lower |
| Genesis `398ddbf` | Shared episode driver, returning `genesis_sim_step`, inline replay, snapshot ring, replay audio isolated from the speaker stream | Bind local driver without restoring live host presentation state; include all pad ports and simulation-affecting mods; new profile/wire pins | Lower |
| SNES `8dc869cf` | Returning `RtlRunFrame`, rollback save/load/digest APIs and blob snapshot ring | Inspected adapter retains its own large episode FSM, not just shared-driver callbacks. Replace that ownership, retain MSU/APU state, avoid scratch-to-allocated snapshot copy, then qualify HLE paths/title enhancements | Medium |
| GBA replicated simulation `92b26b2` | `GbaNetplayHost` snapshot callbacks, whole linked-machine simulation, cable/RFU state and native saves | Bind local driver to the entire linked simulation, not only the controlled GBA. More linked machines increase replay cost. Preserve machine roster, serial event order, RTC/save ownership and boot agreement | Medium |
| N64 `8b4e0ca9` | Rust shared-driver adapter, incremental field replay, dirty-page whole-machine rollback ring and input/audio hooks | New Rust FFI and opaque-row adapter; qualify the selected HLE graphics/audio state and digests. Removing rendering work is per-title and observer-dependent, never a blanket software/HLE switch | Higher |
| PSX `93a93c8e0` | Extensive existing rollback/runtime APIs, safe resume handling and raw boot-state snapshots with snapshot timing | Inspected adapter retains substantial orchestration. Snapshot serializer/device/GPU/SPU/CD ownership, native continuation and frame cost are the main risks; replace full RAM copies only with complete write tracking | Higher |
| GameCube Echoes | Working HLE delay-sync and independent views; separate checkpoint experiment | Fast function-native retail renderer is excluded by current checkpoint configuration; complete rewind for native continuations, HLE audio and GPU ownership, plus ICE process handoff | Higher; see [Echoes assessment](echoes-integration.md) |

These are engineering-risk categories, not elapsed-time estimates. Historical
source comments about fast replay do not prove a current HLE game's frame
budget. Every candidate still needs built measurements and per-game results.

## What supports those judgments

- NES `runner/src/netplay/nes_netplay_rb.c` binds `RNetRbDriver`, rbengine
  snapshots and resim hooks. Its documented incremental model discards the
  stale native stack and resumes a captured guest continuation. Preserve that
  boundary when moving correction policy; a C stack/fiber byte copy is not an
  adequate portable replacement.
- Genesis `runner/netplay/genesis_netplay_rb.c` supplies the shared driver's
  callbacks. `run_tick` returns and replay retains VDP status/simulation work
  while suppressing scanout/audio delivery. That already fits local inline
  replay; current timing still needs measurement.
- SNES `runner/src/netplay/snes_netplay_rb.c` owns sealing/verifying state
  directly in the inspected revision. `rb_snap_serialize` first saves into
  scratch, allocates a second buffer, then copies it into the blob ring.
  `RtlRunFrame` is the reusable live/replay boundary.
- PSX `runtime/src/psx_netplay_rb.c` retains substantial title/runtime episode
  and resume policy. `netplay_snap_ring.c` uses
  `boot_state_save_buffer_raw` and already reports snapshot time/size and VRAM
  dirty-row statistics. Start there before creating another profiler.
- N64 `crates/n64lle-host-common/src/netplay_rb.rs` is the shared-driver Rust
  adapter. `crates/n64lle-rt/src/state/rollback.rs` retains a base plus dirty
  post-images, tracks related hidden-plane data, restores host pad state and
  preserves cartridge/peripheral progress in the rollback domain.
- GBA `src/runtime/multiplayer_netplay.cpp` saves the complete simulation and
  exposes an `RNetRbHost`. Link/RFU state is serialized separately in the cable
  and wireless owners. Correcting one host's pad still requires replaying the
  coupled local consoles.

## rbengine and shared dirty-page snapshots

The inspected rbengine `a7b98507a62fe00e5aec3b90c52a4134f3c174bc` provides a
tick-keyed owned-blob ring in `src/snap/rbe_snap_ring.c`. It is storage, not
dirty tracking. The N64 ring is a useful mechanism to generalize, not a C ABI
or machine-state layout to transplant verbatim.

A shared provider can own base/dirty-page retention, eviction, rollback and
allocation. Each engine must supply memory ranges, deterministic device state,
snapshot boundaries and complete dirty notifications from **all** writers:
native CPU stores, DMA, GPU readback, HLE pointer writes and mods. Missing a
writer silently restores the wrong state. Immutable source-image identity
does not prohibit valid in-memory image/RAM writes; those belong in the
snapshot/mod contract when simulation-visible.

Prove the provider on one existing host, then reuse it. Keep a portable
implementation; OS page-protection optimizations cannot be the sole backend.
Native save files remain separate from disposable current-build snapshots.

## recomp-ui, module and room contracts

The shared UI backend (`src/netplay/recomp_netplay_host.c`) currently exposes
rollback as a boolean and a room-wide delay. The experiment needs an explicit
episode/local selection, per-player delay/depth preview, experimental identity,
saved match reports and the visible diagnostics mod's recording preference.
It must preserve seat-to-controller mapping and settled mod plans.

The server already passes unknown `match_caps` keys through. A versioned mode
key can use the host caps codec instead of changing the server's simulation
protocol. Every occupied peer must confirm the same mode/profile before frame
0. Existing build/content checks remain; do not equate a CPU feature bit with
deterministic qualification. The library's loadable module must expose the new
queries and driver symbols with an updated loader minor contract.

The current host-relay ICE implementation does not configure TURN. Regional
TURN selection and per-pair fallback need client implementation and deployment
capacity checks. The existing server's seated `signal` routing accepts a
target player within a room, which is a useful foundation for mesh signaling;
the session transport itself still needs a real per-pair mesh. A host relay
test is not evidence of a mesh.

## GBA: one original cartridge per player

This is the highest semantic-risk part and is independent of pad rollback.
The existing replicated-machine mode is feasible to migrate as above but
continues to require every locally simulated cartridge/program. It cannot
meet the own-cartridge-only requirement through a pad-format change.

`GbaLinkHub` currently starts a transfer by reading every local port's send
register and completes only when all local port cycle counters equal the
completion cycle. A remote proxy therefore needs explicit serial participant,
clock/event and restart semantics. It is not an existing pluggable network
endpoint. Received cable traffic must arrive before the guest can observe
completion; freezing emulated time can hide network wait, but does not by
itself prove Pokémon's checksum/burst phase or restart behavior correct.

`GbaWirelessDomain::transmit` directly copies parent/child buffers between
local `Port` objects. Its saved associations refer to those local port indices.
Splitting it requires a real radio-domain interface and remote association,
acknowledgment/notification and delivery-order semantics.

A reliable ordered bounded message channel on the existing ICE/UDP transport
is feasible, but neither unordered chat nor the current single state-transfer
workflow is already a general bidirectional channel. Use separate sequence,
acknowledgment, retransmission, backpressure and session-epoch semantics.
It must work without starting the rollback driver.

The current lobby joins by strict game/version/disc identity.
`docs/MIXED_CARTRIDGE_MULTIPLAYER.md` explicitly retains the replicated model
and both-cartridge requirement. Distributed linking needs a separately
versioned compatibility-family/per-seat cartridge negotiation and admission
path in the **existing** server/client/UI. Do not spoof equal game/disc hashes
or disable the strict checks for ordinary rooms. Own-cartridge linking must
not upload the other player's native save.

The deciding spike is two distinct cartridges, one local console per process,
with real serial traffic and injected delay/loss: cable club entry, trade,
battle, valid native saves, checksum/queue/restart behavior, then timed
activities and 3–4 players. Wireless follows cable. Until that passes, the
plan's burst framing/checksum rewrite and the feel of timed activities remain
hypotheses, not established compatibility.

## Suggested proof order

1. Qualify the shared library's synthetic local/episode comparison and module
   API, then use one cheap existing NES or Genesis host for the first HLE game
   A/B and report. Keep individual game gates.
2. Migrate SNES/GBA callbacks; prove and share dirty-page storage where actual
   copy/replay measurements justify it.
3. Qualify PSX/N64 HLE replay and rendering observers, and address Echoes'
   function-native checkpoint boundary. These cannot borrow the cheap-core
   latency claim.
4. Add UI previews/reports, real per-pair transport/fallback and required
   cross-OS/long-running tester qualification.
5. Judge GBA distributed cable/RFU independently from the local-rollback merge
   gate. Upgrade existing lobby metadata only when the prototype establishes
   the compatibility contract.

No other engine was modified or rebuilt for this assessment. No new relay
deployment, gameplay pass, speedup or completed multi-repository rollout is
claimed.
