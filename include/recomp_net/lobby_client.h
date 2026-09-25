/* recomp_net/lobby_client.h -- the WebSocket lobby client, for every console.
 *
 * ONE copy of the client for the recomp-net-server lobby protocol
 * (recomp-net-server docs/WS_LOBBY.md, AUTOMATCH.md, MODERATION.md). It used
 * to live three times -- snesrecomp's snes_lobby_client, psxrecomp's
 * psx_lobby_client and segagenesisrecomp's genesis_lobby_client -- and the
 * copies had drifted apart in both directions: fixes landed in one and not the
 * others, and each had features the others lacked. This is their superset,
 * with every console-specific constant turned into configuration.
 *
 * What the ENGINE supplies (see docs/lobby_client.md for the porting guide):
 *   - identity: title, release pin, platform tag, content fingerprint, seat
 *     counts -- RNetLobbyConfig at rnet_lobby_open();
 *   - the title's own match settings, as opaque JSON members
 *     (RNetLobbyMatchCaps.game_json), which the library carries verbatim;
 *   - optional hooks: mod offer / export / install, log sink, session token;
 *   - a pump: rnet_lobby_pump() once per frame from the thread that owns the
 *     handle.
 *
 * Threading: a handle is NOT thread-safe. Every call on one handle must come
 * from one thread (the launcher/UI thread in practice). The only thread the
 * library starts is the connect worker (DNS + TCP + WebSocket upgrade, so a
 * slow resolver never freezes the UI); it owns a private copy of what it needs
 * and never touches the handle.
 *
 * Every string out-parameter is NUL-terminated and truncated to its buffer.
 * Every "get(index, out)" returns 1 when it filled `out`, 0 otherwise.
 * Every send-style call returns 0 when the frame was handed to the socket (or
 * queued for the handshake), <0 when refused locally. Handed over is not
 * accepted: the server's answer arrives asynchronously. A write the socket
 * cannot take whole (send buffer full, rnet_ws_write_text failing) drops the
 * connection at the next pump rather than leave a partial frame in the
 * stream; reconnect and the handle's configuration is intact.
 */
#ifndef RECOMP_NET_LOBBY_CLIENT_H
#define RECOMP_NET_LOBBY_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── sizes ───────────────────────────────────────────────────────────────── */

#define RNET_LOBBY_ID_LEN        40
#define RNET_LOBBY_NAME_LEN      64
#define RNET_LOBBY_VERSION_LEN   48
#define RNET_LOBBY_ENDPOINT_LEN  64
#define RNET_LOBBY_URL_LEN      256
#define RNET_LOBBY_ERROR_LEN     64
#define RNET_LOBBY_MAX_LIST      32
#define RNET_LOBBY_MAX_ONLINE    64
#define RNET_LOBBY_MAX_LAN_EPS    4

/* Compile-time ceilings. A title picks its own counts in RNetLobbyConfig
 * (max_players / max_spectators); these only size the tables. The server
 * caps a room at 8 players and 4 spectators today (ws_lobby.rs MAX_SLOTS /
 * MAX_SPECTATORS); the gallery ceiling leaves room for it to grow. */
#define RNET_LOBBY_MAX_PLAYERS     8
#define RNET_LOBBY_MAX_SPECTATORS  8
#define RNET_LOBBY_MAX_MEMBERS (RNET_LOBBY_MAX_PLAYERS + RNET_LOBBY_MAX_SPECTATORS)

/* Seat indices are one namespace, matching the server: below the base is a
 * player seat, at or above it is `index - base` in the gallery. The server
 * republishes its own base in every lobby_update; this is the fallback. */
#define RNET_LOBBY_DEFAULT_SPECTATOR_SLOT_BASE 64

#define RNET_LOBBY_DEFAULT_URL "ws://netplay.retcomm.net:8765"

#define RNET_LOBBY_CHAT_TEXT_LEN 256
#define RNET_LOBBY_CHAT_RING      64
#define RNET_LOBBY_MID_LEN        40

#define RNET_LOBBY_MAX_MODS      16
#define RNET_LOBBY_MOD_ID_LEN    96
#define RNET_LOBBY_MOD_VER_LEN   32
#define RNET_LOBBY_MOD_NAME_LEN  64
#define RNET_LOBBY_MOD_FEATS_LEN 192
#define RNET_LOBBY_MOD_SET_LEN   512

/* The title's own match_caps members, and the whole object as received. The
 * server discards a match_caps object over 4096 bytes serialized. */
#define RNET_LOBBY_CAPS_GAME_JSON_LEN 2048
#define RNET_LOBBY_CAPS_JSON_LEN      4104

/* Extra members the title attaches to every set_ready (psxrecomp's
 * bios_offer / memcard_offer). */
#define RNET_LOBBY_READY_EXTRA_LEN 1024

#define RNET_LOBBY_MAX_RULESETS       8
#define RNET_LOBBY_RULESET_ID_LEN    48
#define RNET_LOBBY_RULESET_LABEL_LEN 64
#define RNET_LOBBY_CAPS_SUMMARY_LEN 128

/* A relayed mod transfer spends the TURN operator's bandwidth; a direct pair
 * costs nobody anything and is uncapped. Bytes: 5 MiB. */
#define RNET_LOBBY_MOD_RELAY_MAX_BYTES (5u * 1024u * 1024u)

typedef struct RNetLobby RNetLobby;

/* ── configuration ───────────────────────────────────────────────────────── */

typedef enum RNetLobbyLogLevel {
    RNET_LOBBY_LOG_DEBUG = 0,
    RNET_LOBBY_LOG_INFO  = 1,
    RNET_LOBBY_LOG_WARN  = 2,
    RNET_LOBBY_LOG_ERROR = 3
} RNetLobbyLogLevel;

/* One finished line, no trailing newline. */
typedef void (*RNetLobbyLogFn)(void *user, RNetLobbyLogLevel level,
                               const char *line);

/* The signed-in account's session token for `hello`, or NULL/"" for a guest.
 * Default: rnet_account_session() (recomp_net/auth.h). */
typedef const char *(*RNetLobbySessionFn)(void *user);

/* How the waiting room measures latency to the other seats. */
typedef enum RNetLobbyRttMode {
    RNET_LOBBY_RTT_OFF = 0,
    /* snesrecomp: guests ping the host over the lobby's own `signal` relay
     * (types 100/101) once a second and report the result (102). Measures the
     * WebSocket path, which every seat has. */
    RNET_LOBBY_RTT_WS_SIGNAL = 1,
    /* psxrecomp: a UDP probe to the peer's game endpoint (rtt_probe.h), and --
     * when built with RNET_ENABLE_ICE and the server mints TURN credentials --
     * an ICE data-channel probe (ice_rtt.h) that works through CGNAT. The
     * selected pair is reported to the server as `path_report` telemetry.
     * Stores the pessimistic sample. While this probe owns the ICE signal
     * queue, inbound gameplay ICE is gated (rnet_lobby_set_ice_signal_accept)
     * until `launch` opens it for the match. */
    RNET_LOBBY_RTT_PEER_PATH = 2
} RNetLobbyRttMode;

typedef struct RNetLobbyConfig {
    /* ---- identity ---------------------------------------------------- */
    const char *game_name;     /* the title as the lobby announces it (scopes
                                  the list, server chat, players online) */
    const char *game_version;  /* release pin; NULL/"" -> "dev" */
    const char *build_id;      /* optional: the exact build (commit, dirty
                                  hash), logged beside the pin so a desync
                                  report can name both */
    const char *platform;      /* "snes", "psx", "genesis", ... -- chat report
                                  metadata only */
    const char *content_fp;    /* optional 64-hex SHA-256 of the guest image
                                  (ROM / disc TOC); see rnet_lobby_set_fp */
    const char *log_prefix;    /* prefix for default log lines; NULL ->
                                  "rnet_lobby" */

    /* ---- seats ------------------------------------------------------- */
    int max_players;           /* player seats this title can seat, 2..
                                  RNET_LOBBY_MAX_PLAYERS (default 2) */
    int max_spectators;        /* gallery seats a host may open, 0..
                                  RNET_LOBBY_MAX_SPECTATORS (default 4) */
    int spectator_slot_base;   /* fallback seat base (default 64) */
    int default_max_slots;     /* create's seat count until
                                  rnet_lobby_set_max_slots (default 2) */
    int host_port;             /* default host bind port (7777) */
    int guest_port;            /* preferred guest bind port (7778) */

    /* ---- server ------------------------------------------------------ */
    const char *default_url;   /* NULL -> RNET_LOBBY_DEFAULT_URL */
    const char *url_env_var;   /* env var overriding default_url, e.g.
                                  "SNES_NET_LOBBY_URL"; NULL = none */
    const char *version_env_var; /* env var forcing the presented pin for one
                                  run (testing two dev builds against each
                                  other), e.g. "SNES_NET_GAME_VERSION";
                                  NULL = none */
    int blocking_connect;      /* 0 (default): connect() starts a worker and
                                  returns; poll rnet_lobby_connecting().
                                  1: connect() blocks until TCP is up. */
    int connect_timeout_ms;    /* per-address TCP connect timeout (3000) */
    int handshake_timeout_ms;  /* TCP up but no WebSocket upgrade answer:
                                  give up after this (10000) */

    /* ---- policy ------------------------------------------------------ */
    int auto_ready;            /* 1: re-arm Ready after created / joined /
                                  any update that cleared it (a title with
                                  no Ready UI; older servers gate start on
                                  all_ready). Default 0. */
    int require_server_relay;  /* 1: refuse a launch whose transport is
                                  ice_p2p (psxrecomp section 108: online
                                  lobbies are SFU-only). Default 0. */
    int host_bind_all_interfaces; /* 1 (default): the host listens on
                                  0.0.0.0:<port of host_bind> -- the
                                  advertised address may be a NAT address
                                  this machine cannot bind. 0: bind the
                                  host_bind text verbatim. */
    int fingerprint_in_rooms;  /* 1 (default): send content_fp on create and
                                  join, so the server refuses a mismatched
                                  image (disc_mismatch). NOTE the server
                                  treats one empty side as a mismatch, so a
                                  build that sends it cannot share rooms
                                  with one that does not. 0: only automatch
                                  carries it (snesrecomp's historic wire). */
    RNetLobbyRttMode waiting_room_rtt; /* default RNET_LOBBY_RTT_PEER_PATH */
    int list_latency;          /* 1 (default): after each lobby_list, UDP-
                                  ping every row's LAN beacon / host
                                  endpoint once; fills RNetLobbyRow.
                                  latency_ms */
    int lan_beacon;            /* 1 (default): host announces its LAN
                                  endpoint on the local UDP beacon, guests
                                  listen (lan_beacon.h). Private addresses
                                  never go to the hub. */
    int host_advertise;        /* 1 (default): after create, STUN-discover
                                  the host's public UDP mapping and publish
                                  it with set_host_endpoint (list RTT). */

    /* ---- match_caps defaults (what an absent key reads as) ------------ */
    int caps_input_delay_default; /* 6 */
    int caps_input_delay_min;     /* 0 */
    int caps_input_delay_max;     /* 20 */
    int caps_input_prediction_default; /* 10 */
    int caps_input_prediction_min;     /* 2 */
    int caps_input_prediction_max;     /* 16 */
    int caps_rollback_default;    /* 1 */

    /* ---- hooks ------------------------------------------------------- */
    RNetLobbyLogFn log;        /* NULL -> stderr "<prefix>: <line>" */
    void *log_user;
    int log_min_level;         /* lines below this are dropped (INFO) */
    RNetLobbySessionFn session; /* NULL -> rnet_account_session() */
    void *session_user;
} RNetLobbyConfig;

/* Fills every field with its documented default. Call before setting yours. */
void rnet_lobby_config_init(RNetLobbyConfig *cfg);

/* Creates a client. Does not connect. The config's strings are copied.
 * Returns 0 and sets *out, or <0 (bad config / out of memory). */
int  rnet_lobby_open(RNetLobby **out, const RNetLobbyConfig *cfg);
/* Disconnects, abandons an in-flight connect without blocking, frees. Sets
 * *lobby to NULL. */
void rnet_lobby_close(RNetLobby **lobby);

/* ── connection ──────────────────────────────────────────────────────────── */

/* The URL connect() uses when given NULL/"": the env override, else
 * cfg.default_url, else RNET_LOBBY_DEFAULT_URL. */
const char *rnet_lobby_default_url(RNetLobby *lobby);

/* ws:// only (no TLS). 0 = connected (blocking mode) or started (async);
 * also 0 when already connected/connecting. <0: bad URL (-1), resolve (-2),
 * connect (-3), upgrade send (-4), worker start (-5). */
int  rnet_lobby_connect(RNetLobby *lobby, const char *ws_url);
void rnet_lobby_disconnect(RNetLobby *lobby);
/* TCP up (the WebSocket handshake may still be in flight; frames sent now
 * are queued until it completes). */
int  rnet_lobby_connected(RNetLobby *lobby);
/* 1 while the async worker resolves / connects. */
int  rnet_lobby_connecting(RNetLobby *lobby);
/* 1 once the server's `welcome` arrived (player_id known). */
int  rnet_lobby_ready(RNetLobby *lobby);
/* The URL of the current connection, "" when not connected. */
const char *rnet_lobby_url(RNetLobby *lobby);

/* Non-blocking. Call every frame: it drives the socket, the connect worker,
 * the mod transfer, the automatch probe and every latency probe. */
void rnet_lobby_pump(RNetLobby *lobby);

/* ── identity ────────────────────────────────────────────────────────────── */

/* Re-sent to the server immediately (as `hello`) when connected and changed:
 * `hello` is the whole rename protocol. */
void rnet_lobby_set_display_name(RNetLobby *lobby, const char *name);
const char *rnet_lobby_display_name(RNetLobby *lobby);
/* The name the server accepted in its last `hello_ok` -- differs from the
 * requested one when the room already had it ("Alex (2)"), or when a signed-in
 * account's handle overrides it. "" before the first answer. */
const char *rnet_lobby_accepted_name(RNetLobby *lobby);
/* 1 when the server refused the last name / lobby name / password
 * ("name_rejected", "lobby_name_rejected", "password_invalid"); writes the
 * code. Cleared by set_display_name and clear_last_error. */
int  rnet_lobby_name_refused(RNetLobby *lobby, char *code, size_t code_cap);
/* 1 when the server rejected the session token in `hello` (the connection
 * continues as a guest). */
int  rnet_lobby_session_invalid(RNetLobby *lobby);
const char *rnet_lobby_player_id(RNetLobby *lobby);

/* Title + release pin. version NULL/"" -> "dev". The version env override (if
 * configured and set) wins, and is announced in the log beside the real pin
 * every time, because a build lying about which build it is must be visible
 * in the log a desync report is written from. Kept across reconnects. */
void rnet_lobby_set_game_identity(RNetLobby *lobby, const char *game_name,
                                  const char *game_version);
const char *rnet_lobby_game_name(RNetLobby *lobby);
/* The pin actually presented on the wire. */
const char *rnet_lobby_game_version(RNetLobby *lobby);
/* Should a browser hide rooms of other versions? True only for a plain
 * released pin: "dev", anything starting "dev", and anything carrying a '+'
 * qualifier ("0.1.5+abc-dirty.1234") list unfiltered -- the join still
 * enforces the pin, but a hidden room cannot explain itself. Always false
 * while the version env override is active. */
int  rnet_lobby_version_filter_strict(RNetLobby *lobby);
/* The rule above applied to any pin (for a LAN browser to share it). */
int  rnet_lobby_version_is_release(const char *game_version);

/* Content fingerprint: 64 hex chars (case folded). Anything else clears it.
 * Required to automatch (a wildcard in a queue pairs different dumps); sent on
 * create/join when cfg.fingerprint_in_rooms. Kept across reconnects. */
void rnet_lobby_set_fp(RNetLobby *lobby, const char *hex64);
const char *rnet_lobby_fp(RNetLobby *lobby);

/* ── lobby list / players online ─────────────────────────────────────────── */

typedef struct RNetLobbyRow {
    char lobby_id[RNET_LOBBY_ID_LEN];
    char name[RNET_LOBBY_NAME_LEN];
    char game_name[RNET_LOBBY_NAME_LEN];
    char game_version[RNET_LOBBY_VERSION_LEN];
    int  player_count;
    int  max_slots;
    int  has_password;
    /* Host UDP endpoint from the list (public / STUN), for latency. */
    char host_endpoint[RNET_LOBBY_ENDPOINT_LEN];
    /* Deprecated hub lan_endpoints (older hosts). Current hosts announce LAN
     * endpoints on the local beacon instead. */
    char lan_endpoints[RNET_LOBBY_MAX_LAN_EPS][RNET_LOBBY_ENDPOINT_LEN];
    int  lan_count;
    /* Round trip to the first reachable candidate; -1 unknown/timed out. */
    int  latency_ms;
    char host_country[4];       /* alpha-2 GeoIP; "" unknown */
    int  allow_spectators;      /* gallery: 0 none */
    int  max_spectators;
    int  spectator_count;
    int  lobby_kind;            /* match_caps.lobby_kind echo (0 standard) */
} RNetLobbyRow;

typedef struct RNetLobbyOnlinePlayer {
    char display_name[RNET_LOBBY_NAME_LEN];
    char country[4];
    char lobby_id[RNET_LOBBY_ID_LEN];   /* "" when browsing */
    char lobby_name[RNET_LOBBY_NAME_LEN];
    int  hosting;
    /* First 8 chars of the connection id: compare with player_id to find
     * "which row is me" without publishing whole ids. */
    char tag[12];
    /* Opaque account key; "" for a guest. What a block list keys on: it
     * survives a reconnect and a rename. Never rendered. */
    char account[RNET_LOBBY_ID_LEN];
    /* Title this player browses for. Rows of other titles are dropped at
     * parse time when this client has a title. */
    char game_name[RNET_LOBBY_NAME_LEN];
} RNetLobbyOnlinePlayer;

/* Also re-probes every row's latency on the answer (cfg.list_latency). */
void rnet_lobby_request_list(RNetLobby *lobby);
int  rnet_lobby_list_count(RNetLobby *lobby);
int  rnet_lobby_list_get(RNetLobby *lobby, int index, RNetLobbyRow *out);
int  rnet_lobby_online_count(RNetLobby *lobby);
int  rnet_lobby_online_get(RNetLobby *lobby, int index,
                           RNetLobbyOnlinePlayer *out);

/* ── match caps ──────────────────────────────────────────────────────────── */

/* One package on the lobby wire: a row of the host's required plan, or of a
 * peer's offer of what it already has. Independent of any mod runtime. */
typedef struct RNetLobbyModPkg {
    char id[RNET_LOBBY_MOD_ID_LEN];
    char ver[RNET_LOBBY_MOD_VER_LEN];
    char name[RNET_LOBBY_MOD_NAME_LEN];   /* display name; "" on an offer */
    char feats[RNET_LOBBY_MOD_FEATS_LEN]; /* comma-separated enabled features */
} RNetLobbyModPkg;

/*
 * Host-authoritative settings negotiated over the lobby. The library owns the
 * keys it acts on itself; everything else is the title's, carried verbatim.
 *
 * Library-owned keys (emitted by the library, parsed into the fields below):
 *   v, input_delay, input_prediction, rollback, force_turn,
 *   force_input_relay, mod_plan, mod_set, mod_cosmetic_allow
 *
 * `mod_plan`, deliberately NOT `mods`: `mods` is the key the server enforces
 * by refusing to SEAT a joiner who lacks a package. A player without a mod
 * belongs in the room -- to see what is needed and download it from the host
 * -- so the gate lives at launch (rnet_lobby_match_blocked_by_mods), not at
 * the door. The plan is an ARRAY of objects: an older encoding put it in a
 * ';'-separated string, which every as_array() reader saw as empty, failing
 * open.
 */
typedef struct RNetLobbyMatchCaps {
    int  valid;              /* 1 when a blob was received / set */
    int  input_delay;        /* delay frames, clamped to the config range */
    int  input_prediction;   /* rollback runway; 0 = title does not use it
                                (not emitted) */
    int  rollback;           /* 0/1 */
    int  force_turn;         /* 0/1: ICE relay-only (a delay-floor hint on
                                SFU servers) */
    int  force_input_relay;  /* 0/1: the HOST's toggle. The transport a
                                launch actually got is RNetLobbyJoinInfo.
                                force_input_relay -- never read it here. */
    int  mod_count;
    RNetLobbyModPkg mods[RNET_LOBBY_MAX_MODS];  /* the plan */
    /* The host's EFFECTIVE set: one entry per enabled feature with resolved
     * option values, ';' where the canonical text has newlines. */
    char mod_set[RNET_LOBBY_MOD_SET_LEN];
    /* Which packages this match's AUTHORITY exempts as presentation-only:
     * ';'-separated id@version or id@version#sha256. Empty = nothing is. */
    char mod_cosmetic_allow[RNET_LOBBY_MOD_SET_LEN];
    /* The title's own members, as a JSON member list WITHOUT braces:
     *   "\"widescreen\":true,\"ws_extra\":0"
     * OUT: appended verbatim after the library's keys (must be valid JSON
     * members; the library does not re-validate them). IN: every member of
     * the received object the library does not own, verbatim. Read it with
     * rnet_lobby_json_get_*. */
    char game_json[RNET_LOBBY_CAPS_GAME_JSON_LEN];
    /* The whole object, as received or as last serialised. */
    char json[RNET_LOBBY_CAPS_JSON_LEN];
} RNetLobbyMatchCaps;

/* valid=0, library defaults from the handle's config (NULL -> built-ins). */
void rnet_lobby_match_caps_init(RNetLobby *lobby, RNetLobbyMatchCaps *caps);
/* Serialises `caps` as a JSON object "{...}". Returns bytes written, 0 when it
 * would not fit `cap` or would exceed the server's 4096-byte limit -- nothing
 * is published rather than a short plan. */
size_t rnet_lobby_match_caps_encode(RNetLobby *lobby,
                                    const RNetLobbyMatchCaps *caps,
                                    char *out, size_t cap);
/* Parses an object. Returns 1 when `json` was an object (caps->valid = 1). */
int  rnet_lobby_match_caps_decode(RNetLobby *lobby, const char *json,
                                  RNetLobbyMatchCaps *caps);

/* Latest caps (valid=0 until create/join/launch delivers one). */
const RNetLobbyMatchCaps *rnet_lobby_match_caps(RNetLobby *lobby);
/* Host: republish while in a lobby (the server clears ready). */
int  rnet_lobby_set_match_caps(RNetLobby *lobby, const RNetLobbyMatchCaps *caps);

/* ── rooms ───────────────────────────────────────────────────────────────── */

typedef struct RNetLobbyJoinInfo {
    int      ok;
    char     lobby_id[RNET_LOBBY_ID_LEN];
    uint32_t session_id;
    int      local_slot;
    char     host_endpoint[RNET_LOBBY_ENDPOINT_LEN];
    char     guest_endpoint[RNET_LOBBY_ENDPOINT_LEN];
    char     bind_hostport[RNET_LOBBY_ENDPOINT_LEN];  /* what to bind */
    char     peer_hostport[RNET_LOBBY_ENDPOINT_LEN];  /* what to dial ("" =
                                                         host hub / accept
                                                         first peer) */
    int      player_count;
    int      max_slots;
    /* 1 when this client holds a gallery seat: it runs the match and shows
     * it, and contributes no input. */
    int      local_is_spectator;
    int      allow_spectators;     /* server-echoed; 0 on old servers */
    int      max_spectators;
    int      spectator_count;
    int      spectator_slot_base;
    /* Where the gallery starts in the RELAY's slot space -- the number that
     * makes a spectator unable to send (rnet_lobby_local_wire_slot). */
    int      spectator_relay_base;
    /* Launch: the host watches from the gallery but runs the match from
     * session slot 0 (pad muted); player seats sit at lobby seat + 1. */
    int      host_spectates;
    /* This match's transport, as the LAUNCH stated it: 1 = the server's UDP
     * input relay (everyone dials it), 0 = peer-to-peer. Restated on every
     * launch; never re-derived from match_caps, whose copy is the host's
     * toggle and is rewritten by every republish. */
    int      force_input_relay;
    char     transport[16];        /* launch "transport" ("sfu", "ice_p2p") */
    char     last_error[RNET_LOBBY_ERROR_LEN]; /* need_password, bad_password,
                                      version_mismatch, disc_mismatch,
                                      need_mods, peer_needs_mods,
                                      missing_endpoints, sfu_required, ... */
} RNetLobbyJoinInfo;

/* Create a room. name NULL -> "Lobby"; game_name/game_version NULL -> the
 * identity (a non-empty game_name also becomes the identity); password
 * NULL/"" = open; host_bind NULL -> "0.0.0.0:<host_port>"; caps may be NULL;
 * max_slots <= 0 -> the rnet_lobby_set_max_slots default. Clamped to
 * 2..cfg.max_players. Poll in_lobby()/join_info(). */
int  rnet_lobby_create(RNetLobby *lobby, const char *name,
                       const char *game_name, const char *game_version,
                       const char *password, const char *host_bind,
                       const RNetLobbyMatchCaps *caps, int max_slots);
/* Join. guest_bind NULL/""/":0" -> a free UDP port from cfg.guest_port
 * upward: never advertise port 0 (a server-hosted launch would hand the host
 * peer_ip:0). Carries the mod offer when a supplier is installed. */
int  rnet_lobby_join(RNetLobby *lobby, const char *lobby_id,
                     const char *password, const char *guest_bind);
int  rnet_lobby_leave(RNetLobby *lobby);
/* The seat count create() uses when given max_slots <= 0. */
void rnet_lobby_set_max_slots(RNetLobby *lobby, int max_slots);

int  rnet_lobby_in_lobby(RNetLobby *lobby);
int  rnet_lobby_is_host(RNetLobby *lobby);
/* Stable across seat swaps; "" if unknown. */
const char *rnet_lobby_host_player_id(RNetLobby *lobby);
const RNetLobbyJoinInfo *rnet_lobby_join_info(RNetLobby *lobby);
void rnet_lobby_clear_last_error(RNetLobby *lobby);

/* ── members ─────────────────────────────────────────────────────────────── */

typedef struct RNetLobbyMember {
    /* Seat index in the shared namespace: pass it back to kick/move as-is. */
    int  slot;
    char player_id[RNET_LOBBY_ID_LEN];
    char display_name[RNET_LOBBY_NAME_LEN];
    int  ready;
    /* 1 when this row is in the gallery. Read this, not `slot` vs a base. */
    int  is_spectator;
    char country[4];
    char account[RNET_LOBBY_ID_LEN];  /* see RNetLobbyOnlinePlayer.account */
    int  mod_offer_count;             /* packages this seat announced; -1
                                         when its row did not parse whole */
} RNetLobbyMember;

int  rnet_lobby_member_count(RNetLobby *lobby);
int  rnet_lobby_member_get(RNetLobby *lobby, int index, RNetLobbyMember *out);
/* The member's whole row as the server sent it, including any title-owned
 * members (bios_offer, memcard_offer, ...). Valid until the next pump. NULL
 * when out of range. */
const char *rnet_lobby_member_json(RNetLobby *lobby, int index);
/* Waiting-room RTT in ms to `slot`, -1 unknown / own seat / host's own row
 * (RNET_LOBBY_RTT_WS_SIGNAL). */
int  rnet_lobby_member_latency_ms(RNetLobby *lobby, int slot);
/* member.player_id == host_player_id. Prefer this to `slot == 0`. */
int  rnet_lobby_member_is_host(RNetLobby *lobby, const RNetLobbyMember *member);

/* Ready flag of our row in the last update. */
int  rnet_lobby_local_ready(RNetLobby *lobby);
/* Every seated player ready and player_count >= 2. */
int  rnet_lobby_all_ready(RNetLobby *lobby);
/* Every set_ready carries the mod offer and the ready extras. */
int  rnet_lobby_set_ready(RNetLobby *lobby, int ready);
/* Title-owned members attached to every set_ready, e.g.
 *   "\"bios_offer\":{\"v\":1,\"prefer\":\"openbios\"}"
 * NULL/"" clears. The server echoes only keys it knows (bios_offer,
 * memcard_offer, mod_offer) into each seat's row. Returns -1 if too long. */
int  rnet_lobby_set_ready_extra_json(RNetLobby *lobby, const char *members);

/* Host: remove the occupant of `slot` (not the host). */
int  rnet_lobby_kick(RNetLobby *lobby, int slot);
/* Host: swap / move seats, either side may be in the gallery. */
int  rnet_lobby_move(RNetLobby *lobby, int from_slot, int to_slot);

/* Seat self-service: move yourself to a FREE seat, or ask the occupant of a
 * taken one to trade. incoming: 1 while somebody is asking this player (who,
 * their seat); respond answers it. outgoing: 0 idle, 1 waiting, 2 accepted,
 * -1 declined; clear returns a finished result to 0. */
int  rnet_lobby_seat_move_self(RNetLobby *lobby, int to_slot);
int  rnet_lobby_seat_swap_request(RNetLobby *lobby, int target_slot);
int  rnet_lobby_seat_swap_incoming(RNetLobby *lobby, char *who, size_t who_cap,
                                   int *from_slot);
int  rnet_lobby_seat_swap_respond(RNetLobby *lobby, int accept);
int  rnet_lobby_seat_swap_outgoing(RNetLobby *lobby);
void rnet_lobby_seat_swap_clear(RNetLobby *lobby);

/* ── spectators ──────────────────────────────────────────────────────────── */

/* Host: open a gallery on the NEXT create. Sticky across reconnects. */
void rnet_lobby_set_allow_spectators(RNetLobby *lobby, int allow);
/* What the toggle is set to (what the host asked for). */
int  rnet_lobby_allow_spectators_pref(RNetLobby *lobby);
/* What the current room actually has (server-echoed). */
int  rnet_lobby_allow_spectators(RNetLobby *lobby);
int  rnet_lobby_max_spectators(RNetLobby *lobby);
int  rnet_lobby_spectator_count(RNetLobby *lobby);
int  rnet_lobby_local_is_spectator(RNetLobby *lobby);
int  rnet_lobby_spectator_slot_base(RNetLobby *lobby);
/* 1 when `slot` addresses a seat in either table. */
int  rnet_lobby_seat_valid(RNetLobby *lobby, int slot);
/* Seat index of gallery position `index`; -1 out of range. */
int  rnet_lobby_spectator_slot(RNetLobby *lobby, int index);
/* This client's slot in the RELAY's namespace (RNetConfig.wire_slot); -1 when
 * not a spectator or no relay base was published -- and a spectator without
 * one must not launch. */
int  rnet_lobby_local_wire_slot(RNetLobby *lobby);

/* ── chat ────────────────────────────────────────────────────────────────── */

/* One chat line. The server echoes every line to everyone INCLUDING the
 * sender, so the ring is the room's order: the client never appends its own
 * send. Lines are masked (chat_filter.h) on arrival whatever relayed them. */
typedef struct RNetLobbyChatMsg {
    char     player_id[RNET_LOBBY_ID_LEN];
    char     account[RNET_LOBBY_ID_LEN];   /* "" guest / system */
    char     from[RNET_LOBBY_NAME_LEN];
    char     country[4];                   /* server chat carries it */
    char     text[RNET_LOBBY_CHAT_TEXT_LEN];
    /* The SERVER's id for this line: what a report names. Empty for system
     * lines and servers too old to assign one -- unreportable. A report
     * carries this and never the text, so a client cannot fabricate one. */
    char     mid[RNET_LOBBY_MID_LEN];
    int      is_local;                     /* decided by player id */
    int      is_system;
    uint32_t seq;                          /* monotonic across clears */
} RNetLobbyChatMsg;

/* Room chat (seated, spectators included). Cleared on create/join/leave. */
int  rnet_lobby_send_chat(RNetLobby *lobby, const char *text);
int  rnet_lobby_chat_count(RNetLobby *lobby);
int  rnet_lobby_chat_get(RNetLobby *lobby, int index, RNetLobbyChatMsg *out);
void rnet_lobby_chat_clear(RNetLobby *lobby);
/* Server chat: per title, outside any room. Its own ring and sequence;
 * cleared on disconnect. */
int  rnet_lobby_send_server_chat(RNetLobby *lobby, const char *text);
int  rnet_lobby_server_chat_count(RNetLobby *lobby);
int  rnet_lobby_server_chat_get(RNetLobby *lobby, int index,
                                RNetLobbyChatMsg *out);
/* Report lines for moderation: `mids` are RNetLobbyChatMsg.mid values (empty
 * entries skipped); reason one of RNET_REPORT_* (chat_report.h). */
int  rnet_lobby_report_chat(RNetLobby *lobby, const char *const *mids,
                            int mid_count, const char *reason,
                            const char *note);
/* The mid of the last `chat_report_ok`, "" if none this connection. */
const char *rnet_lobby_last_report_ack(RNetLobby *lobby);
/* Replace the server-side block set: ';'-separated opaque account ids (the
 * client's own file is the authority). Enforced by the server: no pairing,
 * and the blocked player cannot see or join the blocker's room. The server
 * forgets it per connection; the handle keeps the last set and re-sends it on
 * every `welcome`. Returns -1 when not connected (the set is still kept). */
int  rnet_lobby_set_blocks(RNetLobby *lobby, const char *accounts);

/* ── start / launch ──────────────────────────────────────────────────────── */

/* Host: ask the server to launch. Refused locally (-2, last_error
 * "peer_needs_mods") while a seated peer lacks a plan package. caps non-NULL
 * and valid freezes them into the launch. */
int  rnet_lobby_request_start(RNetLobby *lobby, const RNetLobbyMatchCaps *caps);
/* Set by op:launch; both host and guests boot netplay. */
int  rnet_lobby_launch_pending(RNetLobby *lobby);
void rnet_lobby_clear_launch_pending(RNetLobby *lobby);
/* Copies the join info when a launch is pending and its endpoints are
 * usable. Returns 1 if filled. Does not clear launch_pending. */
int  rnet_lobby_try_fill_launch(RNetLobby *lobby, RNetLobbyJoinInfo *out);
/* After a soft return / rematch: allow waiting-room probes again. A launch
 * suspends them so they cannot steal the match's ICE signals. */
void rnet_lobby_resume_waiting_room_rtt(RNetLobby *lobby);

/* ── ICE signaling relay ─────────────────────────────────────────────────── */

/* text is SDP / a candidate (max 2047). to_player_id NULL/"" broadcasts to
 * the other seated members (the game's own ICE); a point-to-point exchange
 * must name its peer. */
int  rnet_lobby_send_signal_to(RNetLobby *lobby, const char *to_player_id,
                               int type, int flag, const char *text);
int  rnet_lobby_send_signal(RNetLobby *lobby, int type, int flag,
                            const char *text);
/* Inbound gameplay ICE, LOCAL_* as the peer emitted it -- remap to REMOTE_*
 * before rnet_session_push_signal. Signals from spectators (and every signal
 * while this client spectates) are dropped: a third party's SDP reads to the
 * one session agent as a peer ICE restart. Returns 1 when one was copied. */
int  rnet_lobby_poll_signal(RNetLobby *lobby, int *type, int *flag,
                            char *text, size_t text_cap);
void rnet_lobby_clear_signals(RNetLobby *lobby);
/* 0 discards inbound gameplay ICE (lobby / post-match hygiene); a launch
 * re-enables it. Only meaningful under RNET_LOBBY_RTT_PEER_PATH -- other modes
 * start and stay open. */
void rnet_lobby_set_ice_signal_accept(RNetLobby *lobby, int accept);

/* ── TURN credentials ────────────────────────────────────────────────────── */

typedef struct RNetLobbyTurnCredentials {
    int      valid;          /* minted and not expired */
    char     stun_host[128];
    int      stun_port;
    char     turn_host[128];
    int      turn_port;
    int      turns_port;     /* 0 when not published */
    char     realm[64];
    char     username[192];
    char     password[128];
    uint32_t ttl_secs;
} RNetLobbyTurnCredentials;

/* Requested automatically on `welcome`. Refreshes when missing or within 60 s
 * of expiry. */
int  rnet_lobby_request_turn_credentials(RNetLobby *lobby);
/* Never NULL; valid=0 when unavailable / expired. Strings stable until the
 * next successful mint or disconnect -- safe to borrow into RNetIceConfig. */
const RNetLobbyTurnCredentials *rnet_lobby_turn_credentials(RNetLobby *lobby);

/* ── mods ────────────────────────────────────────────────────────────────── */

/* Fills `out` with the packages this peer has; returns the count. Without a
 * supplier the peer announces nothing and every host's gate reads it as
 * having nothing -- the safe direction, but a build with a mod runtime must
 * install one. */
typedef int (*RNetLobbyModOfferFn)(RNetLobbyModPkg *out, int max, void *ctx);
void rnet_lobby_set_mod_offer_supplier(RNetLobby *lobby, RNetLobbyModOfferFn fn,
                                       void *ctx);

/* The last need_mods refusal (valid until the next join). can_transfer is the
 * server's claim about itself. */
int  rnet_lobby_need_mods_count(RNetLobby *lobby);
const RNetLobbyModPkg *rnet_lobby_need_mods_get(RNetLobby *lobby, int index);
int  rnet_lobby_need_mods_can_transfer(RNetLobby *lobby);
/* The launch gate: how many plan packages the OTHER seated peers lack (0 =
 * may start), naming the first offender and package. Matched on id alone --
 * version agreement is the engine's session-start mod-set exchange. */
int  rnet_lobby_match_blocked_by_mods(RNetLobby *lobby, char *who,
                                      size_t who_cap, char *what,
                                      size_t what_cap);
/* How many plan packages THIS peer lacks. */
int  rnet_lobby_local_missing_mods(RNetLobby *lobby);

/* Peer-to-peer package transfer over its own ICE agent (ice_xfer.h). The
 * server relays only SDP/candidates on the seated `signal` channel (types
 * 110/111 and 120+RNetSignalType); no package byte touches it.
 * export: pack `package_id@version` into a malloc()ed archive plus its
 *   lower-case hex SHA-256; return 1 on success (ownership of *out passes to
 *   the library, which frees it with free() once sent, or with free_fn if it
 *   is never queued).
 * install: verify `expect_sha256` BEFORE unpacking; return 1 on success. */
typedef int (*RNetLobbyModExportFn)(const char *package_id, const char *version,
                                    uint8_t **out, uint32_t *out_len,
                                    char *sha256_hex, uint32_t sha_cap,
                                    char *err, uint32_t err_cap, void *ctx);
typedef void (*RNetLobbyModFreeFn)(uint8_t *blob);
typedef int (*RNetLobbyModInstallFn)(const uint8_t *data, uint32_t len,
                                     const char *expect_sha256,
                                     char *installed_id, uint32_t id_cap,
                                     char *installed_ver, uint32_t ver_cap,
                                     char *err, uint32_t err_cap, void *ctx);
void rnet_lobby_set_mod_transfer_hooks(RNetLobby *lobby,
                                       RNetLobbyModExportFn export_fn,
                                       RNetLobbyModFreeFn free_fn,
                                       RNetLobbyModInstallFn install_fn,
                                       void *ctx);
/* May a transfer of `bytes` proceed over an ICE pair of type `path` ("host",
 * "srflx", "prflx", "relay", "unknown", "none")? Only "relay" is capped; an
 * unknown path allows. On refusal writes a player-facing sentence. Pure. */
int  rnet_lobby_mod_relay_size_allows(const char *path, uint64_t bytes,
                                      const char *package_id,
                                      char *reason, size_t reason_cap);
/* Guest: ask the host for one package. 0 asked, -2 busy, -1 refused. */
int  rnet_lobby_mod_request(RNetLobby *lobby, const char *package_id,
                            const char *version);
void rnet_lobby_mod_cancel(RNetLobby *lobby);
/* -1 idle, -2 failed, 0..100 in flight. */
int  rnet_lobby_mod_progress(RNetLobby *lobby);
int  rnet_lobby_mod_failed(RNetLobby *lobby, char *err, size_t err_cap);
const char *rnet_lobby_mod_in_flight(RNetLobby *lobby);

/* ── desync reports ──────────────────────────────────────────────────────── */

/* Evidence, not a verdict: both digests, from both peers independently. A
 * fork can be version skew or an emulation bug; which side moved is only
 * answerable across many matches, server side. Best effort, never queued. */
typedef struct RNetLobbyDesyncReport {
    uint32_t    tick;
    const char *partition;   /* e.g. "wram", "apu", "ppu", "post", "other" */
    uint32_t    mine;
    uint32_t    theirs;
    int         is_host;
    const char *mod_exempt;  /* ';'-separated id@version#sha256; NULL none */
} RNetLobbyDesyncReport;
int  rnet_lobby_report_desync(RNetLobby *lobby, const RNetLobbyDesyncReport *r);

/* ── automatch ───────────────────────────────────────────────────────────── */

typedef struct RNetLobbyRuleset {
    char id[RNET_LOBBY_RULESET_ID_LEN];
    char label[RNET_LOBBY_RULESET_LABEL_LEN];
    char caps_summary[RNET_LOBBY_CAPS_SUMMARY_LEN];
    char game_version[RNET_LOBBY_VERSION_LEN];  /* "" = any release */
    int  max_slots;
    RNetLobbyMatchCaps caps;  /* the server is the host of an automatch room */
} RNetLobbyRuleset;

/* Values match recomp-ui's RECOMP_LAUNCHER_AUTOMATCH_*. */
enum {
    RNET_LOBBY_AUTOMATCH_IDLE = 0,
    RNET_LOBBY_AUTOMATCH_QUEUED = 1,
    RNET_LOBBY_AUTOMATCH_FOUND = 2,
    RNET_LOBBY_AUTOMATCH_ACCEPTED = 3,
    RNET_LOBBY_AUTOMATCH_FAILED = 4
};

typedef struct RNetLobbyAutomatchFound {
    char opponent[RNET_LOBBY_NAME_LEN];          /* handle */
    char opponent_username[RNET_LOBBY_NAME_LEN]; /* @username disambiguator */
    char opponent_country[8];
    char ruleset_id[RNET_LOBBY_RULESET_ID_LEN];
    char ruleset_label[RNET_LOBBY_RULESET_LABEL_LEN];
    char game_version[RNET_LOBBY_VERSION_LEN];
    int  est_rtt_ms;        /* rtt_a + rtt_b; <0 unknown */
    int  accept_secs;       /* seconds left, recomputed on every read */
    int  input_delay;       /* the floored delay the match will run; -1 n/a */
    int  input_prediction;  /* -1 n/a */
    int  frames_needed;     /* 0 = nothing measured; -1 n/a */
} RNetLobbyAutomatchFound;

/* Asks once (refuses to re-send while one is outstanding). */
int  rnet_lobby_automatch_request_rulesets(RNetLobby *lobby);
/* 1 once the server answered with at least one ruleset. */
int  rnet_lobby_automatch_available(RNetLobby *lobby);
int  rnet_lobby_automatch_ruleset_count(RNetLobby *lobby);
int  rnet_lobby_automatch_ruleset_get(RNetLobby *lobby, int index,
                                      RNetLobbyRuleset *out);
/* ruleset_id NULL/"" = the first. mods_enabled is the caller's assertion that
 * a sim-affecting mod is on (the server refuses true); mod_exempt the
 * ';'-separated evidence for cosmetic exemptions, sent as an array. 0 sent;
 * <0 refused locally (not connected, no fingerprint, already queued); a
 * server refusal arrives as state FAILED with automatch_error(). */
int  rnet_lobby_automatch_queue(RNetLobby *lobby, const char *ruleset_id,
                                int mods_enabled, const char *mod_exempt);
int  rnet_lobby_automatch_cancel(RNetLobby *lobby);
int  rnet_lobby_automatch_state(RNetLobby *lobby);
int  rnet_lobby_automatch_queued_secs(RNetLobby *lobby);
int  rnet_lobby_automatch_pool(RNetLobby *lobby);
int  rnet_lobby_automatch_found_get(RNetLobby *lobby,
                                    RNetLobbyAutomatchFound *out);
/* The floored caps offered with the current FOUND (valid=0 if none). */
const RNetLobbyMatchCaps *rnet_lobby_automatch_found_caps(RNetLobby *lobby);
/* accept != 0 accepts; 0 declines and takes the dodge strike. */
int  rnet_lobby_automatch_accept(RNetLobby *lobby, int accept);
/* Non-zero while seated in a room AUTOMATCH created (there is no host to
 * rematch with -- leave it after the match). */
int  rnet_lobby_automatch_room(RNetLobby *lobby);
/* Refuse locally with the reason the player sees. */
void rnet_lobby_automatch_refuse_local(RNetLobby *lobby, const char *why);
const char *rnet_lobby_automatch_error(RNetLobby *lobby);
/* This client's measured RTT to the relay, -1 until probed. */
int  rnet_lobby_automatch_rtt_ms(RNetLobby *lobby);

/* ── JSON helpers (for the title's own keys) ─────────────────────────────── */

/* `json` is an object "{...}" or a bare member list "\"a\":1,\"b\":2" (the
 * shape of RNetLobbyMatchCaps.game_json). Lookups are TOP-LEVEL only: a key
 * inside a nested object never answers for the outer one. `out` must not
 * overlap `json`. */
/* 1 found (out unescaped, \uXXXX -> UTF-8), 0 absent / not a string (out ""),
 * -1 found but truncated to cap. */
int  rnet_lobby_json_get_str(const char *json, const char *key, char *out,
                             size_t cap);
int  rnet_lobby_json_get_int(const char *json, const char *key, int def);
/* true/false; a number reads as nonzero; anything else -> def. */
int  rnet_lobby_json_get_bool(const char *json, const char *key, int def);
/* Copies the value's JSON text verbatim (object, array, scalar). 1 ok, 0
 * absent or does not fit. */
int  rnet_lobby_json_get_raw(const char *json, const char *key, char *out,
                             size_t cap);
/* JSON-escapes `in` (" \\ \n \r \t escaped, other controls dropped). Needs
 * cap >= 2*strlen(in)+1 to never truncate. Returns bytes written. */
size_t rnet_lobby_json_escape(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NET_LOBBY_CLIENT_H */
