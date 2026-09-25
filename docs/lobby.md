# Lobby protocol (client-facing)

`recomp-net` is a delay-sync library only — it does **not** include a lobby
server. The open-source control plane lives in the sibling repo
[`recomp-net-server`](https://github.com/RetroPortingToolKit/recomp-net-server).

Authoritative wire documentation for the MotK / psxrecomp WebSocket JSON
protocol:

- https://github.com/RetroPortingToolKit/recomp-net-server/blob/main/docs/WS_LOBBY.md
- Architecture: https://github.com/RetroPortingToolKit/recomp-net-server/blob/main/docs/HOW_IT_WORKS.md

Default client URL: `ws://netplay.retcomm.net:8765`  
Override with env `RNET_LOBBY_URL`. An engine may register its older spelling
as an alias (`RNetLobbyConfig.legacy_env_prefix`): snesrecomp registers
`SNES_NET_`, so `SNES_NET_LOBBY_URL` keeps working; psxrecomp's own client
still reads `PSX_NET_LOBBY_URL`.
Local bring-up: `ws://127.0.0.1:8765` (run `recomp-net-server` yourself).

## The client: `recomp_net/lobby_client.h`

The protocol client lives here (`src/lobby/rnet_lobby_client.c`), lifted from
snesrecomp's `snes_lobby_client.c` so every engine links one copy instead of
forking it (psxrecomp's `psx_lobby_client.c` is the remaining fork; see
"Engines still carrying a fork" below). It speaks every op of `WS_LOBBY.md`
and `AUTOMATCH.md` the SNES client spoke, plus the peer-to-peer mod transfer
that rides the seated `signal` relay.

What stayed engine-specific became configuration:

| Was hard-coded | Now |
| --- | --- |
| `SNES_GAME_VERSION` fallback | `rnet_lobby_set_game_identity` / `RNetLobbyConfig.game_version`; `"dev"` when unset |
| `SNES_NET_LOBBY_URL`, `SNES_NET_GAME_VERSION` | `RNET_LOBBY_URL`, `RNET_LOBBY_GAME_VERSION`, plus `legacy_env_prefix` aliases |
| player seat ceiling 4 | `RNetLobbyConfig.max_players` (array size 8) |
| automatch `max_slots:2` | `RNetLobbyConfig.automatch_slots` (default 2) |
| chat-report `platform:"snes"` | `RNetLobbyConfig.platform` |
| widescreen keys in `match_caps` | `RNetLobbyMatchCaps.ext` + `RNetLobbyCapsCodec` (engine writes/parses its own keys) |

`RNetLobbyMatchCaps` carries only the keys every engine shares:
`input_delay`, `input_prediction` (sent only when the host set it),
`rollback`, `force_turn`, `force_input_relay`, the mod plan (`mod_plan`),
the effective mod set (`mod_set`) and the cosmetic grant
(`mod_cosmetic_allow`). Spectators are a `create` option
(`rnet_lobby_set_allow_spectators`), not a caps key.

The launcher-facing half -- `RecompLauncherCNetplayCallbacks` over this client
and the LAN modules -- is recomp-ui's optional `recomp_launcher_netplay`
module (`src/netplay/recomp_netplay_host.h`), not part of this library.

### Engines still carrying a fork

psxrecomp `runtime/src/psx_lobby_client.c` (common ancestor psx 31015cea). It
has what this client lacks -- waiting-room RTT over the ICE path
(`ice_rtt`), `path_report`, `set_host_endpoint` / STUN, an asynchronous
connect -- and lacks what this client has (mod plan/transfer,
`desync_report`). Folding it in means porting those four features here first,
then deleting the fork.

Host `match_caps` (opaque JSON on create/start) are echoed to guests so
sim-affecting settings stay aligned; see the server `WS_LOBBY.md`.

Lobbies also carry `game_name` + `game_version` (release pin). Create/join
must match; `list` can filter by either. Empty version normalizes to `dev`.

## Summary (for host integrators)

- One WebSocket per player; text frames are JSON objects with an `"op"` field.
- Server assigns `player_id` on connect (`welcome`).
- Hosts `create` lobbies; guests `list` / `join` (optional password).
- Server returns `session_id`, slot map, and rewritten `host_endpoint` /
  `guest_endpoint` for LAN (or ICE `signal` relay).
- After handoff, peers use **this** library (`rnet_session_*`) for INPUT
  exchange. The lobby server is not on the input path.

Open-source clients (this library's `rnet_lobby_client`, psxrecomp's
`psx_lobby_client`) implement the protocol; they do not embed the server.

## Local LAN room registry

`recomp_net/lan_lobby.h` provides the small, server-independent room registry
used by launchers running multiple instances on one machine. A LAN/Direct IP
host publishes an `RNetLanLobby` beside its configuration; launchers may merge
that row with the remote WebSocket list for discovery. Online hosts publish
only to the lobby server and must not dual-publish here (join/member/start
handling differs per channel). Joining claims the guest slot but does not
start the game; both launchers continue showing their shared room until the
host calls `rnet_lan_lobby_set_started()`.

Host kick clears the guest seat via `rnet_lan_lobby_kick()` while keeping the
room published; the joiner drops out when the registry no longer lists them.

Lobby create port policy is owned by **recomp-ui** (`launcher_udp_port.*`)
before `create()`: LAN/Direct IP requires the exact UDP port; online scans
preferred..preferred+31 and rewrites the endpoint. `rnet_udp_port_available` /
`rnet_udp_find_free_port` remain available for hosts that need the same probe
outside the launcher.

The registry does not carry input or replace `rnet_session_start_lan()`.
