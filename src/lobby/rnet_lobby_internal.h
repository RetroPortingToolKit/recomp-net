/* rnet_lobby_internal.h -- the lobby client's private state and the seams
 * between its translation units. Tests include this (with src/ on their
 * include path) to drive the real parsers without a socket; nothing outside
 * recomp_net_lobby and its tests may.
 */
#ifndef RNET_LOBBY_INTERNAL_H
#define RNET_LOBBY_INTERNAL_H

#include "recomp_net/lobby_client.h"
#include "recomp_net/ice.h"
#include "recomp_net/ice_rtt.h"
#include "recomp_net/ice_xfer.h"
#include "recomp_net/lan_beacon.h"
#include "recomp_net/rtt_probe.h"
#include "lobby/rnet_lobby_json.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Signal types this client speaks on the seated `signal` relay, beside the
 * game's own ICE (RNetSignalType 1..6). */
enum {
    RNET_LOBBY_SIG_RTT_PING = 100,
    RNET_LOBBY_SIG_RTT_PONG = 101,
    RNET_LOBBY_SIG_RTT_REPORT = 102,
    /* Mod transfer: rides the seated relay, which the server forwards
     * verbatim, so it needs no server support. Only SDP and candidates. */
    RNET_LOBBY_SIG_MOD_REQ = 110,      /* guest -> host "<id>@<ver>" */
    RNET_LOBBY_SIG_MOD_NAK = 111,      /* host -> guest, text = reason */
    RNET_LOBBY_SIG_MOD_ICE_BASE = 120  /* + RNetSignalType */
};

#define RNET_LOBBY_RX_CAP        (128u * 1024u)
#define RNET_LOBBY_MSG_CAP       (128u * 1024u)
#define RNET_LOBBY_TX_QUEUE       32
#define RNET_LOBBY_SIG_QUEUE      32
#define RNET_LOBBY_SIG_TEXT      2048
#define RNET_LOBBY_SIG_HOLD       24
#define RNET_LOBBY_MEMBER_JSON   (RNET_LOBBY_MAX_MODS * 256 + 1024)
#define RNET_LOBBY_MAX_PROBE_PEND (RNET_LOBBY_MAX_LIST * (RNET_LOBBY_MAX_LAN_EPS + 1))
#define RNET_LOBBY_BLOCKS_CAP    (256 * 41)
/* Worst-case escaped size of one {id,ver,n,f} package row. */
#define RNET_LOBBY_MOD_ROW_JSON  832

typedef void (*RNetLobbyTestTxFn)(void *user, const char *frame);

typedef struct RNetLobbyConnectJob RNetLobbyConnectJob;

typedef struct RNetLobbyChatRing {
    RNetLobbyChatMsg msg[RNET_LOBBY_CHAT_RING];
    int head;
    int count;
    uint32_t seq;
} RNetLobbyChatRing;

/* Automatch: its own lifetime (the queue, not the room). */
typedef struct RNetLobbyAutomatch {
    int  have_rulesets;
    int  rulesets_in_flight;   /* the launcher polls every frame */
    int  ruleset_count;
    RNetLobbyRuleset rulesets[RNET_LOBBY_MAX_RULESETS];

    int  state;
    char ticket_id[RNET_LOBBY_ID_LEN];
    char match_id[RNET_LOBBY_ID_LEN];
    int  queued_secs;
    int  pool;
    char error[160];

    RNetLobbyAutomatchFound found;
    RNetLobbyMatchCaps found_caps;
    uint64_t found_deadline_ms;

    char     probe_host[160];
    int      probe_port;
    unsigned probe_magic;
    int      probe_type;
    int      rtt_ms;           /* <0 not measured */
    uint32_t probe_nonce;
    uint64_t probe_sent_ms;    /* 0 none outstanding */
    int      probe_socket;     /* -1 closed */
    int      rtt_reported;
    int      queue_in_flight;
    int      in_automatch_room;
} RNetLobbyAutomatch;

/* Everything that belongs to ONE connection. Reset (after closing what it
 * owns) on connect / disconnect; nothing configured by the title lives here --
 * the snesrecomp client learned that when its transfer hooks lived in the
 * per-connection struct and were wiped by the reset every connect() does. */
typedef struct RNetLobbyConn {
    int  fd;
    int  connected;
    int  handshake_done;
    int  welcomed;
    char url[RNET_LOBBY_URL_LEN];
    char host[128];
    int  port;
    char path[128];
    char peer_ip[64];          /* actual TCP peer (split horizon) */

    char     rx_http[4096];
    size_t   rx_http_len;
    uint8_t  rx[RNET_LOBBY_RX_CAP];
    size_t   rx_len;
    char     msg[RNET_LOBBY_MSG_CAP];   /* fragment assembly */
    size_t   msg_len;
    int      msg_active;
    int      msg_overflow;

    char    *txq[RNET_LOBBY_TX_QUEUE];  /* frames held until the handshake */
    int      txq_n;
    int      tx_failed;                 /* a write failed; pump disconnects */
    char     ws_key[32];                /* our Sec-WebSocket-Key */
    uint64_t handshake_deadline_ms;

    char player_id[RNET_LOBBY_ID_LEN];
    char accepted_name[RNET_LOBBY_NAME_LEN];
    char name_refused[32];
    int  session_invalid;
    char last_report_ack[RNET_LOBBY_MID_LEN];

    RNetLobbyRow list[RNET_LOBBY_MAX_LIST];
    int  list_count;
    RNetLobbyOnlinePlayer online[RNET_LOBBY_MAX_ONLINE];
    int  online_count;

    int  in_lobby;
    int  is_host;
    char host_player_id[RNET_LOBBY_ID_LEN];
    char my_bind[RNET_LOBBY_ENDPOINT_LEN];
    char room_name[RNET_LOBBY_NAME_LEN];   /* what we created (LAN beacon) */
    int  room_has_password;
    RNetLobbyJoinInfo join;
    RNetLobbyMember members[RNET_LOBBY_MAX_MEMBERS];
    char member_json[RNET_LOBBY_MAX_MEMBERS][RNET_LOBBY_MEMBER_JSON];
    RNetLobbyModPkg member_offer[RNET_LOBBY_MAX_MEMBERS][RNET_LOBBY_MAX_MODS];
    int  member_offer_count[RNET_LOBBY_MAX_MEMBERS];  /* -1 unknown */
    int  member_count;
    int  local_ready;
    int  spectator_offer_sent;  /* auto_ready: the gallery's one announce */
    int  all_ready;
    int  launch_pending;
    RNetLobbyMatchCaps match_caps;

    RNetLobbyChatRing chat;
    RNetLobbyChatRing schat;

    int  swap_in_valid;
    char swap_in_asker_id[RNET_LOBBY_ID_LEN];
    char swap_in_asker_name[RNET_LOBBY_NAME_LEN];
    int  swap_in_from_slot;
    int  swap_out;

    RNetLobbyModPkg need_mods[RNET_LOBBY_MAX_MODS];
    int  need_mods_count;
    int  need_mods_can_transfer;
    char need_mods_lobby_id[RNET_LOBBY_ID_LEN];
    char need_mods_host_player_id[RNET_LOBBY_ID_LEN];

    struct {
        int type;
        int flag;
        char text[RNET_LOBBY_SIG_TEXT];
    } sig_q[RNET_LOBBY_SIG_QUEUE];
    int  sig_head;
    int  sig_tail;
    int  sig_count;
    int  ice_signal_accept;
    int  ice_rtt_suspended;     /* launch .. rematch/leave */

    RNetLobbyTurnCredentials turn;
    uint64_t turn_received_ms;
    int  turn_request_pending;
    uint64_t turn_retry_ms;      /* no automatic re-ask before this */

    int      member_rtt_ms[RNET_LOBBY_MAX_MEMBERS];
    uint64_t rtt_next_ping_ms;
    int      last_rtt_report;

    /* mod transfer (one at a time, one direction) */
    RNetIceXfer *xfer;
    int      xfer_busy;
    int      xfer_sending;
    int      xfer_progress;     /* -1 idle, -2 failed, 0..100 */
    char     xfer_peer[RNET_LOBBY_ID_LEN];
    char     xfer_id[RNET_LOBBY_MOD_ID_LEN];
    char     xfer_ver[RNET_LOBBY_MOD_VER_LEN];
    char     xfer_sha[65];
    uint32_t xfer_expect;
    char     xfer_err[256];
    uint8_t *xfer_hold;
    size_t   xfer_hold_len;
    char     xfer_hold_hdr[512];
    int      xfer_path_priced;
    char     ice_stun[128], ice_turn[128], ice_user[192], ice_pass[128];
    RNetSignal sig_hold[RNET_LOBBY_SIG_HOLD];
    int      sig_hold_n;
    char     sig_hold_from[RNET_LOBBY_ID_LEN];
    int      xfer_last_state;
    uint64_t xfer_started_ms;
    uint64_t xfer_connected_ms;

    /* waiting-room / list latency (RNET_LOBBY_RTT_PEER_PATH, list_latency,
     * lan_beacon, host_advertise) */
    RNetRttProbe    *rtt_probe;
    RNetIceRttProbe *ice_rtt;
    char     ice_rtt_peer_id[RNET_LOBBY_ID_LEN];
    int      ice_rtt_force_relay;
    uint64_t ice_rtt_last_log_ms;
    char     ice_path_reported[16];
    uint64_t ice_path_report_ms;
    RNetRttProbe *list_probe;
    int      list_rtt_active;
    int      list_rtt_on_next_list;
    uint64_t list_rtt_deadline_ms;
    unsigned long long list_rtt_sent_ts[RNET_LOBBY_MAX_PROBE_PEND];
    int      list_rtt_lobby_idx[RNET_LOBBY_MAX_PROBE_PEND];
    int      list_rtt_pend_active[RNET_LOBBY_MAX_PROBE_PEND];
    int      list_rtt_pend_count;
    RNetLanBeacon *beacon_pub;
    RNetLanBeacon *beacon_listen;
    int      beacon_last_count;
    int      beacon_last_max;
    char     ice_rtt_bind[64];
    int      host_adv_state;
    uint64_t host_adv_deadline_ms;

    RNetLobbyAutomatch am;
} RNetLobbyConn;

struct RNetLobby {
    /* ---- configuration: survives every reconnect ---- */
    RNetLobbyConfig cfg;
    char cfg_platform[24];
    char cfg_prefix[48];
    char cfg_default_url[RNET_LOBBY_URL_LEN];
    char cfg_url_env[64];
    char cfg_version_env[64];
    char cfg_build_id[96];
    char default_url_buf[RNET_LOBBY_URL_LEN];

    char display_name[RNET_LOBBY_NAME_LEN];
    char game_name[RNET_LOBBY_NAME_LEN];
    char game_version[RNET_LOBBY_VERSION_LEN];   /* presented (override) */
    char real_version[RNET_LOBBY_VERSION_LEN];   /* what the build says */
    int  version_forced;
    char fp[65];
    int  allow_spectators_pref;
    int  max_slots_pref;
    char ready_extra[RNET_LOBBY_READY_EXTRA_LEN];
    char blocks[RNET_LOBBY_BLOCKS_CAP];
    int  blocks_set;

    RNetLobbyModOfferFn    offer_fn;
    void                  *offer_ctx;
    RNetLobbyModExportFn   export_fn;
    RNetLobbyModFreeFn     free_fn;
    RNetLobbyModInstallFn  install_fn;
    void                  *hook_ctx;

    RNetLobbyConnectJob *job;   /* in-flight async connect */
    int  wsa_started;

    RNetLobbyTestTxFn test_tx;
    void *test_tx_user;

    /* ---- per connection ---- */
    RNetLobbyConn c;
};

/* ── shared helpers (rnet_lobby_client.c) ────────────────────────────────── */

void rnet_lobby__log(RNetLobby *l, RNetLobbyLogLevel lv, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
#define LOBBY_DEBUG(l, ...) rnet_lobby__log((l), RNET_LOBBY_LOG_DEBUG, __VA_ARGS__)
#define LOBBY_INFO(l, ...)  rnet_lobby__log((l), RNET_LOBBY_LOG_INFO, __VA_ARGS__)
#define LOBBY_WARN(l, ...)  rnet_lobby__log((l), RNET_LOBBY_LOG_WARN, __VA_ARGS__)
#define LOBBY_ERROR(l, ...) rnet_lobby__log((l), RNET_LOBBY_LOG_ERROR, __VA_ARGS__)

uint64_t rnet_lobby__now_ms(void);
/* Queue (before the handshake) or write. 0 ok, <0 not connected / failed. */
int  rnet_lobby__send(RNetLobby *l, const char *json);
int  rnet_lobby__socket_close(int fd);
int  rnet_lobby__set_nonblock(int fd);

int  rnet_lobby__member_index_for_player(RNetLobby *l, const char *player_id);
int  rnet_lobby__member_slot_for_player(RNetLobby *l, const char *player_id);
int  rnet_lobby__member_is_spectator(RNetLobby *l, const char *player_id);
int  rnet_lobby__rtt_index_for_slot(RNetLobby *l, int slot);
int  rnet_lobby__using_server_input_relay(RNetLobby *l,
                                          const RNetLobbyJoinInfo *j);
int  rnet_lobby__endpoint_usable(const char *endpoint);  /* host:port, port>0 */
int  rnet_lobby__endpoint_host_port(const char *ep, char *host,
                                    size_t host_cap, int *port);
void rnet_lobby__send_set_ready(RNetLobby *l, int ready);
int  rnet_lobby__ice_signal_is_for_us(RNetLobby *l, int type, const char *from);
void rnet_lobby__enqueue_signal(RNetLobby *l, int type, int flag,
                                const char *text);
/* The gate's resting state for this mode: open unless PEER_PATH. */
void rnet_lobby__ice_gate_rest(RNetLobby *l);
int  rnet_lobby__max_players(RNetLobby *l);
int  rnet_lobby__max_spectators(RNetLobby *l);
void rnet_lobby__chat_push(RNetLobby *l, RNetLobbyChatRing *ring,
                           const char *player_id, const char *account,
                           const char *from, const char *country,
                           const char *text, const char *mid, int is_system);
void rnet_lobby__parse_slots(RNetLobby *l, RNetJsonSpan msg);
void rnet_lobby__fill_peer_bind(RNetLobby *l);
int  rnet_lobby__request_turn(RNetLobby *l);

/* Test / parser entry: dispatch one server frame. */
void rnet_lobby__ingest(RNetLobby *l, const char *json);
/* Test / parser entry: append raw bytes as if received after the handshake
 * and run the frame reader. Returns 0, 1 when the reader flagged the stream
 * fatal (close frame, oversized frame), -1 when the bytes do not fit. */
int  rnet_lobby__rx_feed(RNetLobby *l, const void *bytes, size_t n);
/* Test seam: mark the handle connected + welcomed as `player_id`, with every
 * outbound frame handed to `tx` instead of a socket. */
void rnet_lobby__test_attach(RNetLobby *l, const char *player_id,
                             RNetLobbyTestTxFn tx, void *user);

/* ── automatch (rnet_lobby_automatch.c) ──────────────────────────────────── */

void rnet_lobby__am_reset_queue(RNetLobby *l);
void rnet_lobby__am_on_connection_reset(RNetLobby *l);
void rnet_lobby__am_close(RNetLobby *l);
void rnet_lobby__am_poll(RNetLobby *l);
int  rnet_lobby__am_handle_op(RNetLobby *l, const char *op, RNetJsonSpan msg);
int  rnet_lobby__am_claim_error(RNetLobby *l, const char *code,
                                RNetJsonSpan msg);
void rnet_lobby__am_on_joined(RNetLobby *l);
void rnet_lobby__am_on_left(RNetLobby *l);

/* ── mods (rnet_lobby_mods.c) ────────────────────────────────────────────── */

/* `"<key>":[{id,ver,n,f},...]` -> rows; rows missing id or ver (or with one
 * too long to hold) are skipped; a string-encoded plan yields 0. */
int  rnet_lobby__parse_mod_pkgs(RNetJsonSpan obj, const char *key,
                                RNetLobbyModPkg *out, int max);
/* Emits `"<key>":[...]`; 0 when the whole array does not fit. */
int  rnet_lobby__append_mod_pkgs(char *dst, size_t cap, const char *key,
                                 const RNetLobbyModPkg *pkgs, int count);
/* `,"mod_offer":{"v":1,"pkgs":[...]}`; 0 when no supplier / too large. */
int  rnet_lobby__append_mod_offer(RNetLobby *l, char *dst, size_t cap);
int  rnet_lobby__member_missing_count(RNetLobby *l, int member_index);
int  rnet_lobby__mod_ice_type_for_push(int emitted_type);
/* Handles the mod-transfer signal types; 1 when consumed. */
int  rnet_lobby__mod_on_signal(RNetLobby *l, int type, int flag,
                               const char *text, const char *from);
void rnet_lobby__mod_pump(RNetLobby *l);
void rnet_lobby__mod_reset(RNetLobby *l);

/* ── latency (rnet_lobby_latency.c) ──────────────────────────────────────── */

void rnet_lobby__lat_tick(RNetLobby *l);          /* after the socket pump */
void rnet_lobby__lat_close_all(RNetLobby *l);     /* connection reset */
void rnet_lobby__lat_on_created(RNetLobby *l);
void rnet_lobby__lat_on_launch(RNetLobby *l);
void rnet_lobby__lat_on_leave(RNetLobby *l);
void rnet_lobby__lat_clear_members(RNetLobby *l);
/* Called with the list already replaced; prev_* describe the old rows so
 * a row whose endpoints did not change keeps its measured RTT. */
void rnet_lobby__lat_on_list(RNetLobby *l, int want_probe);
/* Handles RTT signal types; 1 when consumed. */
int  rnet_lobby__lat_on_signal(RNetLobby *l, int type, const char *text,
                               const char *from);
/* Rewrites a relay endpoint to the lobby's own host when that improves
 * reachability (same process serves WS and relay). 1 rewritten. */
int  rnet_lobby__rewrite_relay_endpoint(RNetLobby *l, char *ep, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* RNET_LOBBY_INTERNAL_H */
