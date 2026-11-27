# Echoes and the existing Recomp Net service

Source assessment, 2026-11-26. **An experimental Echoes client can reuse the
existing lobby/signaling and TURN service. A new game-specific relay server is
not required by local rollback.** A playable rollback build still requires
client/engine work and game validation.

## Why the service can be reused

Reviewed `recomp-net-server` upstream `a39d1d4`:

- `README.md` separates lobby/signaling from gameplay traffic. The server
  does not execute the game and no longer provides the old gameplay UDP SFU.
- `src/ws_lobby.rs` treats `game_version` as an opaque exact-match room pin
  and refuses `version_mismatch`/`game_mismatch`. Experimental builds can use
  a distinct version without changing the server's gameplay protocol.
- The existing room contract supports host relay over ICE with
  `relay=host, relay_via=ice`. Signaling forwards connectivity messages.
- `docs/COTURN.md` describes the existing credential service and separate
  Coturn process. TURN forwards datagrams, independently of the rollback
  protocol inside them. Wire 4 does not need a new TURN implementation.

This is source-supported compatibility. It does not verify which server
revision/configuration the deployed service currently runs, its capacity, or
a live experimental match. No production configuration was changed.

## What Echoes has

The relevant consumer is `gcnllr/MetroidPrime2Echoes`, the original GameCube
G2ME01 revision 0 title. The Wii Trilogy project is a separate consumer.

The current consumer has a recomp-ui Online/LAN/Direct-IP launcher, verified
disc/firmware identities, isolated network boot settings, synchronized
controller seats and independent player views. Existing private technical
evidence demonstrates a two-process LAN delay-sync match and independent
movement; it does not qualify the new rollback mode.

Its current function-native runtime uses HLE DSP and the retail Aurora renderer.
`gcnllr/src/recompcore/netplay.c` sends an 8-byte GameCube pad or a 22-byte row
with keyboard/mouse data. The opaque 32-byte local API can carry either after
the experimental host chooses one fixed layout for every seat. Relative fields
need a predictor that clears deltas while retaining held buttons and axes.

## Concrete gaps in the current client

1. `launcher/main.cpp` explicitly rejects `transport_ice_hub` and `rollback`,
   forces delay-sync caps and removes the corresponding callbacks. The runtime
   starts raw LAN UDP. The experimental launcher/runtime must support the
   existing ICE/host-relay handoff; changing the server cannot fix that client
   rejection. Keep a single session/ICE owner or use an explicit cross-process
   data/signaling bridge; an ICE-agent pointer cannot be passed to the child
   process as a handoff.
   Also, the fetched library's waiting-room `host_ice` path explicitly creates
   agents without TURN. The existing service can supply TURN, but that hub
   client path needs credential/configuration and fallback coverage before a
   forced-TURN test can pass. Do not infer that coverage from direct ICE.
2. `gcnllr/src/recompcore/CMakeLists.txt` restricts current game checkpoints to
   block-native dispatch, synchronous eager packet GX, no function-native
   sources, no resident XFB and no FIFO worker. The accepted function-native
   retail renderer therefore cannot simply enable this checkpoint flag.
3. `machine_state.inc` currently drains graphics and copies MEM1 and ARAM
   along with device/audio/renderer state. `machine_rollback.inc` holds 16
   snapshots and uses the episode driver. This is a starting reference,
   not a measured low-cost snapshot/replay provider for the fast runtime.
4. Function-native continuations, HLE DSP/audio queues, GPU-owned memory and
   renderer resources need a complete current-build rewind contract. Preserve
   native in-game saves and keep experimental states isolated. Old save-state
   compatibility is unnecessary.
5. The current dependency has GameCube-specific episode pad extensions
   (`lt`, `rt`, exact-pad policy) absent from the newly fetched upstream base.
   A plain dependency pin swap is not sufficient. Adapt to the opaque local
   API while retaining a valid episode A/B path with the full input payload.

## Recommended branch proof

Use matching experimental consumer and engine branches with the experimental
library as a pinned submodule. Keep the existing accepted build intact.

First establish an HLE snapshot/restore and replay contract on one repeatable
two-player Sidehopper Station match in the function-native retail runtime.
Measure save/load/digest/replay cost and confirm complete pad/mouse transport,
identical confirmed state and independent inputs. Choose delay/depth from the
actual replay budget, not the game's name or an assumed frame rate.

Then compare episode/local modes under the same built workload. Exercise the
existing service's direct ICE path and forced TURN separately, retaining the
wire/lobby experiment identity. Keep 3–4 players, broad cross-OS qualification
and the long-distance human shooter gate explicit until tested.

If ICE/TURN deployment lacks a required region, credentials or capacity, that
is a deployment change to the existing service. A new server architecture is
only justified by a separately chosen server-authoritative game model or a
different relay product; neither is required by this deterministic rollback
design.
