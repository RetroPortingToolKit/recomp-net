/* rnet_lobby_latency.c -- every latency measurement the lobby makes, and the
 * host-side advertising that feeds them.
 *
 *   RNET_LOBBY_RTT_WS_SIGNAL   snesrecomp: ping/pong over the seated signal
 *                              relay (types 100..102).
 *   RNET_LOBBY_RTT_PEER_PATH   psxrecomp: UDP probe to the peer's game
 *                              endpoint (rtt_probe.h) plus an ICE/TURN data
 *                              probe (ice_rtt.h) that works through CGNAT,
 *                              with `path_report` telemetry.
 *   cfg.list_latency           psxrecomp: one-shot UDP pings to each listed
 *                              room's LAN beacon / public endpoint.
 *   cfg.lan_beacon             psxrecomp: the host announces its LAN endpoint
 *                              on the local broadcast beacon (never the hub).
 *   cfg.host_advertise         psxrecomp: STUN-discover the host's public
 *                              mapping and publish it (set_host_endpoint).
 */
#include "platform/rnet_platform.h"
#include "lobby/rnet_lobby_internal.h"

#include "recomp_net/address.h"
#include "recomp_net/ice_rtt.h"
#include "recomp_net/lan_beacon.h"
#include "recomp_net/rtt_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HOST_ADV_IDLE = 0, HOST_ADV_WAIT_TURN = 1, HOST_ADV_DONE = 2 };

/* ── address classification ──────────────────────────────────────────────── */

static int parse_ipv4(const char *host, unsigned o[4])
{
    unsigned v[4];
    int i = 0;
    const char *p = host;
    if (!host || !host[0])
        return 0;
    for (i = 0; i < 4; ++i) {
        unsigned x = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            x = x * 10u + (unsigned)(*p - '0');
            if (++digits > 3 || x > 255u)
                return 0;
            ++p;
        }
        if (!digits)
            return 0;
        v[i] = x;
        if (i < 3) {
            if (*p != '.')
                return 0;
            ++p;
        }
    }
    if (*p)
        return 0;
    memcpy(o, v, sizeof(v));
    return 1;
}

static int ipv4_rfc1918(const unsigned o[4])
{
    return o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) ||
           (o[0] == 192 && o[1] == 168);
}

static int host_is_loopback(const char *h)
{
    return h && (strncmp(h, "127.", 4) == 0 || strcmp(h, "::1") == 0 ||
                 strcmp(h, "localhost") == 0);
}

static int host_is_rfc1918(const char *h)
{
    unsigned o[4];
    return parse_ipv4(h, o) && ipv4_rfc1918(o);
}

static int endpoint_is_loopback(const char *ep)
{
    char host[128];
    int port;
    if (!rnet_lobby__endpoint_host_port(ep, host, sizeof(host), &port))
        return ep && (strncmp(ep, "127.", 4) == 0 || strncmp(ep, "localhost", 9) == 0);
    return host_is_loopback(host);
}

static int endpoint_is_public_ipv4(const char *ep)
{
    char host[128];
    unsigned o[4];
    int port = 0;
    if (!rnet_lobby__endpoint_host_port(ep, host, sizeof(host), &port))
        return 0;
    if (!parse_ipv4(host, o))
        return 0;
    if (o[0] == 0 || o[0] == 127 || o[0] >= 224)
        return 0;
    if (ipv4_rfc1918(o) || (o[0] == 169 && o[1] == 254))
        return 0;
    return 1;
}

/* The lobby WebSocket and the UDP relay are the same server process. Rewrite
 * the relay host only when it improves reachability: keep an RFC1918
 * advertise (the server already picked a LAN target); use the WS peer IP when
 * THAT is private/loopback and the advertise is public, loopback or a name;
 * else fall back to the URL host for a loopback advertise. */
int rnet_lobby__rewrite_relay_endpoint(RNetLobby *l, char *ep, size_t cap)
{
    char rh[128];
    const char *use = NULL;
    int rport = 0, n;
    if (!ep || !cap || !rnet_lobby__endpoint_host_port(ep, rh, sizeof(rh), &rport))
        return 0;
    if (host_is_rfc1918(rh))
        return 0;
    if (l->c.peer_ip[0] &&
        (host_is_rfc1918(l->c.peer_ip) || host_is_loopback(l->c.peer_ip)))
        use = l->c.peer_ip;
    else if (host_is_loopback(rh) && l->c.host[0] && !host_is_loopback(l->c.host))
        use = l->c.host;
    if (!use || !use[0] || strcmp(rh, use) == 0)
        return 0;
    n = snprintf(ep, cap, "%s:%d", use, rport);
    if (n <= 0 || (size_t)n >= cap)
        return -1;
    return 1;
}

static int my_bind_port(RNetLobby *l)
{
    char host[128];
    int port = l->cfg.host_port;
    if (l->c.my_bind[0] &&
        rnet_lobby__endpoint_host_port(l->c.my_bind, host, sizeof(host), &port))
        return port;
    return l->cfg.host_port;
}

/* The host's LAN endpoint: the bind's own address when it names an RFC1918
 * NIC (the host picked it); otherwise, for a 0.0.0.0 bind, the first private
 * address the OS lists (LAN-capable addresses come first). */
static int host_lan_endpoint(RNetLobby *l, char *out, size_t cap)
{
    char host[128];
    int port = my_bind_port(l);
    out[0] = '\0';
    host[0] = '\0';
    if (l->c.my_bind[0])
        (void)rnet_lobby__endpoint_host_port(l->c.my_bind, host, sizeof(host), &port);
    if (host_is_rfc1918(host)) {
        snprintf(out, cap, "%s:%d", host, port);
        return 1;
    }
    if (!host[0] || strcmp(host, "0.0.0.0") == 0) {
        RNetIpv4Address addrs[16];
        int n = rnet_ipv4_enumerate(addrs, sizeof(addrs) / sizeof(addrs[0]));
        int i;
        if (n > (int)(sizeof(addrs) / sizeof(addrs[0])))
            n = (int)(sizeof(addrs) / sizeof(addrs[0]));
        for (i = 0; i < n; ++i) {
            if (host_is_rfc1918(addrs[i].address)) {
                snprintf(out, cap, "%s:%d", addrs[i].address, port);
                return 1;
            }
        }
    }
    return 0;
}

/* ── UDP waiting-room probe (PEER_PATH) ──────────────────────────────────── */

static void udp_rtt_close(RNetLobby *l)
{
    rnet_rtt_probe_close(&l->c.rtt_probe);
}

static void store_for_peer(RNetLobby *l, const char *peer_id, int ms)
{
    int idx;
    if (ms < 0 || !peer_id || !peer_id[0])
        return;
    idx = rnet_lobby__rtt_index_for_slot(l, rnet_lobby__member_slot_for_player(l, peer_id));
    if (idx < 0 || idx >= RNET_LOBBY_MAX_MEMBERS)
        return;
    /* The pessimistic sample: an optimistic REPORT must not undercut a higher
     * measurement and starve the delay floor. */
    if (l->c.member_rtt_ms[idx] < 0 || ms > l->c.member_rtt_ms[idx])
        l->c.member_rtt_ms[idx] = ms;
}

static int first_remote_peer(RNetLobby *l, char *out, size_t cap)
{
    int i;
    out[0] = '\0';
    for (i = 0; i < l->c.member_count; ++i) {
        if (!l->c.members[i].player_id[0])
            continue;
        if (l->c.player_id[0] && !strcmp(l->c.members[i].player_id, l->c.player_id))
            continue;
        snprintf(out, cap, "%s", l->c.members[i].player_id);
        return 0;
    }
    return -1;
}

static void report_rtt(RNetLobby *l, int ms)
{
    char report[32];
    if (l->c.is_host || ms == l->c.last_rtt_report)
        return;
    snprintf(report, sizeof(report), "%d", ms);
    if (rnet_lobby_send_signal(l, RNET_LOBBY_SIG_RTT_REPORT, 0, report) == 0)
        l->c.last_rtt_report = ms;
}

static int waiting_room_active(RNetLobby *l)
{
    return l->c.in_lobby && !l->c.launch_pending && !l->c.ice_rtt_suspended;
}

static void udp_rtt_tick(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    const char *peer = NULL;
    int ms = 0;
    if (!waiting_room_active(l) || rnet_lobby__using_server_input_relay(l, &c->join)) {
        udp_rtt_close(l);
        return;
    }
    if (!c->rtt_probe) {
        const char *bind = c->my_bind[0] ? c->my_bind : NULL;
        /* 3+ seat guests bind ephemerally for the session; probe the same. */
        if (!c->is_host && c->join.max_slots >= 3)
            bind = NULL;
        if (rnet_rtt_probe_open(&c->rtt_probe, bind) != 0)
            return;
    }
    if (rnet_lobby__endpoint_usable(c->join.peer_hostport))
        peer = c->join.peer_hostport;
    else if (!c->is_host && rnet_lobby__endpoint_usable(c->join.host_endpoint))
        peer = c->join.host_endpoint;
    if (peer)
        (void)rnet_rtt_probe_set_peer(c->rtt_probe, peer);
    if (rnet_rtt_probe_pump(c->rtt_probe, &ms) == 1) {
        char pid[RNET_LOBBY_ID_LEN];
        if (first_remote_peer(l, pid, sizeof(pid)) == 0)
            store_for_peer(l, pid, ms);
        report_rtt(l, ms);
    }
    {
        uint64_t now = rnet_lobby__now_ms();
        if (now >= c->rtt_next_ping_ms && rnet_rtt_probe_peer_known(c->rtt_probe)) {
            (void)rnet_rtt_probe_ping(c->rtt_probe);
            c->rtt_next_ping_ms = now + 2500ull;
        }
    }
}

/* ── ICE waiting-room probe (PEER_PATH, RNET_ENABLE_ICE) ─────────────────── */

static void ice_rtt_close(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    rnet_ice_rtt_close(&c->ice_rtt);
    c->ice_rtt_peer_id[0] = '\0';
    c->ice_rtt_force_relay = 0;
    c->ice_rtt_last_log_ms = 0;
    c->ice_path_reported[0] = '\0';
    c->ice_path_report_ms = 0;
    /* Through a launch and the match the gate stays open for the match's
     * ICE; in the waiting room it closes until the probe restarts. */
    if (!c->launch_pending && !c->ice_rtt_suspended &&
        l->cfg.waiting_room_rtt == RNET_LOBBY_RTT_PEER_PATH)
        c->ice_signal_accept = 0;
}

#if defined(RNET_ENABLE_ICE)
static void ice_rtt_emit(const RNetSignal *msg, void *user)
{
    RNetLobby *l = (RNetLobby *)user;
    if (!msg || !l)
        return;
    (void)rnet_lobby_send_signal(l, (int)msg->type, (int)msg->flag, msg->text);
}
#endif

static void ice_rtt_tick(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    if (!waiting_room_active(l) || c->member_count < 2) {
        if (c->ice_rtt)
            ice_rtt_close(l);
        return;
    }
#if !defined(RNET_ENABLE_ICE)
    return;
#else
    {
        char peer_id[RNET_LOBBY_ID_LEN];
        int want_force_relay, type = 0, flag = 0, ms;
        char text[RNET_LOBBY_SIG_TEXT];
        char path[16];
        RNetIceState st;
        if (!c->turn.valid) {
            if (!c->turn_request_pending &&
                rnet_lobby__now_ms() >= c->turn_retry_ms)
                (void)rnet_lobby__request_turn(l);
            return;
        }
        want_force_relay = (c->match_caps.valid && c->match_caps.force_turn) ? 1 : 0;
        if (first_remote_peer(l, peer_id, sizeof(peer_id)) != 0) {
            ice_rtt_close(l);
            return;
        }
        /* Queue the peer's SDP even before our agent exists. */
        c->ice_signal_accept = 1;
        if (c->ice_rtt && (strcmp(peer_id, c->ice_rtt_peer_id) != 0 ||
                           want_force_relay != c->ice_rtt_force_relay)) {
            ice_rtt_close(l);
            rnet_lobby_clear_signals(l);
            c->ice_signal_accept = 1;
        }
        if (!c->ice_rtt) {
            RNetIceConfig ice;
            RNetIpv4Address addrs[8];
            int naddr;
            rnet_ice_config_init_defaults(&ice);
            ice.controlling = c->is_host ? 1u : 0u;
            ice.force_relay = want_force_relay ? 1u : 0u;
            if (c->turn.stun_host[0]) {
                ice.stun_host = c->turn.stun_host;
                ice.stun_port = (rnet_u16)(c->turn.stun_port > 0 ? c->turn.stun_port : 3478);
            }
            if (c->turn.turn_host[0] && c->turn.username[0] && c->turn.password[0]) {
                ice.turn_host = c->turn.turn_host;
                ice.turn_user = c->turn.username;
                ice.turn_pass = c->turn.password;
                ice.turn_port = (rnet_u16)(c->turn.turn_port > 0 ? c->turn.turn_port : 3478);
            } else if (want_force_relay) {
                return; /* relay-only without credentials: wait for the mint */
            }
            naddr = rnet_ipv4_enumerate(addrs, sizeof(addrs) / sizeof(addrs[0]));
            if (naddr > 0 && addrs[0].address[0]) {
                snprintf(c->ice_rtt_bind, sizeof(c->ice_rtt_bind), "%s",
                         addrs[0].address);
                ice.bind_address = c->ice_rtt_bind;
            }
            ice.bind_port = 0; /* never the game port STUN advertise uses */
            if (rnet_ice_rtt_open(&c->ice_rtt, &ice, ice_rtt_emit, l) != 0)
                return;
            snprintf(c->ice_rtt_peer_id, sizeof(c->ice_rtt_peer_id), "%s", peer_id);
            c->ice_rtt_force_relay = want_force_relay;
            LOBBY_INFO(l, "ICE RTT probe start (controlling=%d force_relay=%d)",
                       c->is_host ? 1 : 0, want_force_relay);
        }
        while (rnet_lobby_poll_signal(l, &type, &flag, text, sizeof(text))) {
            RNetSignal sig;
            memset(&sig, 0, sizeof(sig));
            if (type == (int)RNET_SIGNAL_LOCAL_SDP)
                type = (int)RNET_SIGNAL_REMOTE_SDP;
            else if (type == (int)RNET_SIGNAL_LOCAL_CANDIDATE)
                type = (int)RNET_SIGNAL_REMOTE_CANDIDATE;
            sig.type = (RNetSignalType)type;
            sig.flag = (rnet_u8)(flag & 0xFF);
            snprintf(sig.text, sizeof(sig.text), "%s", text);
            rnet_ice_rtt_push_signal(c->ice_rtt, &sig);
        }
        rnet_ice_rtt_pump(c->ice_rtt);
        ms = rnet_ice_rtt_ms(c->ice_rtt);
        if (ms >= 0) {
            if (c->ice_rtt_peer_id[0])
                store_for_peer(l, c->ice_rtt_peer_id, ms);
            report_rtt(l, ms);
        }
        st = rnet_ice_rtt_state(c->ice_rtt);
        rnet_ice_rtt_selected_path(c->ice_rtt, path, sizeof(path));
        {
            uint64_t now = rnet_lobby__now_ms();
            const char *report = NULL;
            if (now - c->ice_rtt_last_log_ms >= 3000ull) {
                c->ice_rtt_last_log_ms = now;
                LOBBY_DEBUG(l, "ICE RTT state=%s path=%s rtt=%d",
                            rnet_ice_state_name(st), path, ms);
            }
            /* Telemetry only: the server picks the transport. */
            if (!strcmp(path, "host") || !strcmp(path, "srflx") || !strcmp(path, "prflx"))
                report = "direct";
            else if (!strcmp(path, "relay"))
                report = "relay";
            else if (!strcmp(path, "failed"))
                report = "fail";
            if (report && c->member_count == 2 &&
                (strcmp(report, c->ice_path_reported) != 0 ||
                 now - c->ice_path_report_ms >= 10000ull)) {
                char msg[96];
                snprintf(msg, sizeof(msg), "{\"op\":\"path_report\",\"path\":\"%s\"}",
                         report);
                (void)rnet_lobby__send(l, msg);
                snprintf(c->ice_path_reported, sizeof(c->ice_path_reported), "%s", report);
                c->ice_path_report_ms = now ? now : 1ull;
                LOBBY_INFO(l, "path_report %s (telemetry)", report);
            }
        }
    }
#endif
}

/* ── WS signal probe (WS_SIGNAL) ─────────────────────────────────────────── */

static void ws_rtt_tick(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    uint64_t now;
    /* Guests probe the host about once a second while seated. */
    if (!waiting_room_active(l) || c->is_host)
        return;
    now = rnet_lobby__now_ms();
    if (now >= c->rtt_next_ping_ms) {
        char ts[32];
        snprintf(ts, sizeof(ts), "%llu", (unsigned long long)now);
        (void)rnet_lobby_send_signal_to(l, c->host_player_id, RNET_LOBBY_SIG_RTT_PING,
                                        0, ts);
        c->rtt_next_ping_ms = now + 1000ull;
    }
}

int rnet_lobby__lat_on_signal(RNetLobby *l, int type, const char *text,
                              const char *from)
{
    RNetLobbyConn *c = &l->c;
    const int ws = l->cfg.waiting_room_rtt == RNET_LOBBY_RTT_WS_SIGNAL;
    if (type == RNET_LOBBY_SIG_RTT_PING) {
        /* Only the host answers, and to the asker: a broadcast pong was timed
         * by every guest against its own clock. */
        if (ws && c->is_host)
            (void)rnet_lobby_send_signal_to(l, from, RNET_LOBBY_SIG_RTT_PONG, 0, text);
        return 1;
    }
    if (type == RNET_LOBBY_SIG_RTT_PONG) {
        unsigned long long sent = 0;
        uint64_t now = rnet_lobby__now_ms();
        if (ws && text && sscanf(text, "%llu", &sent) == 1 && (uint64_t)sent <= now) {
            int ms = (int)(now - (uint64_t)sent);
            int idx;
            if (ms > 60000)
                ms = 60000;
            idx = rnet_lobby__rtt_index_for_slot(
                l, rnet_lobby__member_slot_for_player(l, c->player_id));
            if (idx >= 0 && idx < RNET_LOBBY_MAX_MEMBERS)
                c->member_rtt_ms[idx] = ms;
            {
                char report[32];
                snprintf(report, sizeof(report), "%d", ms);
                (void)rnet_lobby_send_signal(l, RNET_LOBBY_SIG_RTT_REPORT, 0, report);
            }
        }
        return 1;
    }
    if (type == RNET_LOBBY_SIG_RTT_REPORT) {
        int idx = rnet_lobby__rtt_index_for_slot(
            l, rnet_lobby__member_slot_for_player(l, from));
        int ms = text ? (int)strtol(text, NULL, 10) : -1;
        if (idx >= 0 && idx < RNET_LOBBY_MAX_MEMBERS && ms >= 0 && ms <= 60000) {
            if (ws || c->member_rtt_ms[idx] < 0 || ms > c->member_rtt_ms[idx])
                c->member_rtt_ms[idx] = ms;
        }
        return 1;
    }
    return 0;
}

/* ── LAN beacon ──────────────────────────────────────────────────────────── */

static void beacon_publish(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    RNetLanBeaconRoom room;
    char ep[RNET_LOBBY_ENDPOINT_LEN];
    if (!c->is_host || !c->in_lobby || c->launch_pending || !c->join.lobby_id[0])
        return;
    if (!host_lan_endpoint(l, ep, sizeof(ep))) {
        rnet_lan_beacon_close(&c->beacon_pub);
        return;
    }
    if (!c->beacon_pub && rnet_lan_beacon_publish_open(&c->beacon_pub, 0) != 0)
        return;
    memset(&room, 0, sizeof(room));
    snprintf(room.lobby_id, sizeof(room.lobby_id), "%s", c->join.lobby_id);
    snprintf(room.endpoint, sizeof(room.endpoint), "%s", ep);
    snprintf(room.game_name, sizeof(room.game_name), "%s", l->game_name);
    /* The beacon's version field is shorter than a pin may be; a TRUNCATED
     * pin would be a wrong one, so an over-long pin is announced as none. */
    if (strlen(l->game_version) < sizeof(room.game_version))
        memcpy(room.game_version, l->game_version, strlen(l->game_version) + 1);
    snprintf(room.room_name, sizeof(room.room_name), "%s", c->room_name);
    room.has_password = c->room_has_password;
    room.player_count = c->join.player_count;
    room.max_slots = c->join.max_slots;
    room.started = 0;
    if (rnet_lan_beacon_publish_set_room(c->beacon_pub, &room) != 0) {
        rnet_lan_beacon_close(&c->beacon_pub);
        return;
    }
    LOBBY_INFO(l, "LAN beacon publish %s -> %s", c->join.lobby_id, ep);
}

static void list_rtt_start(RNetLobby *l, int force_all);

static void beacon_tick(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    if (c->is_host && c->in_lobby && !c->launch_pending) {
        if (!c->beacon_pub || c->beacon_last_count != c->join.player_count ||
            c->beacon_last_max != c->join.max_slots) {
            c->beacon_last_count = c->join.player_count;
            c->beacon_last_max = c->join.max_slots;
            beacon_publish(l);
        }
        if (c->beacon_pub)
            (void)rnet_lan_beacon_publish_tick(c->beacon_pub);
    } else if (c->beacon_pub) {
        rnet_lan_beacon_close(&c->beacon_pub);
    }
    /* Guests (and hosts browsing after leave) listen for same-LAN rooms. */
    if (!c->is_host || !c->in_lobby) {
        int updated = 0;
        if (!c->beacon_listen)
            (void)rnet_lan_beacon_listen_open(&c->beacon_listen, 0);
        if (c->beacon_listen)
            updated = rnet_lan_beacon_listen_pump(c->beacon_listen);
        /* A new announce: re-probe rows still missing latency. */
        if (updated > 0 && l->cfg.list_latency && c->list_count > 0 &&
            !c->list_rtt_active) {
            int i;
            for (i = 0; i < c->list_count; ++i)
                if (c->list[i].latency_ms < 0) {
                    list_rtt_start(l, 0);
                    break;
                }
        }
    }
}

/* ── host advertise ──────────────────────────────────────────────────────── */

/* A public UDP mapping for the game port. A TURN server on the same LAN can
 * map to RFC1918: rejected, and retried against the default STUN. */
static int host_stun_public(RNetLobby *l, char *out, size_t out_len)
{
    RNetExternalIpv4Config stun;
    char any_bind[64];
    char endpoint[RNET_ENDPOINT_TEXT_MAX];
    int attempt, last_rc = RNET_EXTERNAL_IPV4_ERR_ARGUMENT;
    out[0] = '\0';
    snprintf(any_bind, sizeof(any_bind), "0.0.0.0:%d", my_bind_port(l));
    for (attempt = 0; attempt < 4; ++attempt) {
        const char *bind_hp;
        endpoint[0] = '\0';
        rnet_external_ipv4_config_init(&stun);
        if (attempt < 2 && l->c.turn.valid && l->c.turn.stun_host[0]) {
            stun.stun_host = l->c.turn.stun_host;
            stun.stun_port = (unsigned short)l->c.turn.stun_port;
        }
        /* 0: coturn+bind, 1: coturn+any, 2: default+bind, 3: default+any */
        bind_hp = (attempt & 1) ? any_bind
                                : (l->c.my_bind[0] ? l->c.my_bind : any_bind);
        last_rc = rnet_external_udp_endpoint_discover(&stun, bind_hp, endpoint,
                                                      sizeof(endpoint));
        if (last_rc != RNET_EXTERNAL_IPV4_OK || !endpoint[0])
            continue;
        if (!endpoint_is_public_ipv4(endpoint)) {
            LOBBY_DEBUG(l, "STUN mapped private %s (bind=%s) -- retrying", endpoint,
                        bind_hp);
            continue;
        }
        snprintf(out, out_len, "%s", endpoint);
        return 0;
    }
    return last_rc != 0 ? last_rc : -1;
}

static void host_advertise_tick(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    char endpoint[RNET_ENDPOINT_TEXT_MAX];
    char msg[256];
    char ep_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_ENDPOINT_LEN)];
    int rc;
    if (c->host_adv_state != HOST_ADV_WAIT_TURN)
        return;
    if (!c->is_host || !c->in_lobby || c->launch_pending) {
        c->host_adv_state = HOST_ADV_IDLE;
        return;
    }
    if (rnet_lobby__using_server_input_relay(l, &c->join)) {
        c->host_adv_state = HOST_ADV_DONE;
        return;
    }
    /* Prefer the TURN server's STUN; do not wait for it forever. */
    if (!c->turn.valid && rnet_lobby__now_ms() < c->host_adv_deadline_ms)
        return;
    c->host_adv_state = HOST_ADV_DONE;
    if (!c->my_bind[0] || endpoint_is_loopback(c->join.host_endpoint))
        return;
    /* Free the game port for an exclusive STUN bind. */
    udp_rtt_close(l);
    rc = host_stun_public(l, endpoint, sizeof(endpoint));
    if (rc != 0 || !endpoint[0]) {
        LOBBY_INFO(l, "STUN advertise failed (%d) -- keeping %s", rc,
                   c->join.host_endpoint[0] ? c->join.host_endpoint : "(none)");
        return;
    }
    snprintf(c->join.host_endpoint, sizeof(c->join.host_endpoint), "%s", endpoint);
    rnet_json_escape(c->join.host_endpoint, ep_esc, sizeof(ep_esc));
    snprintf(msg, sizeof(msg), "{\"op\":\"set_host_endpoint\",\"host_endpoint\":\"%s\"}",
             ep_esc);
    (void)rnet_lobby__send(l, msg);
    LOBBY_INFO(l, "advertised host_endpoint=%s (LAN via local beacon)",
               c->join.host_endpoint);
}

/* ── list latency ────────────────────────────────────────────────────────── */

static void list_rtt_close(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    rnet_rtt_probe_close(&c->list_probe);
    c->list_rtt_active = 0;
    c->list_rtt_pend_count = 0;
    memset(c->list_rtt_pend_active, 0, sizeof(c->list_rtt_pend_active));
}

static int cand_already(char cands[][RNET_LOBBY_ENDPOINT_LEN], int n, const char *ep)
{
    int i;
    for (i = 0; i < n; ++i)
        if (!strcmp(cands[i], ep))
            return 1;
    return 0;
}

/* The local beacon's LAN endpoint first, then legacy hub lan_endpoints on our
 * /24, then the rest, then the public endpoint. */
static int row_candidates(RNetLobby *l, const RNetLobbyRow *row,
                          char cands[][RNET_LOBBY_ENDPOINT_LEN], int max_cands)
{
    RNetIpv4Address addrs[16];
    unsigned local[8][4];
    int local_n = 0, n = 0, i, pass, naddr;
    char beacon_ep[RNET_LOBBY_ENDPOINT_LEN];
    beacon_ep[0] = '\0';
    if (l->c.beacon_listen && row->lobby_id[0] &&
        rnet_lan_beacon_lookup(l->c.beacon_listen, row->lobby_id, beacon_ep,
                               sizeof(beacon_ep)) &&
        rnet_lobby__endpoint_usable(beacon_ep) && n < max_cands) {
        snprintf(cands[n++], RNET_LOBBY_ENDPOINT_LEN, "%s", beacon_ep);
    }
    naddr = rnet_ipv4_enumerate(addrs, sizeof(addrs) / sizeof(addrs[0]));
    if (naddr > (int)(sizeof(addrs) / sizeof(addrs[0])))
        naddr = (int)(sizeof(addrs) / sizeof(addrs[0]));
    for (i = 0; i < naddr && local_n < 8; ++i) {
        unsigned o[4];
        if (parse_ipv4(addrs[i].address, o) && ipv4_rfc1918(o))
            memcpy(local[local_n++], o, sizeof(o));
    }
    for (pass = 0; pass < 2; ++pass) {
        for (i = 0; i < row->lan_count && n < max_cands; ++i) {
            char host[64];
            int port = 0, same = 0, j;
            unsigned o[4];
            if (!rnet_lobby__endpoint_usable(row->lan_endpoints[i]))
                continue;
            if (!rnet_lobby__endpoint_host_port(row->lan_endpoints[i], host,
                                                sizeof(host), &port) ||
                !parse_ipv4(host, o) || !ipv4_rfc1918(o))
                continue;
            for (j = 0; j < local_n; ++j)
                if (local[j][0] == o[0] && local[j][1] == o[1] && local[j][2] == o[2]) {
                    same = 1;
                    break;
                }
            if ((pass == 0) != (same != 0))
                continue;
            if (cand_already(cands, n, row->lan_endpoints[i]))
                continue;
            snprintf(cands[n++], RNET_LOBBY_ENDPOINT_LEN, "%s", row->lan_endpoints[i]);
        }
    }
    if (n < max_cands && rnet_lobby__endpoint_usable(row->host_endpoint) &&
        !cand_already(cands, n, row->host_endpoint))
        snprintf(cands[n++], RNET_LOBBY_ENDPOINT_LEN, "%s", row->host_endpoint);
    return n;
}

static void list_rtt_start(RNetLobby *l, int force_all)
{
    RNetLobbyConn *c = &l->c;
    int i;
    list_rtt_close(l);
    if (c->list_count <= 0)
        return;
    if (l->cfg.lan_beacon) {
        /* Drain local beacons first: they may beat the WS list. */
        if (!c->beacon_listen)
            (void)rnet_lan_beacon_listen_open(&c->beacon_listen, 0);
        if (c->beacon_listen)
            (void)rnet_lan_beacon_listen_pump(c->beacon_listen);
    }
    if (rnet_rtt_probe_open(&c->list_probe, NULL) != 0)
        return;
    for (i = 0; i < c->list_count; ++i) {
        char cands[RNET_LOBBY_MAX_LAN_EPS + 1][RNET_LOBBY_ENDPOINT_LEN];
        int cn, k;
        if (force_all)
            c->list[i].latency_ms = -1;
        if (c->list[i].latency_ms >= 0)
            continue;
        cn = row_candidates(l, &c->list[i], cands, RNET_LOBBY_MAX_LAN_EPS + 1);
        for (k = 0; k < cn && c->list_rtt_pend_count < RNET_LOBBY_MAX_PROBE_PEND; ++k) {
            unsigned long long sent = 0;
            int slot = c->list_rtt_pend_count;
            if (rnet_rtt_probe_set_peer(c->list_probe, cands[k]) != 0)
                continue;
            if (rnet_rtt_probe_ping_ts(c->list_probe, &sent) != 0)
                continue;
            c->list_rtt_sent_ts[slot] = sent;
            c->list_rtt_lobby_idx[slot] = i;
            c->list_rtt_pend_active[slot] = 1;
            c->list_rtt_pend_count++;
        }
    }
    if (c->list_rtt_pend_count <= 0) {
        list_rtt_close(l);
        return;
    }
    c->list_rtt_active = 1;
    /* STUN advertise briefly drops the host's answering socket. */
    c->list_rtt_deadline_ms = rnet_lobby__now_ms() + 1500ull;
}

static void list_rtt_tick(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    int remaining = 0, p;
    if (!c->list_rtt_active || !c->list_probe)
        return;
    for (;;) {
        int ms = 0;
        unsigned long long echo = 0;
        if (rnet_rtt_probe_pump_ex(c->list_probe, &ms, &echo) != 1)
            break;
        for (p = 0; p < c->list_rtt_pend_count; ++p) {
            int li, q;
            if (!c->list_rtt_pend_active[p] || c->list_rtt_sent_ts[p] != echo)
                continue;
            li = c->list_rtt_lobby_idx[p];
            if (li >= 0 && li < c->list_count && c->list[li].latency_ms < 0)
                c->list[li].latency_ms = ms;
            for (q = 0; q < c->list_rtt_pend_count; ++q)
                if (c->list_rtt_lobby_idx[q] == li)
                    c->list_rtt_pend_active[q] = 0;
            break;
        }
    }
    for (p = 0; p < c->list_rtt_pend_count; ++p)
        if (c->list_rtt_pend_active[p])
            ++remaining;
    if (remaining <= 0 || rnet_lobby__now_ms() >= c->list_rtt_deadline_ms)
        list_rtt_close(l);
}

void rnet_lobby__lat_on_list(RNetLobby *l, int want_probe)
{
    RNetLobbyConn *c = &l->c;
    int i;
    if (!l->cfg.list_latency)
        return;
    if (want_probe) {
        list_rtt_start(l, 1);
        return;
    }
    /* Restart even mid-burst: an advertise may have just published a public
     * endpoint or a LAN candidate. */
    for (i = 0; i < c->list_count; ++i)
        if (c->list[i].latency_ms < 0 &&
            (c->list[i].host_endpoint[0] || c->list[i].lan_count > 0)) {
            list_rtt_start(l, 0);
            return;
        }
}

/* ── lifecycle ───────────────────────────────────────────────────────────── */

void rnet_lobby__lat_tick(RNetLobby *l)
{
    if (l->cfg.host_advertise)
        host_advertise_tick(l);
    if (l->cfg.lan_beacon)
        beacon_tick(l);
    if (l->cfg.waiting_room_rtt == RNET_LOBBY_RTT_WS_SIGNAL) {
        ws_rtt_tick(l);
    } else if (l->cfg.waiting_room_rtt == RNET_LOBBY_RTT_PEER_PATH) {
        udp_rtt_tick(l);
        ice_rtt_tick(l);
    }
    if (l->cfg.list_latency)
        list_rtt_tick(l);
}

void rnet_lobby__lat_close_all(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    ice_rtt_close(l);
    udp_rtt_close(l);
    list_rtt_close(l);
    rnet_lan_beacon_close(&c->beacon_pub);
    rnet_lan_beacon_close(&c->beacon_listen);
    c->host_adv_state = HOST_ADV_IDLE;
    c->host_adv_deadline_ms = 0;
    c->list_rtt_on_next_list = 0;
}

void rnet_lobby__lat_on_created(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    if (l->cfg.host_advertise) {
        c->host_adv_state = HOST_ADV_WAIT_TURN;
        c->host_adv_deadline_ms = rnet_lobby__now_ms() + 500ull;
    }
    if (l->cfg.lan_beacon)
        beacon_publish(l);
}

void rnet_lobby__lat_on_launch(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    /* The match's ICE must not be stolen by a waiting-room probe. */
    ice_rtt_close(l);
    udp_rtt_close(l);
    rnet_lan_beacon_close(&c->beacon_pub);
    c->host_adv_state = HOST_ADV_IDLE;
}

void rnet_lobby__lat_on_leave(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    c->ice_rtt_suspended = 0;
    ice_rtt_close(l);
    udp_rtt_close(l);
    rnet_lan_beacon_close(&c->beacon_pub);
    c->host_adv_state = HOST_ADV_IDLE;
}

void rnet_lobby_resume_waiting_room_rtt(RNetLobby *l)
{
    if (!l)
        return;
    l->c.ice_rtt_suspended = 0;
    ice_rtt_close(l);
    udp_rtt_close(l);
}
