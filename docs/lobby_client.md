# Lobby client (`recomp_net_lobby`)

`include/recomp_net/lobby_client.h` is the one client for the
recomp-net-server WebSocket lobby (`docs/WS_LOBBY.md`, `docs/AUTOMATCH.md`,
`docs/MODERATION.md` in recomp-net-server). It replaces three engine copies —
snesrecomp's `snes_lobby_client`, psxrecomp's `psx_lobby_client` and
segagenesisrecomp's `genesis_lobby_client` — with their superset, and turns
every console-specific constant into configuration.

## Target

The client is its own static library, **`recomp_net_lobby`**
(`recomp_net::recomp_net_lobby`), not part of `recomp_net` core:

- it starts a thread (the connect worker: DNS + TCP + WebSocket upgrade off
  the UI thread), so it links `Threads::Threads`;
- each handle carries ~1 MB of state (receive buffers, seat rows with mod
  offers, chat rings, rulesets), which LAN-only consumers of `recomp_net`
  should not pay for.

It links `recomp_net` publicly and reuses the library's WebSocket framing
(`rnet_ws.h`), chat filter, chat report builder, account session (`auth.h`),
ICE blob transfer (`ice_xfer.h`), ICE/UDP RTT probes, LAN beacon and STUN
helpers. It vendors nothing.

```cmake
target_link_libraries(my_runner PRIVATE recomp_net::recomp_net_lobby)
```

With `RNET_ENABLE_ICE=ON` (propagated from `recomp_net`) the waiting-room ICE
RTT probe and the peer-to-peer mod transfer are live; without it they compile
to the library's stubs and the rest of the client is unaffected.

## Model

- **Handle-based.** `rnet_lobby_open(&lobby, &cfg)` / `rnet_lobby_close(&lobby)`.
  Several handles may coexist (tests do).
- **Single-threaded per handle.** Every call on a handle comes from one thread.
  The connect worker owns a private copy of what it needs and never touches the
  handle; `close()` / `disconnect()` abandon a worker still resolving without
  blocking (it frees itself).
- **Poll-based.** Call `rnet_lobby_pump(lobby)` every frame. It drives the
  socket, the connect worker, the mod transfer, the automatch relay probe and
  every latency probe. Nothing happens between pumps.
- **Send calls** return 0 when the frame was handed to the socket (or held
  until the handshake completes), `<0` when refused locally. Handed over is not
  accepted: the server's answer arrives through the pump.
- **Configuration survives reconnects** (identity, hooks, gallery preference,
  block set, ready extras); everything a connection owns is wiped on connect
  and disconnect.

## What the engine supplies

### 1. Identity and policy: `RNetLobbyConfig`

Call `rnet_lobby_config_init(&cfg)` first; it fills every field with the
documented default. Strings are copied at open.

| Field | Meaning | snesrecomp | psxrecomp | genesis |
|-------|---------|-----------|-----------|---------|
| `game_name` | title; scopes list, server chat, players online | title | title | title |
| `game_version` | release pin (`NULL` → `"dev"`) | build pin | build pin | build pin |
| `build_id` | exact build, logged beside the pin | commit+dirty | — | — |
| `platform` | chat-report metadata | `"snes"` | `"psx"` | `"genesis"` |
| `content_fp` | 64-hex SHA-256 of the image | ROM hash | disc TOC fp | ROM hash |
| `log_prefix` | default log prefix | `"snes_lobby"` | `"psx_lobby"` | `"genesis_lobby"` |
| `max_players` | player seats (≤ 8) | 4 | 8 | 2 |
| `max_spectators` | gallery seats (≤ 8) | 4 | 4 | 0 |
| `default_max_slots` | create's seat count | 2 | 2 | 2 |
| `url_env_var` | env override of the URL | `SNES_NET_LOBBY_URL` | `PSX_NET_LOBBY_URL` | `GENESIS_NET_LOBBY_URL` |
| `version_env_var` | env override of the pin (testing) | `SNES_NET_GAME_VERSION` | — | — |
| `default_url` | `NULL` → `ws://netplay.retcomm.net:8765` | — | compile-time default | `ws://netplay.technicallycomputers.ca:8765` |
| `blocking_connect` | 1 = `connect()` blocks | 1 (adapter expects `connected()` right after) | 0 | 1 |
| `auto_ready` | re-arm Ready (no Ready UI) | 1 | 0 | 0 |
| `require_server_relay` | refuse `transport=ice_p2p` launches | 0 | 1 | 0 |
| `host_bind_all_interfaces` | host binds `0.0.0.0:<port>` | 1 | 0 | 1 |
| `fingerprint_in_rooms` | `disc_fp` on create/join | 0 (historic wire) | 1 | 0 |
| `waiting_room_rtt` | seat latency method | `WS_SIGNAL` | `PEER_PATH` | `OFF` |
| `list_latency`, `lan_beacon`, `host_advertise` | list RTT / LAN beacon / STUN advertise | 0 | 1 | 0 |
| `caps_input_delay_min/max/default` | caps clamp / absent value | 2 / 20 / 6 | 0 / 20 / 6 | 0 / 16 / 2 |
| `caps_rollback_default` | absent `rollback` | 1 | 0 | 0 |
| `caps_input_prediction_default` | absent `input_prediction` | 0 (unused) | 10 | 0 |

The "snesrecomp/psxrecomp/genesis" columns reproduce each engine's historic
behaviour. The defaults from `config_init` are the full-featured set
(`PEER_PATH`, list latency, LAN beacon, host advertise, fingerprint in rooms);
a title opts out where it must stay wire-compatible with older builds.

`fingerprint_in_rooms` needs care: the server treats one empty side as a
mismatch, so a build that sends `disc_fp` cannot share a room with a build
that does not. Flip it for a title only when every shipped build of that title
sends it.

Hooks in the config:

- `log(user, level, line)` — default: `stderr` as `"<prefix>: <line>"`;
  `log_min_level` filters (default INFO).
- `session(user)` — the account session token for `hello`; default
  `rnet_account_session()` from `auth.h`.

### 2. The title's match settings: `RNetLobbyMatchCaps.game_json`

The library owns only the keys it acts on: `v`, `input_delay`,
`input_prediction`, `rollback`, `force_turn`, `force_input_relay`, `mod_plan`,
`mod_set`, `mod_cosmetic_allow`. Everything else belongs to the title and is
carried verbatim in `game_json`, a JSON **member list without braces**:

```c
RNetLobbyMatchCaps caps;
rnet_lobby_match_caps_init(lobby, &caps);
caps.valid = 1;
caps.input_delay = delay;
caps.rollback = 1;
snprintf(caps.game_json, sizeof(caps.game_json),
         "\"widescreen\":%s,\"widescreen_hud\":%s,\"ws_extra\":%d",
         ws ? "true" : "false", hud ? "true" : "false", extra);
rnet_lobby_create(lobby, "My room", NULL, NULL, password, bind, &caps, 0);
```

On receipt (`created`, `joined`, `lobby_update`, `launch`, rulesets, automatch
offers) every member the library does not own lands back in `game_json`, and
the whole object in `json`. Read the title's keys with the helpers:

```c
const RNetLobbyMatchCaps *mc = rnet_lobby_match_caps(lobby);
int ws  = rnet_lobby_json_get_bool(mc->game_json, "widescreen", 0);
int ext = rnet_lobby_json_get_int(mc->game_json, "ws_extra", 0);
```

`rnet_lobby_json_get_{str,int,bool,raw}` take an object or a member list and
look up TOP-LEVEL keys only. `rnet_lobby_match_caps_encode/decode` are public
too (a LAN lobby can carry the same blob). Encode refuses (returns 0, publishes
nothing) rather than emit a short plan or exceed the server's 4096-byte limit.

`force_input_relay` in the caps is the HOST's toggle. The transport a launch
actually got is `RNetLobbyJoinInfo.force_input_relay` — read that.

### 3. Per-seat extras: `rnet_lobby_set_ready_extra_json`

Members attached to every `set_ready`; the server echoes the keys it knows
(`bios_offer`, `memcard_offer`, `mod_offer`) into each seat's row. Each row is
available verbatim through `rnet_lobby_member_json(lobby, index)`.
psxrecomp's BIOS / memory-card offers and its session-BIOS settle live in the
PSX adapter on top of these two calls:

```c
rnet_lobby_set_ready_extra_json(lobby,
    "\"bios_offer\":{\"v\":1,\"prefer\":\"openbios\","
    "\"can_openbios\":true,\"can_scph1001\":false}");
/* settle: for each member */
char offer[256], prefer[16];
if (rnet_lobby_json_get_raw(rnet_lobby_member_json(lobby, i), "bios_offer",
                            offer, sizeof(offer)))
    rnet_lobby_json_get_str(offer, "prefer", prefer, sizeof(prefer));
```

### 4. Mod hooks (optional)

- `rnet_lobby_set_mod_offer_supplier(fn, ctx)` — what this build has. Without
  one every host reads this peer as having nothing and its launch gate holds
  the match (the safe direction).
- `rnet_lobby_set_mod_transfer_hooks(export, free, install, ctx)` — pack a
  package into a `malloc()`ed archive + SHA-256; verify the digest BEFORE
  unpacking on install. Transfers run peer-to-peer over their own ICE agent;
  the server relays only SDP/candidates (signal types 110, 111, 120+type). A
  relayed pair is capped at 5 MiB (`rnet_lobby_mod_relay_size_allows`).

### 5. The pump, and the launch

```c
rnet_lobby_pump(lobby);                         /* every frame */
if (rnet_lobby_launch_pending(lobby)) {
    RNetLobbyJoinInfo ji;
    if (rnet_lobby_try_fill_launch(lobby, &ji)) {
        /* ji.bind_hostport / ji.peer_hostport / ji.session_id /
         * ji.local_slot / ji.force_input_relay / ji.local_is_spectator;
         * a spectator uses rnet_lobby_local_wire_slot() as RNetConfig.wire_slot
         * and must not launch when it is -1. */
        rnet_lobby_clear_launch_pending(lobby);
    }
}
```

After a soft return to the lobby, `rnet_lobby_resume_waiting_room_rtt()` lets
the waiting-room probes run again (a launch suspends them so they cannot steal
the match's ICE signals).

Gameplay ICE bridging: `rnet_lobby_poll_signal()` yields the peer's signals as
it emitted them (`LOCAL_*` — remap to `REMOTE_*` before
`rnet_session_push_signal`); `rnet_lobby_send_signal()` relays ours. Signals
from spectators, and every signal while this client spectates, are dropped:
the session owns one ICE agent and a third party's SDP reads to it as a peer
ICE restart. `rnet_lobby_turn_credentials()` supplies the server's TURN mint
(requested automatically on `welcome`).

## API overview

| Area | Calls |
|------|-------|
| lifecycle | `config_init`, `open`, `close` |
| connection | `default_url`, `connect`, `disconnect`, `connected`, `connecting`, `ready`, `url`, `pump` |
| identity | `set_display_name`, `display_name`, `accepted_name`, `name_refused`, `session_invalid`, `player_id`, `set_game_identity`, `game_name`, `game_version`, `version_filter_strict`, `version_is_release`, `set_fp`, `fp` |
| list | `request_list`, `list_count/get`, `online_count/get` |
| caps | `match_caps_init/encode/decode`, `match_caps`, `set_match_caps` |
| rooms | `create`, `join`, `leave`, `set_max_slots`, `in_lobby`, `is_host`, `host_player_id`, `join_info`, `clear_last_error` |
| members | `member_count/get/json`, `member_latency_ms`, `member_is_host`, `local_ready`, `all_ready`, `set_ready`, `set_ready_extra_json`, `kick`, `move`, `seat_move_self`, `seat_swap_request/incoming/respond/outgoing/clear` |
| gallery | `set_allow_spectators`, `allow_spectators_pref`, `allow_spectators`, `max_spectators`, `spectator_count`, `local_is_spectator`, `spectator_slot_base`, `seat_valid`, `spectator_slot`, `local_wire_slot` |
| chat | `send_chat`, `chat_count/get/clear`, `send_server_chat`, `server_chat_count/get`, `report_chat`, `last_report_ack`, `set_blocks` |
| launch | `request_start`, `launch_pending`, `clear_launch_pending`, `try_fill_launch`, `resume_waiting_room_rtt` |
| signals | `send_signal_to`, `send_signal`, `poll_signal`, `clear_signals`, `set_ice_signal_accept` |
| TURN | `request_turn_credentials`, `turn_credentials` |
| mods | `set_mod_offer_supplier`, `need_mods_count/get/can_transfer`, `match_blocked_by_mods`, `local_missing_mods`, `set_mod_transfer_hooks`, `mod_relay_size_allows`, `mod_request`, `mod_cancel`, `mod_progress`, `mod_failed`, `mod_in_flight` |
| desync | `report_desync` |
| automatch | `automatch_request_rulesets`, `automatch_available`, `automatch_ruleset_count/get`, `automatch_queue`, `automatch_cancel`, `automatch_state`, `automatch_queued_secs`, `automatch_pool`, `automatch_found_get`, `automatch_found_caps`, `automatch_accept`, `automatch_room`, `automatch_refuse_local`, `automatch_error`, `automatch_rtt_ms` |
| JSON | `json_get_str/int/bool/raw`, `json_escape` |

All are `rnet_lobby_*` and take the handle first (except the pure helpers).

## Porting an engine

1. Delete the engine's `*_lobby_client.{c,h}` and its `lobby/ws` copy of
   `rnet_ws` / `rnet_sha1`; link `recomp_net::recomp_net_lobby`.
2. Keep one `RNetLobby *` in the host layer; open it at startup with the
   config above, set hooks, `rnet_lobby_set_fp()` once the image is verified.
3. Mechanical rename in the adapter: `snes_lobby_X(...)` / `psx_lobby_X(...)`
   → `rnet_lobby_X(lobby, ...)`. Renamed calls: `psx_lobby_move_member` →
   `rnet_lobby_move`; `*_set_disc_fp` / `*_disc_fp` → `rnet_lobby_set_fp` /
   `rnet_lobby_fp`. Types: `SnesLobbyX` / `PsxLobbyX` → `RNetLobbyX`.
4. Move the title's caps fields into `game_json` (write on create /
   set_match_caps / start, read with the JSON helpers).
5. psxrecomp: move `bios_offer` / `memcard_offer` and
   `psx_lobby_settle_session_bios` into the adapter (section 3), and settle
   `session_bios` into `game_json` before `rnet_lobby_request_start`.
6. The recomp-ui launcher adapter (snesrecomp `snes_host_lobby.c`) uses 82
   distinct lobby entry points; every one exists here under the renamed prefix.

## Where the engine copies disagreed

| Topic | snesrecomp | psxrecomp | here |
|-------|-----------|-----------|------|
| `automatch_found` | flat `opponent`/`label` (read empty against the server) | `opponent` object, `ruleset_label`, `match_id` | object first, flat fallback; floored delay/prediction/caps parsed |
| `automatch_accept` | `ticket_id` | `match_id` | both |
| requeue / cancelled | requeue always re-queues | `queued:false`, cooldown line | psxrecomp |
| error claim list | — | + `mod_not_approved` | psxrecomp |
| lobby list version filter | missing version rewritten to `dev` first (hid old servers' rooms from releases) | kept | kept; release rule = no `+` qualifier and not `dev*` |
| error → `join.ok` | cleared when not seated | + fatal code list | both |
| `lobby_update` | auto re-ready | host migration (`is_host` from `host_player_id`) | both; `in_lobby` set only when the update seats us |
| connect | blocking | async worker | both (`blocking_connect`) |
| host bind | `0.0.0.0:<port>` | bind text verbatim; 3+ seats host hub | `host_bind_all_interfaces` + hub rule |
| launch | records `join.force_input_relay` | relay rewrite to WS peer, `sfu_required` | all three (policy-gated) |
| WS RTT pong | broadcast (timed by every guest against its own clock) | ignored | sent to the asker only |

Defects fixed in the merge (each has a pinning case in the tests):

- Receive buffer was 4 KB in all three copies: a busy hub's `lobby_list` (32
  rooms + 64 players ≈ 20 KB) disconnected the client. Now 128 KB, 64-bit
  lengths, fragments reassembled, pings answered, close handled.
- The pre-handshake send queue truncated frames at 2 KB (a create with a mod
  plan went out cut mid-token). Now frames are held whole.
- `Sec-WebSocket-Accept` was never checked; the status check was `strstr("101")`;
  a server that accepted TCP and never answered the upgrade held the client
  "connected, not ready" forever (now `handshake_timeout_ms`, default 10 s).
- `strstr("\"key\"")` JSON reads answered for keys at any depth and inside
  strings, and row chunks clipped long rows. Replaced with a span-bounded
  top-level reader; `\uXXXX` now decodes to UTF-8.
- snesrecomp `auto_ready` looped forever for a spectator: the server keeps a
  spectator's ready false and answers each `set_ready` with a `lobby_update`.
- The mod-transfer header interpolated package id/version unescaped.
- `hello_ok` (the server's accepted name), `name_rejected` /
  `lobby_name_rejected` / `password_invalid` and `session_invalid` were
  ignored; they are now surfaced (`accepted_name`, `name_refused`,
  `session_invalid`), and `session_invalid` no longer poses as a join failure.
- The block set is re-sent on every `welcome` (the server forgets it per
  connection).

Not implemented, deliberately:

- `wss://` (no TLS in any engine copy) and IPv6 lobby hosts (the relay/LAN
  logic reasons in IPv4, as both copies did).
- The server-mediated pre-join transfer (`mod_xfer_start` / `mod_xfer_pull` /
  `mod_signal`). It serves titles whose plan is the seat-gating `mods` key; this
  client publishes `mod_plan` and transfers over the seated relay. Inbound
  `mod_xfer_*` ops are logged and ignored.
- psxrecomp's `PsxLobbyOnlinePlayer.player_id`: the server never sends it.

## Tests

| Test | Covers |
|------|--------|
| `lobby_json_test` | the JSON reader: top-level only, escapes/UTF-8, truncation, malformed input |
| `lobby_mod_plan_test` | snesrecomp's `lobby_mod_plan_test` ported: plan/offer wire, launch gate, relay cap, spectator seats, ICE sender filter, launch transport, chat ring |
| `lobby_client_test` | canned server frames for every op: welcome/hello, list, create/join/binds, launch policies, errors, chat/report, seats, caps, signals and the ICE gate, both RTT modes, TURN, automatch, desync; large/fragmented frames |
| `lobby_ws_loopback_test` | the real transport against a loopback WebSocket server: async and blocking connect, the accept check, masked frames, ping/pong, close, handshake timeout against a silent server, refused connect, abandoning an in-flight connect |

Parsers are reached through `src/lobby/rnet_lobby_internal.h`
(`rnet_lobby__ingest`, `rnet_lobby__rx_feed`, `rnet_lobby__test_attach`), which
tests include with `src/` on their include path; it is not public API.
