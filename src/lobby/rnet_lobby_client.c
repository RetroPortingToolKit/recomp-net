/* rnet_lobby_client.c -- the console-agnostic WebSocket lobby client.
 *
 * Lifted from snesrecomp's snes_lobby_client.c (origin/main 284afca) and
 * merged with psxrecomp's psx_lobby_client.c (origin/master) and
 * segagenesisrecomp's genesis_lobby_client.c. Where the copies disagreed the
 * choice is recorded at the site. This file holds the connection, the frame
 * reader, the server-message dispatcher and the room / chat / seat / signal
 * surface; automatch, mods and latency live in their own rnet_lobby_*.c.
 */
#include "platform/rnet_platform.h"   /* winsock first on Windows */
#include "lobby/rnet_lobby_internal.h"

#include "recomp_net/address.h"
#include "recomp_net/auth.h"
#include "recomp_net/chat_filter.h"
#include "recomp_net/chat_report.h"
#include "recomp_net/rnet_ws.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/* ── platform glue ───────────────────────────────────────────────────────── */

static int sock_would_block(void)
{
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static int sock_in_progress(void)
{
#if defined(_WIN32)
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EINPROGRESS || errno == EWOULDBLOCK;
#endif
}

int rnet_lobby__socket_close(int fd)
{
    if (fd < 0)
        return 0;
#if defined(_WIN32)
    return closesocket((SOCKET)fd);
#else
    return close(fd);
#endif
}

int rnet_lobby__set_nonblock(int fd)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

static int set_block(int fd)
{
#if defined(_WIN32)
    u_long mode = 0;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
#endif
}

static int sock_send(int fd, const void *buf, size_t len)
{
#if defined(_WIN32)
    return send((SOCKET)fd, (const char *)buf, (int)len, 0);
#else
    return (int)send(fd, buf, len, 0);
#endif
}

static int sock_recv(int fd, void *buf, size_t len)
{
#if defined(_WIN32)
    return recv((SOCKET)fd, (char *)buf, (int)len, 0);
#else
    return (int)recv(fd, buf, len, 0);
#endif
}

uint64_t rnet_lobby__now_ms(void)
{
    return (uint64_t)rnet_os_monotonic_ms();
}

/* Truncation by bytes can end inside a UTF-8 sequence, and an invalid
 * sequence in a text frame is a protocol error the server drops the
 * connection for. Trim an incomplete trailing sequence. */
static void utf8_trim_tail(char *s, size_t len)
{
    size_t i = len, lead;
    size_t need;
    unsigned char c;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80 && len - i < 3)
        --i;
    if (i == 0)
        return;
    c = (unsigned char)s[i - 1];
    if (c < 0xC0)
        return;
    lead = i - 1;
    need = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    if (len - lead < need)
        s[lead] = '\0';
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!dst || cap == 0)
        return;
    if (!src)
        src = "";
    n = strlen(src);
    if (n + 1 <= cap) {
        memmove(dst, src, n + 1);
        return;
    }
    memmove(dst, src, cap - 1);
    dst[cap - 1] = '\0';
    utf8_trim_tail(dst, cap - 1);
}

/* ── logging ─────────────────────────────────────────────────────────────── */

void rnet_lobby__log(RNetLobby *l, RNetLobbyLogLevel lv, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    if (l && (int)lv < l->cfg.log_min_level)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (l && l->cfg.log) {
        l->cfg.log(l->cfg.log_user, lv, line);
        return;
    }
    fprintf(stderr, "%s: %s\n",
            (l && l->cfg_prefix[0]) ? l->cfg_prefix : "rnet_lobby", line);
}

/* ── configuration ───────────────────────────────────────────────────────── */

void rnet_lobby_config_init(RNetLobbyConfig *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_players = 2;
    cfg->max_spectators = 4;
    cfg->spectator_slot_base = RNET_LOBBY_DEFAULT_SPECTATOR_SLOT_BASE;
    cfg->default_max_slots = 2;
    cfg->host_port = 7777;
    cfg->guest_port = 7778;
    cfg->connect_timeout_ms = 3000;
    cfg->handshake_timeout_ms = 10000;
    cfg->host_bind_all_interfaces = 1;
    cfg->fingerprint_in_rooms = 1;
    cfg->waiting_room_rtt = RNET_LOBBY_RTT_PEER_PATH;
    cfg->list_latency = 1;
    cfg->lan_beacon = 1;
    cfg->host_advertise = 1;
    cfg->caps_input_delay_default = 6;
    cfg->caps_input_delay_min = 0;
    cfg->caps_input_delay_max = 20;
    cfg->caps_input_prediction_default = 10;
    cfg->caps_input_prediction_min = 2;
    cfg->caps_input_prediction_max = 16;
    cfg->caps_rollback_default = 1;
    cfg->log_min_level = RNET_LOBBY_LOG_INFO;
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int rnet_lobby__max_players(RNetLobby *l)
{
    return l ? l->cfg.max_players : 2;
}

int rnet_lobby__max_spectators(RNetLobby *l)
{
    return l ? l->cfg.max_spectators : 4;
}

static void member_rtt_clear(RNetLobby *l)
{
    int i;
    for (i = 0; i < RNET_LOBBY_MAX_MEMBERS; ++i)
        l->c.member_rtt_ms[i] = -1;
    l->c.rtt_next_ping_ms = 0;
    l->c.last_rtt_report = -1;
}

void rnet_lobby__lat_clear_members(RNetLobby *l)
{
    member_rtt_clear(l);
}

void rnet_lobby__ice_gate_rest(RNetLobby *l)
{
    l->c.ice_signal_accept =
        (l->cfg.waiting_room_rtt == RNET_LOBBY_RTT_PEER_PATH) ? 0 : 1;
}

/* Everything one connection owns is closed, then the struct is wiped. The
 * handle's configuration, identity and hooks live outside `c` and survive. */
static void conn_reset(RNetLobby *l)
{
    int i;
    RNetLobbyConn *c = &l->c;
    if (c->fd >= 0)
        rnet_lobby__socket_close(c->fd);
    for (i = 0; i < c->txq_n; ++i)
        free(c->txq[i]);
    rnet_lobby__mod_reset(l);
    rnet_lobby__lat_close_all(l);
    rnet_lobby__am_close(l);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->xfer_progress = -1;
    c->am.rtt_ms = -1;
    c->am.probe_socket = -1;
    member_rtt_clear(l);
    rnet_lobby__ice_gate_rest(l);
    rnet_lobby_match_caps_init(l, &c->match_caps);
}

int rnet_lobby_open(RNetLobby **out, const RNetLobbyConfig *cfg)
{
    RNetLobby *l;
    RNetLobbyConfig def;
    if (!out)
        return -1;
    *out = NULL;
    if (!cfg) {
        rnet_lobby_config_init(&def);
        cfg = &def;
    }
    l = (RNetLobby *)calloc(1, sizeof(*l));
    if (!l)
        return -2;
    l->cfg = *cfg;
    copy_str(l->cfg_platform, sizeof(l->cfg_platform), cfg->platform);
    copy_str(l->cfg_prefix, sizeof(l->cfg_prefix),
             (cfg->log_prefix && cfg->log_prefix[0]) ? cfg->log_prefix
                                                     : "rnet_lobby");
    copy_str(l->cfg_default_url, sizeof(l->cfg_default_url),
             (cfg->default_url && cfg->default_url[0]) ? cfg->default_url
                                                       : RNET_LOBBY_DEFAULT_URL);
    copy_str(l->cfg_url_env, sizeof(l->cfg_url_env), cfg->url_env_var);
    copy_str(l->cfg_version_env, sizeof(l->cfg_version_env),
             cfg->version_env_var);
    copy_str(l->cfg_build_id, sizeof(l->cfg_build_id), cfg->build_id);
    /* The copies own the text now; the config's pointers are the caller's. */
    l->cfg.platform = l->cfg_platform;
    l->cfg.log_prefix = l->cfg_prefix;
    l->cfg.default_url = l->cfg_default_url;
    l->cfg.url_env_var = l->cfg_url_env[0] ? l->cfg_url_env : NULL;
    l->cfg.version_env_var = l->cfg_version_env[0] ? l->cfg_version_env : NULL;
    l->cfg.build_id = l->cfg_build_id;
    l->cfg.game_name = NULL;
    l->cfg.game_version = NULL;
    l->cfg.content_fp = NULL;

    l->cfg.max_players = clampi(cfg->max_players > 0 ? cfg->max_players : 2, 2,
                                RNET_LOBBY_MAX_PLAYERS);
    l->cfg.max_spectators = clampi(cfg->max_spectators, 0,
                                   RNET_LOBBY_MAX_SPECTATORS);
    if (l->cfg.spectator_slot_base <= l->cfg.max_players)
        l->cfg.spectator_slot_base = RNET_LOBBY_DEFAULT_SPECTATOR_SLOT_BASE;
    if (l->cfg.host_port <= 0 || l->cfg.host_port > 65535)
        l->cfg.host_port = 7777;
    if (l->cfg.guest_port <= 0 || l->cfg.guest_port > 65535)
        l->cfg.guest_port = 7778;
    if (l->cfg.connect_timeout_ms <= 0)
        l->cfg.connect_timeout_ms = 3000;
    if (l->cfg.handshake_timeout_ms <= 0)
        l->cfg.handshake_timeout_ms = 10000;
    if (l->cfg.caps_input_delay_max < l->cfg.caps_input_delay_min)
        l->cfg.caps_input_delay_max = l->cfg.caps_input_delay_min;
    if (l->cfg.caps_input_prediction_max < l->cfg.caps_input_prediction_min)
        l->cfg.caps_input_prediction_max = l->cfg.caps_input_prediction_min;
    l->max_slots_pref = clampi(cfg->default_max_slots > 0 ? cfg->default_max_slots
                                                          : 2,
                               2, l->cfg.max_players);

    l->c.fd = -1;
    l->c.am.probe_socket = -1;
    conn_reset(l);
    rnet_lobby_set_game_identity(l, cfg->game_name, cfg->game_version);
    rnet_lobby_set_fp(l, cfg->content_fp);
    *out = l;
    return 0;
}

/* ── connect worker ──────────────────────────────────────────────────────── */

struct RNetLobbyConnectJob {
    char host[128];
    int  port;
    char path[128];
    char key[32];
    int  timeout_ms;
    int  fd;
    int  rc;
    char peer_ip[64];
    int  done;
    int  abandoned;
#if defined(_WIN32)
    CRITICAL_SECTION mu;
    HANDLE thread;
#else
    pthread_mutex_t mu;
    pthread_t thread;
#endif
};

static void job_lock(RNetLobbyConnectJob *j)
{
#if defined(_WIN32)
    EnterCriticalSection(&j->mu);
#else
    pthread_mutex_lock(&j->mu);
#endif
}

static void job_unlock(RNetLobbyConnectJob *j)
{
#if defined(_WIN32)
    LeaveCriticalSection(&j->mu);
#else
    pthread_mutex_unlock(&j->mu);
#endif
}

static int job_cancelled(RNetLobbyConnectJob *j)
{
    int a;
    job_lock(j);
    a = j->abandoned;
    job_unlock(j);
    return a;
}

static void job_free(RNetLobbyConnectJob *j)
{
#if defined(_WIN32)
    DeleteCriticalSection(&j->mu);
#else
    pthread_mutex_destroy(&j->mu);
#endif
    free(j);
}

static int wait_connected(int fd, int timeout_ms)
{
    fd_set wfds, efds;
    struct timeval tv;
    int soerr = 0;
#if defined(_WIN32)
    int len = (int)sizeof(soerr);
#else
    socklen_t len = (socklen_t)sizeof(soerr);
#endif
    int r;
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
#if defined(_WIN32)
    FD_SET((SOCKET)fd, &wfds);
    FD_SET((SOCKET)fd, &efds);
#else
    FD_SET(fd, &wfds);
    FD_SET(fd, &efds);
#endif
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (long)(timeout_ms % 1000) * 1000;
#if defined(_WIN32)
    r = select(0, NULL, &wfds, &efds, &tv);
#else
    r = select(fd + 1, NULL, &wfds, &efds, &tv);
#endif
    if (r <= 0)
        return -1;
#if defined(_WIN32)
    if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len) != 0 ||
        soerr != 0)
        return -1;
#else
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len) != 0 ||
        soerr != 0)
        return -1;
#endif
    return 0;
}

#if defined(_WIN32)
#define NTOP_LEN(cap) (cap)
#else
#define NTOP_LEN(cap) ((socklen_t)(cap))
#endif

static void peer_ip_of(int fd, char *out, size_t cap)
{
    struct sockaddr_storage ss;
#if defined(_WIN32)
    int slen = (int)sizeof(ss);
#else
    socklen_t slen = (socklen_t)sizeof(ss);
#endif
    out[0] = '\0';
    memset(&ss, 0, sizeof(ss));
#if defined(_WIN32)
    if (getpeername((SOCKET)fd, (struct sockaddr *)&ss, &slen) != 0)
        return;
#else
    if (getpeername(fd, (struct sockaddr *)&ss, &slen) != 0)
        return;
#endif
    if (ss.ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)&ss;
        if (!inet_ntop(AF_INET, (void *)&in->sin_addr, out, (socklen_t)cap))
            out[0] = '\0';
    }
#if defined(AF_INET6)
    else if (ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)&ss;
        if (!inet_ntop(AF_INET6, (void *)&in6->sin6_addr, out, (socklen_t)cap))
            out[0] = '\0';
    }
#endif
}

static void make_ws_key(char out[32])
{
    static const char *B64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char raw[16];
    int i, o = 0;
    if (rnet_os_random_bytes(raw, sizeof(raw)) != 0) {
        uint64_t t = rnet_lobby__now_ms() * 2654435761u;
        for (i = 0; i < 16; ++i) {
            t = t * 6364136223846793005ull + 1442695040888963407ull;
            raw[i] = (unsigned char)(t >> 33);
        }
    }
    for (i = 0; i < 16; i += 3) {
        unsigned v = (unsigned)raw[i] << 16;
        if (i + 1 < 16) v |= (unsigned)raw[i + 1] << 8;
        if (i + 2 < 16) v |= (unsigned)raw[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < 16) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < 16) ? B64[v & 63] : '=';
    }
    out[o] = '\0';
}

/* DNS + TCP + the HTTP upgrade request. Runs on the worker (or inline in
 * blocking mode). Touches nothing shared. */
static int lobby_dial(const char *host, int port, const char *path,
                      const char *key, int timeout_ms, RNetLobbyConnectJob *job,
                      int *fd_out, char *peer_ip, size_t peer_cap)
{
    struct addrinfo hints, *res = NULL, *rp;
    char portstr[16];
    char req[768];
    int fd = -1;
    int n;

    *fd_out = -1;
    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    /* IPv4: the relay-rewrite and LAN logic downstream reason in dotted
     * quads, as both engine copies did. */
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -2;
    for (rp = res; rp; rp = rp->ai_next) {
        int rc;
#if defined(_WIN32)
        SOCKET s;
#endif
        if (job && job_cancelled(job))
            break;
#if defined(_WIN32)
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        fd = (int)s;
#else
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
#endif
        rnet_lobby__set_nonblock(fd);
#if defined(_WIN32)
        rc = connect((SOCKET)fd, rp->ai_addr, (int)rp->ai_addrlen);
#else
        rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
#endif
        if (rc == 0)
            break;
        if (sock_in_progress() && wait_connected(fd, timeout_ms) == 0)
            break;
        rnet_lobby__socket_close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -3;
    set_block(fd);
    n = snprintf(req, sizeof(req),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s:%d\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Key: %s\r\n"
                 "Sec-WebSocket-Version: 13\r\n\r\n",
                 (path && path[0]) ? path : "/", host, port, key);
    if (n <= 0 || (size_t)n >= sizeof(req) || sock_send(fd, req, (size_t)n) != n) {
        rnet_lobby__socket_close(fd);
        return -4;
    }
    if (peer_ip && peer_cap)
        peer_ip_of(fd, peer_ip, peer_cap);
    *fd_out = fd;
    return 0;
}

#if defined(_WIN32)
static unsigned __stdcall connect_worker(void *arg)
#else
static void *connect_worker(void *arg)
#endif
{
    RNetLobbyConnectJob *j = (RNetLobbyConnectJob *)arg;
    int fd = -1;
    int rc = lobby_dial(j->host, j->port, j->path, j->key, j->timeout_ms,
                        j, &fd, j->peer_ip, sizeof(j->peer_ip));
    int abandoned;
    job_lock(j);
    abandoned = j->abandoned;
    if (!abandoned) {
        j->fd = fd;
        j->rc = rc;
        j->done = 1;
    }
    job_unlock(j);
    if (abandoned) {
        /* The owner gave up on us and detached: we clean up after ourselves. */
        if (fd >= 0)
            rnet_lobby__socket_close(fd);
        job_free(j);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void job_join(RNetLobbyConnectJob *j)
{
#if defined(_WIN32)
    WaitForSingleObject(j->thread, INFINITE);
    CloseHandle(j->thread);
#else
    pthread_join(j->thread, NULL);
#endif
}

static void job_detach(RNetLobbyConnectJob *j)
{
#if defined(_WIN32)
    CloseHandle(j->thread);
#else
    pthread_detach(j->thread);
#endif
}

/* Never blocks: a worker still in DNS is detached and frees itself. */
static void job_abandon(RNetLobby *l)
{
    RNetLobbyConnectJob *j = l->job;
    int done;
    if (!j)
        return;
    l->job = NULL;
    job_lock(j);
    done = j->done;
    if (!done) {
        /* Detach BEFORE publishing `abandoned`, under the lock: the worker
         * frees the job the moment it sees the flag, so nothing may touch
         * the job after the unlock. */
        job_detach(j);
        j->abandoned = 1;
    }
    job_unlock(j);
    if (done) {
        job_join(j);
        if (j->fd >= 0)
            rnet_lobby__socket_close(j->fd);
        job_free(j);
    }
}

static void adopt_socket(RNetLobby *l, int fd, const char *peer_ip)
{
    RNetLobbyConn *c = &l->c;
    c->fd = fd;
    rnet_lobby__set_nonblock(fd);
    copy_str(c->peer_ip, sizeof(c->peer_ip), peer_ip);
    if (c->peer_ip[0] && strcmp(c->peer_ip, c->host) != 0)
        LOBBY_INFO(l, "WS connected peer %s (url host %s)", c->peer_ip, c->host);
    c->connected = 1;
    c->handshake_done = 0;
    c->rx_http_len = 0;
    /* A server that accepts TCP and never answers the upgrade would
     * otherwise hold the client in "connected, not ready" forever. */
    c->handshake_deadline_ms =
        rnet_lobby__now_ms() + (uint64_t)l->cfg.handshake_timeout_ms;
}

static void job_finish(RNetLobby *l)
{
    RNetLobbyConnectJob *j = l->job;
    int done, fd, rc;
    if (!j)
        return;
    job_lock(j);
    done = j->done;
    job_unlock(j);
    if (!done)
        return;
    job_join(j);
    l->job = NULL;
    fd = j->fd;
    rc = j->rc;
    if (rc == 0 && fd >= 0) {
        adopt_socket(l, fd, j->peer_ip);
    } else {
        LOBBY_WARN(l, "connect to %s:%d failed (%d)", j->host, j->port, rc);
        if (fd >= 0)
            rnet_lobby__socket_close(fd);
    }
    job_free(j);
}

/* ── URL ─────────────────────────────────────────────────────────────────── */

const char *rnet_lobby_default_url(RNetLobby *l)
{
    const char *e;
    if (!l)
        return RNET_LOBBY_DEFAULT_URL;
    e = l->cfg.url_env_var ? getenv(l->cfg.url_env_var) : NULL;
    copy_str(l->default_url_buf, sizeof(l->default_url_buf),
             (e && e[0]) ? e : l->cfg_default_url);
    return l->default_url_buf;
}

static int parse_ws_url(const char *url, char *host, size_t hcap, int *port,
                        char *path, size_t pcap)
{
    const char *p = url;
    const char *slash;
    char hostport[192];
    char *colon;
    size_t n;
    if (!url || !url[0])
        return -1;
    if (strncmp(p, "wss://", 6) == 0)
        return -1; /* no TLS */
    if (strncmp(p, "ws://", 5) == 0)
        p += 5;
    slash = strchr(p, '/');
    n = slash ? (size_t)(slash - p) : strlen(p);
    if (n == 0 || n >= sizeof(hostport))
        return -1;
    memcpy(hostport, p, n);
    hostport[n] = '\0';
    copy_str(path, pcap, slash ? slash : "/");
    colon = strrchr(hostport, ':');
    if (colon && strchr(hostport, ']') == NULL) {
        *colon = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535)
            return -1;
    } else {
        /* The lobby's conventional port, as both engines defaulted. */
        *port = 8765;
    }
    copy_str(host, hcap, hostport);
    return host[0] ? 0 : -1;
}

/* ── connection ──────────────────────────────────────────────────────────── */

int rnet_lobby_connected(RNetLobby *l)
{
    if (!l)
        return 0;
    if (l->test_tx)
        return l->c.connected;
    return l->c.connected && l->c.fd >= 0;
}

int rnet_lobby_connecting(RNetLobby *l)
{
    return l && l->job != NULL;
}

int rnet_lobby_ready(RNetLobby *l)
{
    return l && rnet_lobby_connected(l) && l->c.welcomed;
}

const char *rnet_lobby_url(RNetLobby *l)
{
    if (!l || !rnet_lobby_connected(l))
        return "";
    return l->c.url;
}

int rnet_lobby_connect(RNetLobby *l, const char *ws_url)
{
    const char *use;
    char host[128], path[128];
    int port = 0;
    if (!l)
        return -1;
    if (rnet_lobby_connected(l) || l->job)
        return 0;
    /* Name the pin we present: a version_mismatch is uncomparable without
     * both values in the log. */
    LOBBY_INFO(l, "this build presents game_version=\"%s\"%s%s%s",
               l->game_version, l->cfg_build_id[0] ? " (build " : "",
               l->cfg_build_id, l->cfg_build_id[0] ? ")" : "");
    conn_reset(l);
    rnet_os_startup();
    use = (ws_url && ws_url[0]) ? ws_url : rnet_lobby_default_url(l);
    if (parse_ws_url(use, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        LOBBY_WARN(l, "bad lobby URL \"%s\" (ws:// only)", use ? use : "");
        return -1;
    }
    copy_str(l->c.url, sizeof(l->c.url), use);
    copy_str(l->c.host, sizeof(l->c.host), host);
    copy_str(l->c.path, sizeof(l->c.path), path);
    l->c.port = port;
    make_ws_key(l->c.ws_key);

    if (l->cfg.blocking_connect) {
        int fd = -1;
        char peer[64];
        int rc = lobby_dial(host, port, path, l->c.ws_key,
                            l->cfg.connect_timeout_ms, NULL, &fd, peer,
                            sizeof(peer));
        if (rc != 0) {
            LOBBY_WARN(l, "connect to %s:%d failed (%d)", host, port, rc);
            return rc;
        }
        adopt_socket(l, fd, peer);
        return 0;
    }
    {
        RNetLobbyConnectJob *j =
            (RNetLobbyConnectJob *)calloc(1, sizeof(*j));
        if (!j)
            return -5;
        copy_str(j->host, sizeof(j->host), host);
        copy_str(j->path, sizeof(j->path), path);
        copy_str(j->key, sizeof(j->key), l->c.ws_key);
        j->port = port;
        j->timeout_ms = l->cfg.connect_timeout_ms;
        j->fd = -1;
        j->rc = -1;
#if defined(_WIN32)
        InitializeCriticalSection(&j->mu);
        {
            uintptr_t th = _beginthreadex(NULL, 0, connect_worker, j, 0, NULL);
            if (!th) {
                job_free(j);
                return -5;
            }
            j->thread = (HANDLE)th;
        }
#else
        pthread_mutex_init(&j->mu, NULL);
        if (pthread_create(&j->thread, NULL, connect_worker, j) != 0) {
            job_free(j);
            return -5;
        }
#endif
        l->job = j;
    }
    return 0;
}

static void send_close_frame(int fd)
{
    /* Masked close, no payload. Best effort. */
    unsigned char f[6];
    unsigned char mask[4];
    if (rnet_os_random_bytes(mask, sizeof(mask)) != 0)
        memset(mask, 0x5a, sizeof(mask));
    f[0] = 0x88;
    f[1] = 0x80;
    memcpy(f + 2, mask, 4);
    (void)sock_send(fd, f, sizeof(f));
}

void rnet_lobby_disconnect(RNetLobby *l)
{
    if (!l)
        return;
    job_abandon(l);
    if (l->c.connected && l->c.handshake_done && l->c.fd >= 0)
        send_close_frame(l->c.fd);
    conn_reset(l);
    l->test_tx = NULL;
    l->test_tx_user = NULL;
}

void rnet_lobby_close(RNetLobby **lobby)
{
    if (!lobby || !*lobby)
        return;
    rnet_lobby_disconnect(*lobby);
    free(*lobby);
    *lobby = NULL;
}

/* ── outbound ────────────────────────────────────────────────────────────── */

static int write_now(RNetLobby *l, const char *json)
{
    if (l->test_tx) {
        l->test_tx(l->test_tx_user, json);
        return 0;
    }
    if (rnet_ws_write_text(l->c.fd, json, 1) < 0) {
        /* Deferred: disconnecting here would wipe state under a dispatcher
         * that is still writing into it. The pump disconnects. */
        l->c.tx_failed = 1;
        return -1;
    }
    return 0;
}

static void flush_txq(RNetLobby *l)
{
    int i;
    RNetLobbyConn *c = &l->c;
    if (!c->handshake_done && !l->test_tx)
        return;
    for (i = 0; i < c->txq_n; ++i) {
        (void)write_now(l, c->txq[i]);
        free(c->txq[i]);
        c->txq[i] = NULL;
    }
    c->txq_n = 0;
}

int rnet_lobby__send(RNetLobby *l, const char *json)
{
    RNetLobbyConn *c;
    size_t n;
    char *copy;
    if (!l || !json || !rnet_lobby_connected(l))
        return -1;
    c = &l->c;
    if (c->handshake_done || l->test_tx) {
        flush_txq(l);
        return write_now(l, json);
    }
    /* Before the handshake: hold it, whole. The engine copies held eight
     * 2 KB slots and truncated anything longer -- a create with a mod plan
     * went out cut mid-token. */
    if (c->txq_n >= RNET_LOBBY_TX_QUEUE) {
        LOBBY_ERROR(l, "outbound queue full before the handshake; dropping a "
                       "frame");
        return -1;
    }
    n = strlen(json);
    copy = (char *)malloc(n + 1);
    if (!copy)
        return -1;
    memcpy(copy, json, n + 1);
    c->txq[c->txq_n++] = copy;
    return 0;
}

static void send_pong(RNetLobby *l, const uint8_t *payload, size_t len)
{
    unsigned char f[2 + 4 + 125];
    unsigned char mask[4];
    size_t i;
    if (l->test_tx || l->c.fd < 0)
        return;
    if (len > 125)
        len = 125;
    if (rnet_os_random_bytes(mask, sizeof(mask)) != 0)
        memset(mask, 0x3c, sizeof(mask));
    f[0] = 0x8A;
    f[1] = (unsigned char)(0x80 | len);
    memcpy(f + 2, mask, 4);
    for (i = 0; i < len; ++i)
        f[6 + i] = (unsigned char)(payload[i] ^ mask[i & 3]);
    if (sock_send(l->c.fd, f, 6 + len) < 0)
        l->c.tx_failed = 1;
}

/* ── inbound frames ──────────────────────────────────────────────────────── */

static void handle_server_json(RNetLobby *l, const char *json);

static void dispatch_text(RNetLobby *l, const uint8_t *p, size_t n)
{
    RNetLobbyConn *c = &l->c;
    if (n + 1 > sizeof(c->msg)) {
        LOBBY_WARN(l, "dropping a %u-byte message (over %u)", (unsigned)n,
                   (unsigned)sizeof(c->msg));
        return;
    }
    if ((const char *)p != c->msg)
        memmove(c->msg, p, n);
    c->msg[n] = '\0';
    handle_server_json(l, c->msg);
}

static void rx_drain(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    size_t off = 0;
    while (c->rx_len - off >= 2) {
        uint8_t *b = c->rx + off;
        size_t avail = c->rx_len - off;
        size_t hdr = 2;
        uint64_t plen = b[1] & 0x7f;
        int fin = (b[0] & 0x80) != 0;
        int opcode = b[0] & 0x0f;
        int masked = (b[1] & 0x80) != 0;
        uint8_t *pay;
        if (plen == 126) {
            if (avail < 4)
                break;
            plen = ((uint64_t)b[2] << 8) | b[3];
            hdr = 4;
        } else if (plen == 127) {
            int k;
            if (avail < 10)
                break;
            plen = 0;
            for (k = 0; k < 8; ++k)
                plen = (plen << 8) | b[2 + k];
            hdr = 10;
        }
        if (masked)
            hdr += 4;
        if (plen > (uint64_t)(sizeof(c->rx) - hdr)) {
            /* Could never fit the buffer: the stream is unrecoverable. */
            LOBBY_ERROR(l, "server frame of %llu bytes exceeds the %u-byte "
                           "receive buffer; disconnecting",
                        (unsigned long long)plen, (unsigned)sizeof(c->rx));
            c->tx_failed = 1;
            c->rx_len = 0;
            return;
        }
        if (avail < hdr + (size_t)plen)
            break;
        pay = b + hdr;
        if (masked) {
            /* A server must not mask; tolerate it rather than drop data. */
            const uint8_t *m = b + hdr - 4;
            size_t k;
            for (k = 0; k < (size_t)plen; ++k)
                pay[k] ^= m[k & 3];
        }
        switch (opcode) {
        case 0x1: /* text */
            if (fin) {
                c->msg_active = 0;
                dispatch_text(l, pay, (size_t)plen);
            } else {
                c->msg_active = 1;
                c->msg_overflow = 0;
                c->msg_len = 0;
                if ((size_t)plen + 1 > sizeof(c->msg)) {
                    c->msg_overflow = 1;
                } else {
                    memcpy(c->msg, pay, (size_t)plen);
                    c->msg_len = (size_t)plen;
                }
            }
            break;
        case 0x0: /* continuation */
            if (c->msg_active) {
                if (!c->msg_overflow &&
                    c->msg_len + (size_t)plen + 1 <= sizeof(c->msg)) {
                    memcpy(c->msg + c->msg_len, pay, (size_t)plen);
                    c->msg_len += (size_t)plen;
                } else {
                    c->msg_overflow = 1;
                }
                if (fin) {
                    c->msg_active = 0;
                    if (c->msg_overflow)
                        LOBBY_WARN(l, "dropping an oversized fragmented "
                                      "message");
                    else
                        dispatch_text(l, (const uint8_t *)c->msg, c->msg_len);
                }
            }
            break;
        case 0x8: /* close */
            LOBBY_INFO(l, "server closed the connection");
            c->tx_failed = 1;
            c->rx_len = 0;
            return;
        case 0x9: /* ping */
            send_pong(l, pay, (size_t)plen);
            break;
        default: /* binary, pong: nothing for us */
            break;
        }
        off += hdr + (size_t)plen;
        if (!c->connected)
            return;
    }
    if (off > 0) {
        memmove(c->rx, c->rx + off, c->rx_len - off);
        c->rx_len -= off;
    }
}

/* Case-insensitive header value from an HTTP response head. */
static int http_header(const char *head, const char *name, char *out,
                       size_t cap)
{
    size_t nlen = strlen(name);
    const char *p = strstr(head, "\r\n");
    out[0] = '\0';
    while (p && p[2]) {
        const char *line = p + 2;
        const char *eol = strstr(line, "\r\n");
        size_t k;
        int match = 1;
        if (!eol)
            break;
        for (k = 0; k < nlen; ++k) {
            if (line + k >= eol ||
                tolower((unsigned char)line[k]) != tolower((unsigned char)name[k])) {
                match = 0;
                break;
            }
        }
        if (match && line[nlen] == ':') {
            const char *v = line + nlen + 1;
            size_t len;
            while (v < eol && (*v == ' ' || *v == '\t'))
                ++v;
            len = (size_t)(eol - v);
            while (len > 0 && (v[len - 1] == ' ' || v[len - 1] == '\t'))
                --len;
            if (len + 1 > cap)
                len = cap - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return 1;
        }
        p = eol;
    }
    return 0;
}

static void pump_handshake(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    char buf[2048];
    char *hdr_end;
    int n;
    /* Checked before reading, so a server trickling header bytes without
     * ever finishing the head times out as surely as a silent one. */
    if (rnet_lobby__now_ms() > c->handshake_deadline_ms) {
        LOBBY_ERROR(l, "no WebSocket upgrade answer from %s:%d within %d ms",
                    c->host, c->port, l->cfg.handshake_timeout_ms);
        c->tx_failed = 1;
        return;
    }
    n = sock_recv(c->fd, buf, sizeof(buf));
    if (n < 0) {
        if (!sock_would_block())
            c->tx_failed = 1;
        return;
    }
    if (n == 0) {
        c->tx_failed = 1;
        return;
    }
    if (c->rx_http_len + (size_t)n >= sizeof(c->rx_http)) {
        LOBBY_ERROR(l, "oversized WebSocket upgrade response");
        c->tx_failed = 1;
        return;
    }
    memcpy(c->rx_http + c->rx_http_len, buf, (size_t)n);
    c->rx_http_len += (size_t)n;
    c->rx_http[c->rx_http_len] = '\0';
    hdr_end = strstr(c->rx_http, "\r\n\r\n");
    if (!hdr_end)
        return;
    {
        size_t head_len = (size_t)(hdr_end - c->rx_http) + 4;
        size_t leftover = c->rx_http_len - head_len;
        char accept[64], want[32];
        if (strncmp(c->rx_http, "HTTP/1.1 101", 12) != 0 &&
            strncmp(c->rx_http, "HTTP/1.0 101", 12) != 0) {
            char line[80];
            size_t k = 0;
            while (k + 1 < sizeof(line) && c->rx_http[k] &&
                   c->rx_http[k] != '\r')
                ++k;
            memcpy(line, c->rx_http, k);
            line[k] = '\0';
            LOBBY_ERROR(l, "WebSocket upgrade refused: %s", line);
            c->tx_failed = 1;
            return;
        }
        hdr_end[2] = '\0'; /* keep the last header's CRLF for the scanner */
        if (rnet_ws_accept_key(c->ws_key, want) == 0 &&
            (!http_header(c->rx_http, "Sec-WebSocket-Accept", accept,
                          sizeof(accept)) ||
             strcmp(accept, want) != 0)) {
            LOBBY_ERROR(l, "WebSocket upgrade answered with the wrong "
                           "Sec-WebSocket-Accept; not a lobby server");
            c->tx_failed = 1;
            return;
        }
        c->handshake_done = 1;
        c->rx_len = 0;
        if (leftover > 0 && leftover <= sizeof(c->rx)) {
            memcpy(c->rx, c->rx_http + head_len, leftover);
            c->rx_len = leftover;
        }
        c->rx_http_len = 0;
        flush_txq(l);
        rx_drain(l);
    }
}

void rnet_lobby_pump(RNetLobby *l)
{
    RNetLobbyConn *c;
    if (!l)
        return;
    c = &l->c;
    job_finish(l);
    /* Before the connected check: a transfer mid-handshake and an automatch
     * probe must still fail / time out cleanly when the WS drops. */
    rnet_lobby__mod_pump(l);
    rnet_lobby__am_poll(l);
    if (c->tx_failed) {
        rnet_lobby_disconnect(l);
        return;
    }
    if (!rnet_lobby_connected(l))
        return;
    if (!l->test_tx) {
        if (!c->handshake_done) {
            pump_handshake(l);
            if (c->tx_failed)
                rnet_lobby_disconnect(l);
            return;
        }
        flush_txq(l);
        rx_drain(l);
        for (;;) {
            size_t room = sizeof(c->rx) - c->rx_len;
            int n;
            if (c->tx_failed || room == 0)
                break;
            n = sock_recv(c->fd, c->rx + c->rx_len, room);
            if (n < 0) {
                if (!sock_would_block())
                    c->tx_failed = 1;
                break;
            }
            if (n == 0) {
                LOBBY_INFO(l, "lobby server hung up");
                c->tx_failed = 1;
                break;
            }
            c->rx_len += (size_t)n;
            rx_drain(l);
        }
        if (c->tx_failed) {
            rnet_lobby_disconnect(l);
            return;
        }
    }
    rnet_lobby__lat_tick(l);
    if (c->tx_failed)
        rnet_lobby_disconnect(l);
}

void rnet_lobby__test_attach(RNetLobby *l, const char *player_id,
                             RNetLobbyTestTxFn tx, void *user)
{
    if (!l)
        return;
    job_abandon(l);
    conn_reset(l);
    l->test_tx = tx;
    l->test_tx_user = user;
    l->c.connected = 1;
    l->c.handshake_done = 1;
    l->c.welcomed = 1;
    copy_str(l->c.url, sizeof(l->c.url), "ws://test.invalid:8765");
    copy_str(l->c.host, sizeof(l->c.host), "test.invalid");
    l->c.port = 8765;
    copy_str(l->c.player_id, sizeof(l->c.player_id), player_id);
}

/* ── identity ────────────────────────────────────────────────────────────── */

static void send_hello(RNetLobby *l)
{
    char name_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_NAME_LEN)];
    char game_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_NAME_LEN)];
    char sess_esc[4096];
    char msg[4096 + 512];
    const char *sess = l->cfg.session ? l->cfg.session(l->cfg.session_user)
                                      : rnet_account_session();
    rnet_json_escape(l->display_name, name_esc, sizeof(name_esc));
    rnet_json_escape(l->game_name, game_esc, sizeof(game_esc));
    /* The session is optional and omitted for a guest, which keeps a guest's
     * hello byte-identical to the pre-Discord one. */
    if (sess && sess[0] && strlen(sess) * 2 + 1 < sizeof(sess_esc)) {
        rnet_json_escape(sess, sess_esc, sizeof(sess_esc));
        snprintf(msg, sizeof(msg),
                 "{\"op\":\"hello\",\"display_name\":\"%s\",\"game_name\":\"%s\","
                 "\"session\":\"%s\"}",
                 name_esc, game_esc, sess_esc);
    } else {
        snprintf(msg, sizeof(msg),
                 "{\"op\":\"hello\",\"display_name\":\"%s\",\"game_name\":\"%s\"}",
                 name_esc, game_esc);
    }
    (void)rnet_lobby__send(l, msg);
}

void rnet_lobby_set_display_name(RNetLobby *l, const char *name)
{
    char prev[RNET_LOBBY_NAME_LEN];
    if (!l || !name)
        return;
    copy_str(prev, sizeof(prev), l->display_name);
    copy_str(l->display_name, sizeof(l->display_name), name);
    l->c.name_refused[0] = '\0';
    /* `hello` IS the rename protocol; without it the players-online list and
     * our seat keep the first name until the next reconnect. */
    if (rnet_lobby_ready(l) && strcmp(prev, l->display_name) != 0)
        send_hello(l);
}

const char *rnet_lobby_display_name(RNetLobby *l)
{
    return l ? l->display_name : "";
}

const char *rnet_lobby_accepted_name(RNetLobby *l)
{
    return l ? l->c.accepted_name : "";
}

int rnet_lobby_name_refused(RNetLobby *l, char *code, size_t code_cap)
{
    if (code && code_cap)
        code[0] = '\0';
    if (!l || !l->c.name_refused[0])
        return 0;
    if (code && code_cap)
        copy_str(code, code_cap, l->c.name_refused);
    return 1;
}

int rnet_lobby_session_invalid(RNetLobby *l)
{
    return l ? l->c.session_invalid : 0;
}

const char *rnet_lobby_player_id(RNetLobby *l)
{
    return l ? l->c.player_id : "";
}

int rnet_lobby_version_is_release(const char *v)
{
    /* The QUALIFIER decides, not a "dev" prefix alone: CMake stamps
     * "0.1.5+abc12345-dirty.1234abcd" for a Release-type build of a modified
     * tree, and filtering on that hid every other build from its own list. */
    if (!v || !v[0])
        return 0;
    if (strchr(v, '+'))
        return 0;
    if (strncmp(v, "dev", 3) == 0)
        return 0;
    return 1;
}

void rnet_lobby_set_game_identity(RNetLobby *l, const char *game_name,
                                  const char *game_version)
{
    const char *forced;
    if (!l)
        return;
    copy_str(l->game_name, sizeof(l->game_name), game_name);
    copy_str(l->real_version, sizeof(l->real_version),
             (game_version && game_version[0]) ? game_version : "dev");
    forced = l->cfg.version_env_var ? getenv(l->cfg.version_env_var) : NULL;
    if (forced && forced[0]) {
        copy_str(l->game_version, sizeof(l->game_version), forced);
        l->version_forced = 1;
        /* Every identity change, not once: the honest pin is what a later
         * desync report needs, and a startup-only line is never scrolled to. */
        LOBBY_WARN(l, "game_version FORCED to \"%s\" by %s (this build is really "
                      "\"%s\"). Testing override -- both peers must be the same "
                      "build.",
                   l->game_version, l->cfg.version_env_var, l->real_version);
        return;
    }
    l->version_forced = 0;
    copy_str(l->game_version, sizeof(l->game_version), l->real_version);
}

const char *rnet_lobby_game_name(RNetLobby *l)
{
    return l ? l->game_name : "";
}

const char *rnet_lobby_game_version(RNetLobby *l)
{
    return l ? l->game_version : "dev";
}

int rnet_lobby_version_filter_strict(RNetLobby *l)
{
    /* An override exists to pool two development machines, and the mistake it
     * invites is setting it on ONE -- strict filtering would then hide the
     * other's room. List everything and let the join explain. */
    if (!l || l->version_forced)
        return 0;
    return rnet_lobby_version_is_release(l->game_version);
}

void rnet_lobby_set_fp(RNetLobby *l, const char *hex)
{
    size_t i;
    if (!l)
        return;
    l->fp[0] = '\0';
    if (!hex || strlen(hex) != 64)
        return;
    for (i = 0; i < 64; ++i) {
        char ch = hex[i];
        if (ch >= 'A' && ch <= 'F')
            ch = (char)(ch - 'A' + 'a');
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            l->fp[0] = '\0';
            return;
        }
        l->fp[i] = ch;
    }
    l->fp[64] = '\0';
    LOBBY_INFO(l, "content fingerprint %.16s...", l->fp);
}

const char *rnet_lobby_fp(RNetLobby *l)
{
    return l ? l->fp : "";
}

/* ── list ────────────────────────────────────────────────────────────────── */

static void send_list_request(RNetLobby *l)
{
    char msg[512];
    char gn_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_NAME_LEN)];
    char gv_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_VERSION_LEN)];
    rnet_json_escape(l->game_name, gn_esc, sizeof(gn_esc));
    rnet_json_escape(l->game_version, gv_esc, sizeof(gv_esc));
    if (rnet_lobby_version_filter_strict(l) && l->game_name[0])
        snprintf(msg, sizeof(msg),
                 "{\"op\":\"list\",\"game_name\":\"%s\",\"game_version\":\"%s\"}",
                 gn_esc, gv_esc);
    else if (l->game_name[0])
        snprintf(msg, sizeof(msg), "{\"op\":\"list\",\"game_name\":\"%s\"}",
                 gn_esc);
    else
        snprintf(msg, sizeof(msg), "{\"op\":\"list\"}");
    (void)rnet_lobby__send(l, msg);
}

void rnet_lobby_request_list(RNetLobby *l)
{
    if (!l)
        return;
    l->c.list_rtt_on_next_list = 1;
    send_list_request(l);
}

int rnet_lobby_list_count(RNetLobby *l)
{
    return l ? l->c.list_count : 0;
}

int rnet_lobby_list_get(RNetLobby *l, int index, RNetLobbyRow *out)
{
    if (!l || !out || index < 0 || index >= l->c.list_count)
        return 0;
    *out = l->c.list[index];
    return 1;
}

int rnet_lobby_online_count(RNetLobby *l)
{
    return l ? l->c.online_count : 0;
}

int rnet_lobby_online_get(RNetLobby *l, int index, RNetLobbyOnlinePlayer *out)
{
    if (!l || !out || index < 0 || index >= l->c.online_count)
        return 0;
    *out = l->c.online[index];
    return 1;
}

static void parse_players(RNetLobby *l, RNetJsonSpan msg)
{
    RNetJsonSpan arr, row;
    int n = 0;
    l->c.online_count = 0;
    if (!rnet_json_arr(msg, "players", &arr))
        return; /* an older server has none */
    while (n < RNET_LOBBY_MAX_ONLINE && rnet_json_arr_next(&arr, &row)) {
        RNetLobbyOnlinePlayer *p = &l->c.online[n];
        if (rnet_json_kind(row) != '{')
            continue;
        memset(p, 0, sizeof(*p));
        rnet_json_str(row, "display_name", p->display_name, sizeof(p->display_name));
        rnet_json_str(row, "country", p->country, sizeof(p->country));
        rnet_json_str(row, "lobby_id", p->lobby_id, sizeof(p->lobby_id));
        rnet_json_str(row, "lobby_name", p->lobby_name, sizeof(p->lobby_name));
        p->hosting = rnet_json_bool(row, "hosting", 0);
        rnet_json_str(row, "tag", p->tag, sizeof(p->tag));
        rnet_json_str(row, "account", p->account, sizeof(p->account));
        rnet_json_str(row, "game_name", p->game_name, sizeof(p->game_name));
        /* Players of another title are not "online" for this one; a row with
         * no title yet (has not listed) is kept. */
        if (l->game_name[0] && p->game_name[0] &&
            strcmp(p->game_name, l->game_name) != 0)
            continue;
        if (p->display_name[0])
            ++n;
    }
    l->c.online_count = n;
}

static void row_lan_fingerprint(const RNetLobbyRow *row, char *out, size_t cap)
{
    size_t o = 0;
    int i;
    out[0] = '\0';
    for (i = 0; i < row->lan_count; ++i) {
        int w;
        if (!row->lan_endpoints[i][0])
            continue;
        w = snprintf(out + o, cap - o, "%s%s", o ? "|" : "",
                     row->lan_endpoints[i]);
        if (w < 0 || (size_t)w >= cap - o)
            break;
        o += (size_t)w;
    }
}

static void parse_lobby_list(RNetLobby *l, RNetJsonSpan msg)
{
    RNetLobbyConn *c = &l->c;
    RNetJsonSpan arr, obj;
    int n = 0, i;
    int want_probe = c->list_rtt_on_next_list;
    struct {
        char id[RNET_LOBBY_ID_LEN];
        char ep[RNET_LOBBY_ENDPOINT_LEN];
        char lan[RNET_LOBBY_MAX_LAN_EPS * RNET_LOBBY_ENDPOINT_LEN];
        int  ms;
    } prev[RNET_LOBBY_MAX_LIST];
    int prev_n = c->list_count;

    c->list_rtt_on_next_list = 0;
    parse_players(l, msg);
    /* Keep measured RTTs across the server's periodic pushes; a row whose
     * endpoints changed is re-measured. */
    for (i = 0; i < prev_n; ++i) {
        copy_str(prev[i].id, sizeof(prev[i].id), c->list[i].lobby_id);
        copy_str(prev[i].ep, sizeof(prev[i].ep), c->list[i].host_endpoint);
        row_lan_fingerprint(&c->list[i], prev[i].lan, sizeof(prev[i].lan));
        prev[i].ms = c->list[i].latency_ms;
    }
    c->list_count = 0;
    if (!rnet_json_arr(msg, "lobbies", &arr))
        return;
    while (n < RNET_LOBBY_MAX_LIST && rnet_json_arr_next(&arr, &obj)) {
        RNetLobbyRow *r = &c->list[n];
        RNetJsonSpan lan, ep;
        char lan_fp[RNET_LOBBY_MAX_LAN_EPS * RNET_LOBBY_ENDPOINT_LEN];
        int has_version;
        if (rnet_json_kind(obj) != '{')
            continue;
        memset(r, 0, sizeof(*r));
        r->latency_ms = -1;
        rnet_json_str(obj, "lobby_id", r->lobby_id, sizeof(r->lobby_id));
        rnet_json_str(obj, "name", r->name, sizeof(r->name));
        rnet_json_str(obj, "game_name", r->game_name, sizeof(r->game_name));
        rnet_json_str(obj, "game_version", r->game_version, sizeof(r->game_version));
        /* The broadcast list is unfiltered: drop other titles. */
        if (l->game_name[0] && strcmp(r->game_name, l->game_name) != 0)
            continue;
        /* A row with NO version (an old server) is not filtered by the pin:
         * rewriting it to "dev" first, as the snesrecomp copy did, hid those
         * rooms from every release client. */
        has_version = r->game_version[0] != '\0';
        if (has_version && rnet_lobby_version_filter_strict(l) &&
            strcmp(r->game_version, l->game_version) != 0)
            continue;
        if (!has_version)
            copy_str(r->game_version, sizeof(r->game_version), "dev");
        r->player_count = rnet_json_int(obj, "player_count", 0);
        r->max_slots = rnet_json_int(obj, "max_slots", 2);
        r->has_password = rnet_json_bool(obj, "has_password", 0);
        rnet_json_str(obj, "host_country", r->host_country, sizeof(r->host_country));
        r->allow_spectators = rnet_json_bool(obj, "allow_spectators", 0);
        r->max_spectators = rnet_json_int(obj, "max_spectators", 0);
        r->spectator_count = rnet_json_int(obj, "spectator_count", 0);
        r->lobby_kind = rnet_json_int(obj, "lobby_kind", 0);
        rnet_json_str(obj, "host_endpoint", r->host_endpoint, sizeof(r->host_endpoint));
        if (rnet_json_arr(obj, "lan_endpoints", &lan)) {
            while (r->lan_count < RNET_LOBBY_MAX_LAN_EPS &&
                   rnet_json_arr_next(&lan, &ep)) {
                if (rnet_json_unescape(ep, r->lan_endpoints[r->lan_count],
                                       RNET_LOBBY_ENDPOINT_LEN) == 1 &&
                    r->lan_endpoints[r->lan_count][0])
                    r->lan_count++;
            }
        }
        row_lan_fingerprint(r, lan_fp, sizeof(lan_fp));
        if (!want_probe) {
            for (i = 0; i < prev_n; ++i) {
                if (prev[i].id[0] && strcmp(prev[i].id, r->lobby_id) == 0 &&
                    strcmp(prev[i].ep, r->host_endpoint) == 0 &&
                    strcmp(prev[i].lan, lan_fp) == 0) {
                    r->latency_ms = prev[i].ms;
                    break;
                }
            }
        }
        ++n;
    }
    c->list_count = n;
    rnet_lobby__lat_on_list(l, want_probe);
}

/* ── match caps ──────────────────────────────────────────────────────────── */

void rnet_lobby_match_caps_init(RNetLobby *l, RNetLobbyMatchCaps *caps)
{
    if (!caps)
        return;
    memset(caps, 0, sizeof(*caps));
    caps->input_delay = l ? l->cfg.caps_input_delay_default : 6;
    caps->input_prediction = 0;
    caps->rollback = l ? l->cfg.caps_rollback_default : 1;
}

/* Keys the library owns; every other member is the title's. */
static int caps_key_is_library(RNetJsonSpan k)
{
    static const char *const keys[] = {
        "v", "input_delay", "input_prediction", "rollback", "force_turn",
        "force_input_relay", "mod_plan", "mod_set", "mod_cosmetic_allow"
    };
    size_t i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        size_t n = strlen(keys[i]);
        if (k.n == n && memcmp(k.p, keys[i], n) == 0)
            return 1;
    }
    return 0;
}

int rnet_lobby_match_caps_decode(RNetLobby *l, const char *json,
                                 RNetLobbyMatchCaps *out)
{
    RNetJsonSpan obj = rnet_json_span(json);
    RNetJsonSpan it, k, v, whole;
    size_t o = 0;
    int dmin = l ? l->cfg.caps_input_delay_min : 0;
    int dmax = l ? l->cfg.caps_input_delay_max : 20;
    int pmin = l ? l->cfg.caps_input_prediction_min : 2;
    int pmax = l ? l->cfg.caps_input_prediction_max : 16;
    RNetJsonSpan pv;
    if (!out)
        return 0;
    if (rnet_json_kind(obj) != '{')
        return 0;
    rnet_lobby_match_caps_init(l, out);
    out->input_delay = clampi(rnet_json_int(obj, "input_delay",
                                            l ? l->cfg.caps_input_delay_default : 6),
                              dmin, dmax);
    if (rnet_json_find(obj, "input_prediction", &pv))
        out->input_prediction = clampi(rnet_json_int(obj, "input_prediction", pmin),
                                       pmin, pmax);
    else
        out->input_prediction = l ? l->cfg.caps_input_prediction_default : 10;
    out->rollback = rnet_json_bool(obj, "rollback",
                                   l ? l->cfg.caps_rollback_default : 1) ? 1 : 0;
    out->force_turn = rnet_json_bool(obj, "force_turn", 0) ? 1 : 0;
    out->force_input_relay = rnet_json_bool(obj, "force_input_relay", 0) ? 1 : 0;
    /* Absent = no mods required. A STRING plan (the superseded encoding)
     * parses to nothing on purpose: the server already ignores it. */
    out->mod_count = rnet_lobby__parse_mod_pkgs(obj, "mod_plan", out->mods,
                                                RNET_LOBBY_MAX_MODS);
    rnet_json_str(obj, "mod_set", out->mod_set, sizeof(out->mod_set));
    /* Absent -> empty -> nothing exempt: silence is not permission. */
    rnet_json_str(obj, "mod_cosmetic_allow", out->mod_cosmetic_allow,
                  sizeof(out->mod_cosmetic_allow));
    it = obj;
    while (rnet_json_members_next(&it, &k, &v, &whole)) {
        if (caps_key_is_library(k))
            continue;
        if (o + (o ? 1 : 0) + whole.n + 1 > sizeof(out->game_json)) {
            if (l)
                LOBBY_WARN(l, "match_caps title members exceed %u bytes; "
                              "the rest were dropped",
                           (unsigned)sizeof(out->game_json));
            break;
        }
        if (o)
            out->game_json[o++] = ',';
        memcpy(out->game_json + o, whole.p, whole.n);
        o += whole.n;
    }
    out->game_json[o] = '\0';
    if (!rnet_json_copy(obj, out->json, sizeof(out->json)))
        out->json[0] = '\0';
    out->valid = 1;
    return 1;
}

size_t rnet_lobby_match_caps_encode(RNetLobby *l, const RNetLobbyMatchCaps *caps,
                                    char *dst, size_t cap)
{
    char mods[RNET_LOBBY_MAX_MODS * RNET_LOBBY_MOD_ROW_JSON + 32];
    char set_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_MOD_SET_LEN)];
    char allow_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_MOD_SET_LEN)];
    char pred[32];
    int n;
    (void)l;
    if (!dst || cap < 8 || !caps)
        return 0;
    dst[0] = '\0';
    /* The plan first: a caps blob with a SHORT plan seats a peer that cannot
     * play, so a plan that does not fit publishes nothing. */
    if (!rnet_lobby__append_mod_pkgs(mods, sizeof(mods), "mod_plan", caps->mods,
                                     caps->mod_count))
        return 0;
    rnet_json_escape(caps->mod_set, set_esc, sizeof(set_esc));
    rnet_json_escape(caps->mod_cosmetic_allow, allow_esc, sizeof(allow_esc));
    pred[0] = '\0';
    if (caps->input_prediction > 0)
        snprintf(pred, sizeof(pred), ",\"input_prediction\":%d",
                 caps->input_prediction);
    n = snprintf(dst, cap,
                 "{\"v\":1,\"input_delay\":%d%s,\"rollback\":%s,"
                 "\"force_turn\":%s,\"force_input_relay\":%s,%s,"
                 "\"mod_set\":\"%s\",\"mod_cosmetic_allow\":\"%s\"%s%s}",
                 caps->input_delay, pred, caps->rollback ? "true" : "false",
                 caps->force_turn ? "true" : "false",
                 caps->force_input_relay ? "true" : "false", mods, set_esc,
                 allow_esc, caps->game_json[0] ? "," : "", caps->game_json);
    if (n < 0 || (size_t)n >= cap) {
        dst[0] = '\0';
        return 0;
    }
    /* The server drops a match_caps object over 4096 bytes ENTIRELY, taking
     * delay and rollback down with the plan. Refuse here, where it can be
     * said out loud. */
    if (n > 4000) {
        if (l)
            LOBBY_ERROR(l, "match caps are %d bytes with %d mod(s); the lobby "
                           "server discards anything over 4096, so nothing was "
                           "published -- reduce the mod set or the title's "
                           "members", n, caps->mod_count);
        dst[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

/* `,"match_caps":{...}` or "" -- and "" is logged, never a fragment. The
 * engine copies pasted a truncated snprintf result into the frame, which the
 * server dropped whole: "Create Lobby" closed its modal and created nothing. */
static int caps_member(RNetLobby *l, const RNetLobbyMatchCaps *caps,
                       char *dst, size_t cap, const char *op)
{
    size_t n;
    dst[0] = '\0';
    if (!caps || !caps->valid || cap < 32)
        return 0;
    memcpy(dst, ",\"match_caps\":", 14);
    n = rnet_lobby_match_caps_encode(l, caps, dst + 14, cap - 14);
    if (n == 0) {
        dst[0] = '\0';
        LOBBY_WARN(l, "match caps would not serialise for `%s` -- sending it "
                      "without them, and keeping the previous caps locally so "
                      "this host runs what the room was told", op);
        return 0;
    }
    return 1;
}

static void adopt_caps(RNetLobby *l, const RNetLobbyMatchCaps *caps)
{
    l->c.match_caps = *caps;
    /* Keep .json describing what was actually published. */
    if (!rnet_lobby_match_caps_encode(l, caps, l->c.match_caps.json,
                                      sizeof(l->c.match_caps.json)))
        l->c.match_caps.json[0] = '\0';
}

static void ingest_caps(RNetLobby *l, RNetJsonSpan msg)
{
    RNetJsonSpan obj;
    char buf[RNET_LOBBY_CAPS_JSON_LEN * 2];
    if (!rnet_json_obj(msg, "match_caps", &obj))
        return;
    if (!rnet_json_copy(obj, buf, sizeof(buf))) {
        LOBBY_WARN(l, "match_caps object too large (%u bytes); ignored",
                   (unsigned)obj.n);
        return;
    }
    rnet_lobby_match_caps_decode(l, buf, &l->c.match_caps);
}

const RNetLobbyMatchCaps *rnet_lobby_match_caps(RNetLobby *l)
{
    static RNetLobbyMatchCaps none;
    return l ? &l->c.match_caps : &none;
}

int rnet_lobby_set_match_caps(RNetLobby *l, const RNetLobbyMatchCaps *caps)
{
    char msg[RNET_LOBBY_CAPS_JSON_LEN + 128];
    char cj[RNET_LOBBY_CAPS_JSON_LEN + 32];
    if (!l || !rnet_lobby_connected(l) || !l->c.in_lobby || !l->c.is_host ||
        !caps || !caps->valid)
        return -1;
    if (!caps_member(l, caps, cj, sizeof(cj), "set_match_caps"))
        return -1;
    adopt_caps(l, caps);
    snprintf(msg, sizeof(msg), "{\"op\":\"set_match_caps\"%s}", cj);
    return rnet_lobby__send(l, msg);
}

/* ── seats and members ───────────────────────────────────────────────────── */

int rnet_lobby__member_index_for_player(RNetLobby *l, const char *pid)
{
    int i;
    if (!pid || !pid[0])
        return -1;
    for (i = 0; i < l->c.member_count; ++i)
        if (strcmp(l->c.members[i].player_id, pid) == 0)
            return i;
    return -1;
}

int rnet_lobby__member_slot_for_player(RNetLobby *l, const char *pid)
{
    int i = rnet_lobby__member_index_for_player(l, pid);
    return i >= 0 ? l->c.members[i].slot : -1;
}

int rnet_lobby__member_is_spectator(RNetLobby *l, const char *pid)
{
    int i = rnet_lobby__member_index_for_player(l, pid);
    return i >= 0 ? (l->c.members[i].is_spectator ? 1 : 0) : 0;
}

int rnet_lobby_spectator_slot_base(RNetLobby *l)
{
    if (!l)
        return RNET_LOBBY_DEFAULT_SPECTATOR_SLOT_BASE;
    /* The namespace is the server's; the configured base is the fallback for
     * an update that arrives without one. */
    return l->c.join.spectator_slot_base > 0 ? l->c.join.spectator_slot_base
                                             : l->cfg.spectator_slot_base;
}

/* member_rtt_ms has a cell per seat in BOTH tables, but a gallery seat is
 * numbered from the server's base; without this every spectator's report was
 * dropped as out of range. */
int rnet_lobby__rtt_index_for_slot(RNetLobby *l, int slot)
{
    int base;
    if (slot < 0)
        return -1;
    if (slot < RNET_LOBBY_MAX_PLAYERS)
        return slot;
    base = rnet_lobby_spectator_slot_base(l);
    if (base > 0 && slot >= base && slot < base + RNET_LOBBY_MAX_SPECTATORS)
        return RNET_LOBBY_MAX_PLAYERS + (slot - base);
    return -1;
}

int rnet_lobby_seat_valid(RNetLobby *l, int slot)
{
    int base;
    if (!l || slot < 0)
        return 0;
    if (slot < l->cfg.max_players)
        return 1;
    base = rnet_lobby_spectator_slot_base(l);
    if (base <= 0)
        return 0;
    return slot >= base && slot < base + l->cfg.max_spectators;
}

int rnet_lobby_spectator_slot(RNetLobby *l, int index)
{
    if (!l || index < 0 || index >= l->cfg.max_spectators)
        return -1;
    return rnet_lobby_spectator_slot_base(l) + index;
}

int rnet_lobby_local_wire_slot(RNetLobby *l)
{
    int gi;
    if (!l || !l->c.join.local_is_spectator)
        return -1;
    if (l->c.join.spectator_relay_base <= 0)
        return -1;
    gi = l->c.join.local_slot - rnet_lobby_spectator_slot_base(l);
    if (gi < 0 || gi >= RNET_LOBBY_MAX_SPECTATORS)
        return -1;
    return l->c.join.spectator_relay_base + gi;
}

/* One seat array into the membership table, appending from `n`. Players and
 * spectators land in one table tagged by role. An absent key is a server that
 * predates the gallery, and reads as an empty one. */
static int parse_seat_array(RNetLobby *l, RNetJsonSpan msg, const char *key,
                            int is_spectator, int n, int *saw_self)
{
    RNetLobbyConn *c = &l->c;
    RNetJsonSpan arr, row;
    if (!rnet_json_arr(msg, key, &arr))
        return n;
    while (n < RNET_LOBBY_MAX_MEMBERS && rnet_json_arr_next(&arr, &row)) {
        RNetLobbyMember *m = &c->members[n];
        RNetJsonSpan offer;
        if (rnet_json_kind(row) != '{')
            continue;
        memset(m, 0, sizeof(*m));
        m->slot = rnet_json_int(row, "slot", n);
        rnet_json_str(row, "player_id", m->player_id, sizeof(m->player_id));
        rnet_json_str(row, "display_name", m->display_name, sizeof(m->display_name));
        m->ready = rnet_json_bool(row, "ready", 0);
        m->is_spectator = is_spectator;
        rnet_json_str(row, "country", m->country, sizeof(m->country));
        rnet_json_str(row, "account", m->account, sizeof(m->account));
        if (!rnet_json_copy(row, c->member_json[n], sizeof(c->member_json[n])))
            c->member_json[n][0] = '\0';
        /* mod_offer is an OBJECT {"pkgs":[...]} -- the server requires one and
         * echoes it verbatim. Read as a bare array it returned zero rows and
         * every peer looked empty-handed. */
        c->member_offer_count[n] = 0;
        if (rnet_json_obj(row, "mod_offer", &offer))
            c->member_offer_count[n] = rnet_lobby__parse_mod_pkgs(
                offer, "pkgs", c->member_offer[n], RNET_LOBBY_MAX_MODS);
        m->mod_offer_count = c->member_offer_count[n];
        if (c->player_id[0] && strcmp(m->player_id, c->player_id) == 0) {
            c->local_ready = m->ready;
            /* Seat swaps arrive only in updates: keep local_slot and the ROLE
             * in step with the same update that moved the seat. */
            c->join.local_slot = m->slot;
            c->join.local_is_spectator = is_spectator;
            if (saw_self)
                *saw_self = 1;
        }
        ++n;
    }
    return n;
}

void rnet_lobby__parse_slots(RNetLobby *l, RNetJsonSpan msg)
{
    RNetLobbyConn *c = &l->c;
    int n, self = 0;
    c->member_count = 0;
    c->local_ready = 0;
    c->join.local_is_spectator = 0;
    /* Default to what we already knew: `launch` carries no allow_spectators,
     * and zeroing it there would erase the gallery at the exact moment the
     * client decides whether it is in it. */
    c->join.allow_spectators =
        rnet_json_bool(msg, "allow_spectators", c->join.allow_spectators);
    c->join.max_spectators =
        rnet_json_int(msg, "max_spectators", c->join.max_spectators);
    c->join.spectator_count =
        rnet_json_int(msg, "spectator_count", c->join.spectator_count);
    c->join.spectator_relay_base =
        rnet_json_int(msg, "spectator_relay_base", c->join.spectator_relay_base);
    c->join.host_spectates = rnet_json_bool(msg, "host_spectates", 0);
    c->join.spectator_slot_base = rnet_json_int(
        msg, "spectator_slot_base",
        c->join.spectator_slot_base > 0 ? c->join.spectator_slot_base
                                        : l->cfg.spectator_slot_base);
    n = parse_seat_array(l, msg, "slots", 0, 0, &self);
    n = parse_seat_array(l, msg, "spectators", 1, n, &self);
    c->member_count = n;
}

int rnet_lobby_member_count(RNetLobby *l)
{
    return l ? l->c.member_count : 0;
}

int rnet_lobby_member_get(RNetLobby *l, int index, RNetLobbyMember *out)
{
    if (!l || !out || index < 0 || index >= l->c.member_count)
        return 0;
    *out = l->c.members[index];
    return 1;
}

const char *rnet_lobby_member_json(RNetLobby *l, int index)
{
    if (!l || index < 0 || index >= l->c.member_count)
        return NULL;
    return l->c.member_json[index];
}

int rnet_lobby_member_latency_ms(RNetLobby *l, int slot)
{
    int idx, i;
    if (!l)
        return -1;
    idx = rnet_lobby__rtt_index_for_slot(l, slot);
    if (idx < 0 || idx >= RNET_LOBBY_MAX_MEMBERS)
        return -1;
    if (l->cfg.waiting_room_rtt == RNET_LOBBY_RTT_WS_SIGNAL) {
        /* Each guest measures itself -> host and reports; the host's row has
         * no measurement of its own. */
        if (l->c.host_player_id[0]) {
            for (i = 0; i < l->c.member_count; ++i)
                if (l->c.members[i].slot == slot &&
                    strcmp(l->c.members[i].player_id, l->c.host_player_id) == 0)
                    return -1;
        }
    } else {
        /* Measurements are stored under the REMOTE seat; never self. */
        int local = rnet_lobby__member_slot_for_player(l, l->c.player_id);
        if (local >= 0 && slot == local)
            return -1;
    }
    return l->c.member_rtt_ms[idx];
}

int rnet_lobby_member_is_host(RNetLobby *l, const RNetLobbyMember *m)
{
    if (!l || !m || !m->player_id[0] || !l->c.host_player_id[0])
        return 0;
    return strcmp(m->player_id, l->c.host_player_id) == 0;
}

int rnet_lobby_local_ready(RNetLobby *l)
{
    return l ? l->c.local_ready : 0;
}

int rnet_lobby_all_ready(RNetLobby *l)
{
    return l && l->c.all_ready && l->c.in_lobby && l->c.join.player_count >= 2;
}

/* Every set_ready carries the installed set (the host's launch gate reads
 * it, and it changes while seated) and the title's ready extras. */
void rnet_lobby__send_set_ready(RNetLobby *l, int ready)
{
    char offer[RNET_LOBBY_MAX_MODS * RNET_LOBBY_MOD_ROW_JSON + 64];
    char msg[sizeof(offer) + RNET_LOBBY_READY_EXTRA_LEN + 96];
    int n;
    offer[0] = '\0';
    if (l->offer_fn && !rnet_lobby__append_mod_offer(l, offer, sizeof(offer))) {
        /* Ready without it rather than not at all: the host then sees "claims
         * nothing" and holds the match -- the safe direction. */
        offer[0] = '\0';
        LOBBY_WARN(l, "could not announce the installed mod set (too large); "
                      "the host will see this peer as having none");
    }
    n = snprintf(msg, sizeof(msg), "{\"op\":\"set_ready\",\"ready\":%s%s%s%s}",
                 ready ? "true" : "false", offer, l->ready_extra[0] ? "," : "",
                 l->ready_extra);
    if (n < 0 || (size_t)n >= sizeof(msg))
        snprintf(msg, sizeof(msg), "{\"op\":\"set_ready\",\"ready\":%s}",
                 ready ? "true" : "false");
    (void)rnet_lobby__send(l, msg);
}

int rnet_lobby_set_ready(RNetLobby *l, int ready)
{
    if (!l || !rnet_lobby_connected(l) || !l->c.in_lobby)
        return -1;
    rnet_lobby__send_set_ready(l, ready);
    return 0;
}

int rnet_lobby_set_ready_extra_json(RNetLobby *l, const char *members)
{
    if (!l)
        return -1;
    if (!members) {
        l->ready_extra[0] = '\0';
        return 0;
    }
    if (strlen(members) + 1 > sizeof(l->ready_extra))
        return -1;
    copy_str(l->ready_extra, sizeof(l->ready_extra), members);
    return 0;
}

/* ── ICE signal queue ────────────────────────────────────────────────────── */

void rnet_lobby__enqueue_signal(RNetLobby *l, int type, int flag,
                                const char *text)
{
    RNetLobbyConn *c = &l->c;
    int i;
    if (!c->ice_signal_accept)
        return;
    if (c->sig_count >= RNET_LOBBY_SIG_QUEUE) {
        c->sig_head = (c->sig_head + 1) % RNET_LOBBY_SIG_QUEUE; /* drop oldest */
        c->sig_count--;
    }
    i = c->sig_tail;
    c->sig_q[i].type = type;
    c->sig_q[i].flag = flag;
    copy_str(c->sig_q[i].text, sizeof(c->sig_q[i].text), text);
    c->sig_tail = (c->sig_tail + 1) % RNET_LOBBY_SIG_QUEUE;
    c->sig_count++;
}

/* The session owns ONE ICE agent. The relay forwards a broadcast signal to
 * everyone, and a spectator's agent offers like a player's; a third party's
 * SDP reads to a connected agent as a peer ICE restart and destroys the live
 * match. The gallery negotiates with nobody. An unattributable sender (a
 * server without from_player_id) is accepted as before. */
int rnet_lobby__ice_signal_is_for_us(RNetLobby *l, int type, const char *from)
{
    if (type < (int)RNET_SIGNAL_LOCAL_SDP || type > (int)RNET_SIGNAL_SET_CONTROLLING)
        return 1; /* not gameplay ICE: not this rule's business */
    if (rnet_lobby__member_is_spectator(l, l->c.player_id)) {
        LOBBY_DEBUG(l, "dropping ICE signal type=%d -- spectating, negotiating "
                       "with nobody", type);
        return 0;
    }
    if (from && from[0] && rnet_lobby__member_is_spectator(l, from)) {
        LOBBY_DEBUG(l, "dropping ICE signal type=%d from a spectator", type);
        return 0;
    }
    return 1;
}

int rnet_lobby_send_signal_to(RNetLobby *l, const char *to, int type, int flag,
                              const char *text)
{
    char esc[RNET_JSON_ESC_CAP(RNET_LOBBY_SIG_TEXT)];
    char lid_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_ID_LEN)];
    char to_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_ID_LEN)];
    char msg[sizeof(esc) + 512];
    if (!l || !rnet_lobby_connected(l) || !l->c.in_lobby)
        return -1;
    rnet_json_escape(text ? text : "", esc, sizeof(esc));
    rnet_json_escape(l->c.join.lobby_id, lid_esc, sizeof(lid_esc));
    rnet_json_escape(to ? to : "", to_esc, sizeof(to_esc));
    snprintf(msg, sizeof(msg),
             "{\"op\":\"signal\",\"lobby_id\":\"%s\",\"to_player_id\":\"%s\","
             "\"type\":%d,\"flag\":%d,\"text\":\"%s\"}",
             lid_esc, to_esc, type, flag, esc);
    return rnet_lobby__send(l, msg);
}

int rnet_lobby_send_signal(RNetLobby *l, int type, int flag, const char *text)
{
    return rnet_lobby_send_signal_to(l, "", type, flag, text);
}

int rnet_lobby_poll_signal(RNetLobby *l, int *type, int *flag, char *text,
                           size_t text_cap)
{
    RNetLobbyConn *c;
    int i;
    if (!l || l->c.sig_count <= 0)
        return 0;
    c = &l->c;
    i = c->sig_head;
    if (type) *type = c->sig_q[i].type;
    if (flag) *flag = c->sig_q[i].flag;
    if (text && text_cap)
        copy_str(text, text_cap, c->sig_q[i].text);
    c->sig_head = (c->sig_head + 1) % RNET_LOBBY_SIG_QUEUE;
    c->sig_count--;
    return 1;
}

void rnet_lobby_clear_signals(RNetLobby *l)
{
    if (!l)
        return;
    l->c.sig_head = l->c.sig_tail = l->c.sig_count = 0;
}

void rnet_lobby_set_ice_signal_accept(RNetLobby *l, int accept)
{
    if (l)
        l->c.ice_signal_accept = accept ? 1 : 0;
}

/* ── endpoints ───────────────────────────────────────────────────────────── */

int rnet_lobby__endpoint_host_port(const char *ep, char *host, size_t host_cap,
                                   int *port)
{
    const char *colon;
    size_t n;
    if (!ep || !ep[0] || !host || host_cap == 0 || !port)
        return 0;
    colon = strrchr(ep, ':');
    if (!colon || colon == ep || !colon[1])
        return 0;
    n = (size_t)(colon - ep);
    if (n + 1 > host_cap)
        n = host_cap - 1;
    memcpy(host, ep, n);
    host[n] = '\0';
    *port = (int)strtol(colon + 1, NULL, 10);
    return *port > 0 && *port <= 65535;
}

int rnet_lobby__endpoint_usable(const char *endpoint)
{
    const char *colon;
    unsigned port = 0;
    if (!endpoint || !endpoint[0])
        return 0;
    colon = strrchr(endpoint, ':');
    if (!colon || !colon[1])
        return 0;
    for (colon++; *colon; ++colon) {
        if (*colon < '0' || *colon > '9')
            return 0;
        port = port * 10u + (unsigned)(*colon - '0');
        if (port > 65535u)
            return 0;
    }
    return port != 0;
}

int rnet_lobby__using_server_input_relay(RNetLobby *l, const RNetLobbyJoinInfo *j)
{
    if (l->c.match_caps.valid && l->c.match_caps.force_input_relay)
        return 1;
    /* The server rewrote both endpoints to the same relay address. */
    if (j && rnet_lobby__endpoint_usable(j->host_endpoint) &&
        rnet_lobby__endpoint_usable(j->guest_endpoint) &&
        strcmp(j->host_endpoint, j->guest_endpoint) == 0 &&
        (!l->c.my_bind[0] || strcmp(j->host_endpoint, l->c.my_bind) != 0))
        return 1;
    return 0;
}

void rnet_lobby__fill_peer_bind(RNetLobby *l)
{
    RNetLobbyJoinInfo *j = &l->c.join;
    const int force_relay = rnet_lobby__using_server_input_relay(l, j);
    const int seats = j->player_count >= 2 ? j->player_count : j->max_slots;
    /* 3+ seats without the relay: the host is a hub (peer empty ->
     * rnet_session_start_lan_hub), guests dial it from an ephemeral bind. */
    const int host_hub = (l->c.is_host && seats >= 3 && !force_relay) ? 1 : 0;
    memset(j->bind_hostport, 0, sizeof(j->bind_hostport));
    memset(j->peer_hostport, 0, sizeof(j->peer_hostport));
    if (force_relay) {
        /* Everyone dials the relay from an ephemeral bind (same-PC
         * multi-instance must not collide). */
        copy_str(j->bind_hostport, sizeof(j->bind_hostport), "0.0.0.0:0");
        if (rnet_lobby__endpoint_usable(j->host_endpoint))
            copy_str(j->peer_hostport, sizeof(j->peer_hostport), j->host_endpoint);
        else if (rnet_lobby__endpoint_usable(j->guest_endpoint))
            copy_str(j->peer_hostport, sizeof(j->peer_hostport), j->guest_endpoint);
    } else if (l->c.is_host) {
        const char *port = strrchr(l->c.my_bind, ':');
        if (l->cfg.host_bind_all_interfaces && port && port[1])
            /* The advertised address may be a NAT address this machine
             * cannot bind: listen everywhere on the advertised port. */
            snprintf(j->bind_hostport, sizeof(j->bind_hostport), "0.0.0.0:%s",
                     port + 1);
        else
            copy_str(j->bind_hostport, sizeof(j->bind_hostport), l->c.my_bind);
        /* "ip:0" is unusable: leave the peer empty so the transport learns
         * the guest from its first packet. */
        if (!host_hub && rnet_lobby__endpoint_usable(j->guest_endpoint))
            copy_str(j->peer_hostport, sizeof(j->peer_hostport), j->guest_endpoint);
    } else {
        copy_str(j->bind_hostport, sizeof(j->bind_hostport),
                 seats >= 3 ? "0.0.0.0:0" : l->c.my_bind);
        copy_str(j->peer_hostport, sizeof(j->peer_hostport), j->host_endpoint);
    }
}

/* ── chat ────────────────────────────────────────────────────────────────── */

void rnet_lobby__chat_push(RNetLobby *l, RNetLobbyChatRing *ring,
                           const char *player_id, const char *account,
                           const char *from, const char *country,
                           const char *text, const char *mid, int is_system)
{
    RNetLobbyChatMsg *m;
    int idx;
    if (!text || !text[0])
        return;
    if (ring->count < RNET_LOBBY_CHAT_RING) {
        idx = (ring->head + ring->count) % RNET_LOBBY_CHAT_RING;
        ring->count++;
    } else {
        idx = ring->head;
        ring->head = (ring->head + 1) % RNET_LOBBY_CHAT_RING;
    }
    m = &ring->msg[idx];
    memset(m, 0, sizeof(*m));
    copy_str(m->player_id, sizeof(m->player_id), player_id);
    copy_str(m->account, sizeof(m->account), is_system ? "" : account);
    copy_str(m->from, sizeof(m->from), from);
    copy_str(m->country, sizeof(m->country), country);
    copy_str(m->text, sizeof(m->text), text);
    copy_str(m->mid, sizeof(m->mid), is_system ? "" : mid);
    /* Masked on arrival whatever relayed it: an older server did not. */
    if (!is_system)
        (void)rnet_chat_filter_apply(m->text, sizeof(m->text));
    m->is_system = is_system ? 1 : 0;
    /* "Mine" by player id: our own line comes back as the server's echo. */
    m->is_local = (!is_system && l->c.player_id[0] && player_id &&
                   strcmp(player_id, l->c.player_id) == 0) ? 1 : 0;
    m->seq = ++ring->seq;
}

void rnet_lobby_chat_clear(RNetLobby *l)
{
    if (!l)
        return;
    /* seq keeps counting: "newest seen" must not mistake a new room's first
     * line for one already scrolled past. */
    l->c.chat.head = 0;
    l->c.chat.count = 0;
}

static int ring_get(const RNetLobbyChatRing *r, int index, RNetLobbyChatMsg *out)
{
    if (!out || index < 0 || index >= r->count)
        return 0;
    *out = r->msg[(r->head + index) % RNET_LOBBY_CHAT_RING];
    return 1;
}

int rnet_lobby_chat_count(RNetLobby *l)
{
    return l ? l->c.chat.count : 0;
}

int rnet_lobby_chat_get(RNetLobby *l, int index, RNetLobbyChatMsg *out)
{
    return l ? ring_get(&l->c.chat, index, out) : 0;
}

int rnet_lobby_server_chat_count(RNetLobby *l)
{
    return l ? l->c.schat.count : 0;
}

int rnet_lobby_server_chat_get(RNetLobby *l, int index, RNetLobbyChatMsg *out)
{
    return l ? ring_get(&l->c.schat, index, out) : 0;
}

int rnet_lobby_send_chat(RNetLobby *l, const char *text)
{
    char esc[RNET_JSON_ESC_CAP(RNET_LOBBY_CHAT_TEXT_LEN * 4)];
    char msg[sizeof(esc) + 64];
    if (!l || !rnet_lobby_connected(l) || !l->c.in_lobby || !text || !text[0])
        return -1;
    rnet_json_escape(text, esc, sizeof(esc));
    snprintf(msg, sizeof(msg), "{\"op\":\"chat\",\"text\":\"%s\"}", esc);
    return rnet_lobby__send(l, msg);
}

int rnet_lobby_send_server_chat(RNetLobby *l, const char *text)
{
    char esc[RNET_JSON_ESC_CAP(RNET_LOBBY_CHAT_TEXT_LEN * 4)];
    char game_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_NAME_LEN)];
    char msg[sizeof(esc) + sizeof(game_esc) + 96];
    if (!l || !rnet_lobby_connected(l) || !text || !text[0])
        return -1;
    rnet_json_escape(text, esc, sizeof(esc));
    /* The title rides on the line: the server scopes by it even before it
     * has seen a `list` from us. */
    rnet_json_escape(l->game_name, game_esc, sizeof(game_esc));
    snprintf(msg, sizeof(msg),
             "{\"op\":\"server_chat\",\"game_name\":\"%s\",\"text\":\"%s\"}",
             game_esc, esc);
    return rnet_lobby__send(l, msg);
}

int rnet_lobby_report_chat(RNetLobby *l, const char *const *mids, int mid_count,
                           const char *reason, const char *note)
{
    RNetChatReportMeta meta;
    char msg[4096];
    size_t n;
    if (!l || !rnet_lobby_connected(l))
        return -1;
    memset(&meta, 0, sizeof(meta));
    meta.game = l->game_name;
    meta.game_version = l->game_version;
    /* Metadata only: one moderation queue spans every console. */
    meta.platform = l->cfg_platform;
    meta.server = l->c.url;
    meta.lobby = l->c.join.lobby_id;
    meta.scope = l->c.in_lobby ? "lobby" : "server";
    n = rnet_chat_report_build(msg, sizeof(msg), mids, mid_count, reason, note,
                               &meta);
    if (n == 0)
        return -1;
    return rnet_lobby__send(l, msg);
}

const char *rnet_lobby_last_report_ack(RNetLobby *l)
{
    return l ? l->c.last_report_ack : "";
}

int rnet_lobby_set_blocks(RNetLobby *l, const char *accounts)
{
    /* Room for the server's cap (256 ids of up to 40 chars). A list that
     * would not fit is cut at a separator: a half-written id names nobody. */
    const size_t list_cap = RNET_LOBBY_BLOCKS_CAP + 3 * 256 + 8;
    char *list;
    char *msg;
    size_t n = 0;
    int first = 1;
    int rc;
    const char *p;
    if (!l)
        return -1;
    if (accounts != l->blocks)
        copy_str(l->blocks, sizeof(l->blocks), accounts);
    l->blocks_set = 1;
    if (!rnet_lobby_connected(l))
        return -1;
    list = (char *)malloc(list_cap);
    msg = (char *)malloc(list_cap + 64);
    if (!list || !msg) {
        free(list);
        free(msg);
        return -1;
    }
    p = l->blocks;
    list[0] = '\0';
    while (*p) {
        const char *sep = strchr(p, ';');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        if (len && len < RNET_LOBBY_ID_LEN && n + len + 4 < list_cap) {
            size_t k;
            int ok = 1;
            for (k = 0; k < len; ++k)
                if (p[k] == '"' || p[k] == '\\' || (unsigned char)p[k] < 0x20)
                    ok = 0;
            if (ok) {
                if (!first)
                    list[n++] = ',';
                list[n++] = '"';
                memcpy(list + n, p, len);
                n += len;
                list[n++] = '"';
                list[n] = '\0';
                first = 0;
            }
        }
        if (!sep)
            break;
        p = sep + 1;
    }
    snprintf(msg, list_cap + 64, "{\"op\":\"set_blocks\",\"accounts\":[%s]}",
             list);
    rc = rnet_lobby__send(l, msg);
    free(list);
    free(msg);
    return rc;
}

/* ── TURN ────────────────────────────────────────────────────────────────── */

int rnet_lobby__request_turn(RNetLobby *l)
{
    if (!rnet_lobby_connected(l))
        return -1;
    l->c.turn_request_pending = 1;
    return rnet_lobby__send(l, "{\"op\":\"get_turn_credentials\"}");
}

static int turn_fresh(RNetLobby *l, uint32_t margin_s)
{
    uint64_t now = rnet_lobby__now_ms();
    const RNetLobbyTurnCredentials *t = &l->c.turn;
    if (!t->valid || !l->c.turn_received_ms || !t->ttl_secs)
        return 0;
    if (now < l->c.turn_received_ms)
        return 0;
    return (now - l->c.turn_received_ms) / 1000u + margin_s < t->ttl_secs;
}

int rnet_lobby_request_turn_credentials(RNetLobby *l)
{
    if (!l || !rnet_lobby_connected(l))
        return -1;
    if (turn_fresh(l, 60))
        return 0;
    return rnet_lobby__request_turn(l);
}

const RNetLobbyTurnCredentials *rnet_lobby_turn_credentials(RNetLobby *l)
{
    static RNetLobbyTurnCredentials none;
    if (!l)
        return &none;
    if (l->c.turn.valid && !turn_fresh(l, 0)) {
        memset(&l->c.turn, 0, sizeof(l->c.turn));
        l->c.turn_received_ms = 0;
    }
    return &l->c.turn;
}

/* ── rooms ───────────────────────────────────────────────────────────────── */

void rnet_lobby_set_max_slots(RNetLobby *l, int max_slots)
{
    if (l)
        l->max_slots_pref = clampi(max_slots, 2, l->cfg.max_players);
}

int rnet_lobby_create(RNetLobby *l, const char *name, const char *game_name,
                      const char *game_version, const char *password,
                      const char *host_bind, const RNetLobbyMatchCaps *caps,
                      int max_slots)
{
    char msg[RNET_LOBBY_CAPS_JSON_LEN + 2048];
    char cj[RNET_LOBBY_CAPS_JSON_LEN + 32];
    char name_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_NAME_LEN)];
    char gn_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_NAME_LEN)];
    char gv_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_VERSION_LEN)];
    char pw_esc[RNET_JSON_ESC_CAP(128)];
    char bind_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_ENDPOINT_LEN)];
    char dn_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_NAME_LEN)];
    char fp[96];
    char bind_default[32];
    int slots, n;
    if (!l || !rnet_lobby_connected(l))
        return -1;
    if (password && strlen(password) > 128) {
        copy_str(l->c.join.last_error, sizeof(l->c.join.last_error),
                 "password_invalid");
        return -1;
    }
    slots = max_slots > 0 ? max_slots : l->max_slots_pref;
    /* PLAYERS, not members: the table grew for the gallery, the number of
     * controllers did not. */
    slots = clampi(slots, 2, l->cfg.max_players);
    if ((game_name && game_name[0]) || (game_version && game_version[0])) {
        /* An explicit title or pin on create becomes the identity. Copies:
         * the setter writes the very buffers it would otherwise read. */
        char gn[RNET_LOBBY_NAME_LEN], gv[RNET_LOBBY_VERSION_LEN];
        copy_str(gn, sizeof(gn), (game_name && game_name[0]) ? game_name
                                                             : l->game_name);
        copy_str(gv, sizeof(gv), (game_version && game_version[0])
                                     ? game_version
                                     : l->real_version);
        rnet_lobby_set_game_identity(l, gn, gv);
    }
    snprintf(bind_default, sizeof(bind_default), "0.0.0.0:%d", l->cfg.host_port);
    copy_str(l->c.my_bind, sizeof(l->c.my_bind),
             (host_bind && host_bind[0]) ? host_bind : bind_default);
    copy_str(l->c.room_name, sizeof(l->c.room_name),
             (name && name[0]) ? name : "Lobby");
    l->c.room_has_password = (password && password[0]) ? 1 : 0;
    l->c.join.last_error[0] = '\0';
    l->c.name_refused[0] = '\0';
    if (caps_member(l, caps, cj, sizeof(cj), "create"))
        adopt_caps(l, caps);
    rnet_json_escape(l->c.room_name, name_esc, sizeof(name_esc));
    rnet_json_escape(l->game_name[0] ? l->game_name : "Game", gn_esc, sizeof(gn_esc));
    rnet_json_escape(l->game_version, gv_esc, sizeof(gv_esc));
    rnet_json_escape(password ? password : "", pw_esc, sizeof(pw_esc));
    rnet_json_escape(l->c.my_bind, bind_esc, sizeof(bind_esc));
    rnet_json_escape(l->display_name[0] ? l->display_name : "Host", dn_esc,
                     sizeof(dn_esc));
    fp[0] = '\0';
    if (l->cfg.fingerprint_in_rooms && l->fp[0])
        snprintf(fp, sizeof(fp), ",\"disc_fp\":\"%s\"", l->fp);
    n = snprintf(msg, sizeof(msg),
                 "{\"op\":\"create\",\"name\":\"%s\",\"game_name\":\"%s\","
                 "\"game_version\":\"%s\",\"password\":\"%s\",\"max_slots\":%d,"
                 "\"allow_spectators\":%s,\"host_bind\":\"%s\","
                 "\"display_name\":\"%s\"%s%s}",
                 name_esc, gn_esc, gv_esc, pw_esc, slots,
                 l->allow_spectators_pref ? "true" : "false", bind_esc, dn_esc,
                 fp, cj);
    if (n < 0 || (size_t)n >= sizeof(msg))
        return -1;
    return rnet_lobby__send(l, msg);
}

/* Never advertise :0 -- the server rewrites it to peer_ip:0 and
 * rnet_session_start_lan rejects port 0. */
static void normalize_guest_bind(RNetLobby *l, const char *guest_bind,
                                 char *out, size_t cap)
{
    int port;
    if (guest_bind && guest_bind[0] && rnet_lobby__endpoint_usable(guest_bind)) {
        copy_str(out, cap, guest_bind);
        return;
    }
    port = rnet_udp_find_free_port(l->cfg.guest_port, 32);
    if (port <= 0)
        port = l->cfg.guest_port;
    snprintf(out, cap, "0.0.0.0:%d", port);
}

int rnet_lobby_join(RNetLobby *l, const char *lobby_id, const char *password,
                    const char *guest_bind)
{
    char offer[RNET_LOBBY_MAX_MODS * RNET_LOBBY_MOD_ROW_JSON + 64];
    char msg[sizeof(offer) + 1536];
    char lid_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_ID_LEN)];
    char pw_esc[RNET_JSON_ESC_CAP(128)];
    char bind_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_ENDPOINT_LEN)];
    char dn_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_NAME_LEN)];
    char gn_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_NAME_LEN)];
    char gv_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_VERSION_LEN)];
    char fp[96];
    int n;
    if (!l || !rnet_lobby_connected(l) || !lobby_id || !lobby_id[0])
        return -1;
    if (password && strlen(password) > 128) {
        copy_str(l->c.join.last_error, sizeof(l->c.join.last_error),
                 "password_invalid");
        return -1;
    }
    normalize_guest_bind(l, guest_bind, l->c.my_bind, sizeof(l->c.my_bind));
    l->c.join.last_error[0] = '\0';
    l->c.need_mods_count = 0;
    offer[0] = '\0';
    if (l->offer_fn && !rnet_lobby__append_mod_offer(l, offer, sizeof(offer))) {
        /* Refuse rather than send a short offer: the server would turn this
         * peer away over mods it holds. */
        copy_str(l->c.join.last_error, sizeof(l->c.join.last_error),
                 "mod_offer_too_large");
        return -1;
    }
    rnet_json_escape(lobby_id, lid_esc, sizeof(lid_esc));
    rnet_json_escape(password ? password : "", pw_esc, sizeof(pw_esc));
    rnet_json_escape(l->c.my_bind, bind_esc, sizeof(bind_esc));
    rnet_json_escape(l->display_name[0] ? l->display_name : "Guest", dn_esc,
                     sizeof(dn_esc));
    rnet_json_escape(l->game_name, gn_esc, sizeof(gn_esc));
    rnet_json_escape(l->game_version, gv_esc, sizeof(gv_esc));
    fp[0] = '\0';
    if (l->cfg.fingerprint_in_rooms && l->fp[0])
        snprintf(fp, sizeof(fp), ",\"disc_fp\":\"%s\"", l->fp);
    n = snprintf(msg, sizeof(msg),
                 "{\"op\":\"join\",\"lobby_id\":\"%s\",\"password\":\"%s\","
                 "\"guest_bind\":\"%s\",\"display_name\":\"%s\","
                 "\"game_name\":\"%s\",\"game_version\":\"%s\"%s%s}",
                 lid_esc, pw_esc, bind_esc, dn_esc, gn_esc, gv_esc, fp, offer);
    if (n < 0 || (size_t)n >= sizeof(msg)) {
        copy_str(l->c.join.last_error, sizeof(l->c.join.last_error),
                 "join_too_large");
        return -1;
    }
    return rnet_lobby__send(l, msg);
}

/* Local teardown shared by leave() and the server's left / kicked /
 * lobby_closed. */
static void drop_room(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    rnet_lobby_chat_clear(l);
    rnet_lobby__am_on_left(l);
    rnet_lobby__lat_on_leave(l);
    rnet_lobby__mod_reset(l);
    c->swap_in_valid = 0;
    c->swap_out = 0;
    c->in_lobby = 0;
    c->is_host = 0;
    c->host_player_id[0] = '\0';
    c->member_count = 0;
    c->local_ready = 0;
    c->spectator_offer_sent = 0;
    c->all_ready = 0;
    c->launch_pending = 0;
    c->ice_rtt_suspended = 0;
    rnet_lobby_clear_signals(l);
    rnet_lobby__ice_gate_rest(l);
    rnet_lobby_match_caps_init(l, &c->match_caps);
    member_rtt_clear(l);
}

int rnet_lobby_leave(RNetLobby *l)
{
    int rc;
    if (!l)
        return -1;
    rc = rnet_lobby__send(l, "{\"op\":\"leave\"}");
    drop_room(l);
    return rc;
}

int rnet_lobby_in_lobby(RNetLobby *l)
{
    return l ? l->c.in_lobby : 0;
}

int rnet_lobby_is_host(RNetLobby *l)
{
    return l ? l->c.is_host : 0;
}

const char *rnet_lobby_host_player_id(RNetLobby *l)
{
    return l ? l->c.host_player_id : "";
}

const RNetLobbyJoinInfo *rnet_lobby_join_info(RNetLobby *l)
{
    static RNetLobbyJoinInfo none;
    return l ? &l->c.join : &none;
}

void rnet_lobby_clear_last_error(RNetLobby *l)
{
    if (!l)
        return;
    l->c.join.last_error[0] = '\0';
    l->c.name_refused[0] = '\0';
}

int rnet_lobby_kick(RNetLobby *l, int slot)
{
    char msg[64];
    if (!l || !rnet_lobby_connected(l) || !l->c.in_lobby || !l->c.is_host)
        return -1;
    if (!rnet_lobby_seat_valid(l, slot))
        return -1;
    snprintf(msg, sizeof(msg), "{\"op\":\"kick\",\"slot\":%d}", slot);
    return rnet_lobby__send(l, msg);
}

int rnet_lobby_move(RNetLobby *l, int from_slot, int to_slot)
{
    char msg[96];
    if (!l || !rnet_lobby_connected(l) || !l->c.in_lobby || !l->c.is_host)
        return -1;
    /* Either seat may be in the gallery: this promotes and demotes too. */
    if (!rnet_lobby_seat_valid(l, from_slot) || !rnet_lobby_seat_valid(l, to_slot) ||
        from_slot == to_slot)
        return -1;
    snprintf(msg, sizeof(msg), "{\"op\":\"move\",\"from_slot\":%d,\"to_slot\":%d}",
             from_slot, to_slot);
    return rnet_lobby__send(l, msg);
}

int rnet_lobby_seat_move_self(RNetLobby *l, int to_slot)
{
    char msg[80];
    if (!l || !rnet_lobby_connected(l) || !l->c.in_lobby)
        return -1;
    if (!rnet_lobby_seat_valid(l, to_slot))
        return -1;
    snprintf(msg, sizeof(msg), "{\"op\":\"seat_move\",\"to_slot\":%d}", to_slot);
    return rnet_lobby__send(l, msg);
}

int rnet_lobby_seat_swap_request(RNetLobby *l, int target_slot)
{
    char msg[96];
    if (!l || !rnet_lobby_connected(l) || !l->c.in_lobby)
        return -1;
    if (!rnet_lobby_seat_valid(l, target_slot))
        return -1;
    if (l->c.swap_out == 1)
        return -1; /* one ask at a time */
    snprintf(msg, sizeof(msg), "{\"op\":\"seat_swap_request\",\"target_slot\":%d}",
             target_slot);
    if (rnet_lobby__send(l, msg) != 0)
        return -1;
    l->c.swap_out = 1;
    return 0;
}

int rnet_lobby_seat_swap_incoming(RNetLobby *l, char *who, size_t who_cap,
                                  int *from_slot)
{
    if (!l || !l->c.swap_in_valid)
        return 0;
    if (who && who_cap)
        copy_str(who, who_cap, l->c.swap_in_asker_name);
    if (from_slot)
        *from_slot = l->c.swap_in_from_slot;
    return 1;
}

int rnet_lobby_seat_swap_respond(RNetLobby *l, int accept)
{
    char msg[192];
    char asker_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_ID_LEN)];
    if (!l || !l->c.swap_in_valid)
        return -1;
    l->c.swap_in_valid = 0;
    if (!rnet_lobby_connected(l) || !l->c.in_lobby)
        return -1;
    rnet_json_escape(l->c.swap_in_asker_id, asker_esc, sizeof(asker_esc));
    snprintf(msg, sizeof(msg),
             "{\"op\":\"seat_swap_answer\",\"accept\":%s,\"asker_player_id\":\"%s\"}",
             accept ? "true" : "false", asker_esc);
    return rnet_lobby__send(l, msg);
}

int rnet_lobby_seat_swap_outgoing(RNetLobby *l)
{
    return l ? l->c.swap_out : 0;
}

void rnet_lobby_seat_swap_clear(RNetLobby *l)
{
    if (l && l->c.swap_out != 1)
        l->c.swap_out = 0;
}

void rnet_lobby_set_allow_spectators(RNetLobby *l, int allow)
{
    if (l)
        l->allow_spectators_pref = allow ? 1 : 0;
}

int rnet_lobby_allow_spectators_pref(RNetLobby *l)
{
    return l ? l->allow_spectators_pref : 0;
}

int rnet_lobby_allow_spectators(RNetLobby *l)
{
    return l && l->c.join.allow_spectators ? 1 : 0;
}

int rnet_lobby_max_spectators(RNetLobby *l)
{
    return l ? l->c.join.max_spectators : 0;
}

int rnet_lobby_spectator_count(RNetLobby *l)
{
    return l ? l->c.join.spectator_count : 0;
}

int rnet_lobby_local_is_spectator(RNetLobby *l)
{
    return l && l->c.join.local_is_spectator ? 1 : 0;
}

/* ── start / launch ──────────────────────────────────────────────────────── */

int rnet_lobby_request_start(RNetLobby *l, const RNetLobbyMatchCaps *caps)
{
    char msg[RNET_LOBBY_CAPS_JSON_LEN + 128];
    char cj[RNET_LOBBY_CAPS_JSON_LEN + 32];
    char who[RNET_LOBBY_NAME_LEN];
    char what[RNET_LOBBY_MOD_ID_LEN + RNET_LOBBY_MOD_VER_LEN + 2];
    if (!l || !rnet_lobby_connected(l) || !l->c.in_lobby || !l->c.is_host)
        return -1;
    /* THE gate, and not the door: a peer without the mods belongs in the room
     * to fetch them, but the match must not start while two peers would patch
     * guest memory differently. Checked at the only moment it is current. */
    if (rnet_lobby_match_blocked_by_mods(l, who, sizeof(who), what,
                                         sizeof(what)) > 0) {
        copy_str(l->c.join.last_error, sizeof(l->c.join.last_error),
                 "peer_needs_mods");
        LOBBY_WARN(l, "not starting -- %s does not have %s (and possibly more). "
                      "They can download it from you in the lobby.",
                   who[0] ? who : "a player", what[0] ? what : "a required mod");
        return -2;
    }
    if (caps_member(l, caps, cj, sizeof(cj), "start"))
        adopt_caps(l, caps);
    snprintf(msg, sizeof(msg), "{\"op\":\"start\"%s}", cj);
    return rnet_lobby__send(l, msg);
}

int rnet_lobby_launch_pending(RNetLobby *l)
{
    return l ? l->c.launch_pending : 0;
}

void rnet_lobby_clear_launch_pending(RNetLobby *l)
{
    if (!l)
        return;
    l->c.launch_pending = 0;
}

int rnet_lobby_try_fill_launch(RNetLobby *l, RNetLobbyJoinInfo *out)
{
    const RNetLobbyJoinInfo *j;
    if (!l || !out || !l->c.launch_pending)
        return 0;
    j = &l->c.join;
    if (!j->bind_hostport[0])
        return 0;
    /* A guest needs a concrete peer; the host may learn its from the first
     * packet (or run the hub). */
    if (!l->c.is_host && !j->peer_hostport[0])
        return 0;
    *out = *j;
    return 1;
}

/* ── desync ──────────────────────────────────────────────────────────────── */

int rnet_lobby_report_desync(RNetLobby *l, const RNetLobbyDesyncReport *r)
{
    char msg[2048];
    char part_esc[96];
    char exempt_esc[1024];
    char gv_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_VERSION_LEN)];
    char lid_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_ID_LEN)];
    int n;
    if (!l || !r || !rnet_lobby_connected(l))
        return -1;
    rnet_json_escape(r->partition ? r->partition : "?", part_esc, sizeof(part_esc));
    rnet_json_escape(r->mod_exempt ? r->mod_exempt : "", exempt_esc,
                     sizeof(exempt_esc));
    rnet_json_escape(l->game_version, gv_esc, sizeof(gv_esc));
    rnet_json_escape(l->c.join.lobby_id, lid_esc, sizeof(lid_esc));
    /* Digests as hex STRINGS: a reader that prints a JSON number as
     * 4.29497e+09 destroys the only field the row exists to compare. */
    n = snprintf(msg, sizeof(msg),
                 "{\"op\":\"desync_report\",\"v\":1,\"lobby_id\":\"%s\","
                 "\"game_version\":\"%s\",\"tick\":%u,\"partition\":\"%s\","
                 "\"mine\":\"%08x\",\"theirs\":\"%08x\",\"role\":\"%s\","
                 "\"disc_fp\":\"%s\",\"mod_exempt\":\"%s\"}",
                 lid_esc, gv_esc, (unsigned)r->tick, part_esc, (unsigned)r->mine,
                 (unsigned)r->theirs, r->is_host ? "host" : "guest", l->fp,
                 exempt_esc);
    if (n < 0 || (size_t)n >= sizeof(msg))
        return -1;
    return rnet_lobby__send(l, msg);
}

/* ── the dispatcher ──────────────────────────────────────────────────────── */

static void on_welcome(RNetLobby *l, RNetJsonSpan m)
{
    rnet_json_str(m, "player_id", l->c.player_id, sizeof(l->c.player_id));
    l->c.welcomed = 1;
    /* A fresh socket: the server holds the ticket and the ruleset answer per
     * connection, so neither survives from the last one. */
    rnet_lobby__am_on_connection_reset(l);
    /* Who we are AND what we play, first: the server scopes players-online
     * and server chat by title, and a client that chats before listing would
     * otherwise have none. */
    send_hello(l);
    LOBBY_INFO(l, "hello as \"%s\" for game \"%s\"", l->display_name,
               l->game_name[0] ? l->game_name : "(none)");
    send_list_request(l);
    (void)rnet_lobby__request_turn(l);
    /* The server holds a block set per connection and never persists it. */
    if (l->blocks_set)
        (void)rnet_lobby_set_blocks(l, l->blocks);
}

static void on_turn(RNetLobby *l, RNetJsonSpan m)
{
    RNetLobbyTurnCredentials *t = &l->c.turn;
    l->c.turn_request_pending = 0;
    memset(t, 0, sizeof(*t));
    l->c.turn_received_ms = 0;
    if (!rnet_json_bool(m, "ok", 0)) {
        char err[64];
        /* No automatic re-ask for 30 s: a server without TURN answers every
         * request with this, and a probe asking each pump would flood it. */
        l->c.turn_retry_ms = rnet_lobby__now_ms() + 30000u;
        rnet_json_str(m, "error", err, sizeof(err));
        LOBBY_WARN(l, "turn_credentials failed (%s) -- ICE is STUN-only unless "
                      "the title configures its own TURN",
                   err[0] ? err : "unknown");
        return;
    }
    rnet_json_str(m, "stun_host", t->stun_host, sizeof(t->stun_host));
    rnet_json_str(m, "turn_host", t->turn_host, sizeof(t->turn_host));
    rnet_json_str(m, "username", t->username, sizeof(t->username));
    rnet_json_str(m, "password", t->password, sizeof(t->password));
    rnet_json_str(m, "realm", t->realm, sizeof(t->realm));
    t->stun_port = rnet_json_int(m, "stun_port", 3478);
    t->turn_port = rnet_json_int(m, "turn_port", 3478);
    t->turns_port = rnet_json_int(m, "turns_port", 0);
    t->ttl_secs = (uint32_t)rnet_json_int(m, "ttl_secs", 86400);
    if (t->turn_host[0] && t->username[0] && t->password[0]) {
        t->valid = 1;
        l->c.turn_received_ms = rnet_lobby__now_ms();
        if (!l->c.turn_received_ms)
            l->c.turn_received_ms = 1;
        LOBBY_INFO(l, "turn_credentials ok stun=%s:%d turn=%s:%d ttl=%us",
                   t->stun_host[0] ? t->stun_host : "(none)", t->stun_port,
                   t->turn_host, t->turn_port, (unsigned)t->ttl_secs);
    } else {
        LOBBY_WARN(l, "turn_credentials ok but incomplete fields");
    }
}

static void read_endpoints(RNetLobby *l, RNetJsonSpan m)
{
    RNetLobbyJoinInfo *j = &l->c.join;
    RNetJsonSpan v;
    if (rnet_json_find(m, "host_endpoint", &v))
        rnet_json_unescape(v, j->host_endpoint, sizeof(j->host_endpoint));
    if (rnet_json_find(m, "guest_endpoint", &v))
        rnet_json_unescape(v, j->guest_endpoint, sizeof(j->guest_endpoint));
}

static void read_host_player_id(RNetLobby *l, RNetJsonSpan m)
{
    char id[RNET_LOBBY_ID_LEN];
    rnet_json_str(m, "host_player_id", id, sizeof(id));
    if (id[0])
        copy_str(l->c.host_player_id, sizeof(l->c.host_player_id), id);
}

static void maybe_auto_ready(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    /* A title without a Ready UI re-arms it: older servers still gate start
     * on all_ready, and kick/move/start clear it. */
    if (!l->cfg.auto_ready || !c->in_lobby)
        return;
    if (c->join.local_is_spectator) {
        /* The server keeps a spectator's ready false (no vote) and answers
         * every set_ready with a lobby_update. Re-arming on each update, as
         * the snesrecomp copy did, loops forever for anyone in the gallery.
         * The gallery announces its offer once per seat instead. */
        if (!c->spectator_offer_sent) {
            c->spectator_offer_sent = 1;
            rnet_lobby__send_set_ready(l, 1);
        }
        return;
    }
    c->spectator_offer_sent = 0;
    if (!c->local_ready)
        rnet_lobby__send_set_ready(l, 1);
}

static void on_created(RNetLobby *l, RNetJsonSpan m)
{
    RNetLobbyConn *c = &l->c;
    rnet_lobby_chat_clear(l); /* a new room starts with an empty log */
    c->spectator_offer_sent = 0;
    c->in_lobby = 1;
    c->is_host = 1;
    c->join.ok = 1;
    c->launch_pending = 0;
    c->all_ready = 0;
    c->ice_rtt_suspended = 0;
    member_rtt_clear(l);
    rnet_json_str(m, "lobby_id", c->join.lobby_id, sizeof(c->join.lobby_id));
    c->join.session_id = (uint32_t)rnet_json_i64(m, "session_id", 1);
    c->join.local_slot = rnet_json_int(m, "local_slot", 0);
    read_endpoints(l, m);
    c->join.player_count = 1;
    c->join.max_slots = rnet_json_int(m, "max_slots", l->max_slots_pref);
    c->join.last_error[0] = '\0';
    copy_str(c->host_player_id, sizeof(c->host_player_id), c->player_id);
    read_host_player_id(l, m);
    ingest_caps(l, m);
    rnet_lobby__fill_peer_bind(l);
    rnet_lobby__parse_slots(l, m);
    if (c->member_count == 0) {
        memset(&c->members[0], 0, sizeof(c->members[0]));
        c->members[0].slot = 0;
        copy_str(c->members[0].player_id, sizeof(c->members[0].player_id),
                 c->player_id);
        copy_str(c->members[0].display_name, sizeof(c->members[0].display_name),
                 l->display_name);
        c->member_json[0][0] = '\0';
        c->member_offer_count[0] = 0;
        c->member_count = 1;
        c->local_ready = 0;
    }
    rnet_lobby__lat_on_created(l);
    maybe_auto_ready(l);
}

static void on_joined(RNetLobby *l, RNetJsonSpan m)
{
    RNetLobbyConn *c = &l->c;
    rnet_lobby_chat_clear(l);
    rnet_lobby__am_on_joined(l);
    c->spectator_offer_sent = 0;
    c->in_lobby = 1;
    c->is_host = 0;
    c->join.ok = 1;
    c->launch_pending = 0;
    c->all_ready = 0;
    c->ice_rtt_suspended = 0;
    c->need_mods_count = 0;
    member_rtt_clear(l);
    rnet_json_str(m, "lobby_id", c->join.lobby_id, sizeof(c->join.lobby_id));
    c->join.session_id = (uint32_t)rnet_json_i64(m, "session_id", 1);
    c->join.local_slot = rnet_json_int(m, "local_slot", 1);
    read_endpoints(l, m);
    c->join.player_count = rnet_json_int(m, "player_count", 2);
    c->join.max_slots = rnet_json_int(m, "max_slots", 2);
    c->join.last_error[0] = '\0';
    read_host_player_id(l, m);
    ingest_caps(l, m);
    rnet_lobby__fill_peer_bind(l);
    rnet_lobby__parse_slots(l, m);
    /* The real `joined` carries no seat arrays (the lobby_update that follows
     * does), but it says the ROLE outright: a joiner that overflowed into the
     * gallery must know before it shows or sends anything. */
    if (rnet_lobby__member_index_for_player(l, c->player_id) < 0) {
        int base = rnet_lobby_spectator_slot_base(l);
        c->join.local_slot = rnet_json_int(m, "local_slot", 1);
        c->join.local_is_spectator =
            rnet_json_bool(m, "spectator", c->join.local_slot >= base) ? 1 : 0;
    }
    /* An automatch room says so itself; the state heuristic covers servers
     * that predate the flag. */
    if (rnet_json_bool(m, "automatch", 0))
        c->am.in_automatch_room = 1;
    maybe_auto_ready(l);
}

static void on_lobby_update(RNetLobby *l, RNetJsonSpan m)
{
    RNetLobbyConn *c = &l->c;
    read_endpoints(l, m);
    c->join.player_count = rnet_json_int(m, "player_count", c->join.player_count);
    c->join.max_slots = rnet_json_int(m, "max_slots", c->join.max_slots);
    c->join.session_id =
        (uint32_t)rnet_json_i64(m, "session_id", (long long)c->join.session_id);
    c->all_ready = rnet_json_bool(m, "all_ready", 0);
    read_host_player_id(l, m);
    /* Host migration: the server names the host; follow it. */
    if (c->host_player_id[0] && c->player_id[0])
        c->is_host = strcmp(c->host_player_id, c->player_id) == 0;
    ingest_caps(l, m);
    rnet_lobby__parse_slots(l, m);
    /* An update that seats us means we are in the room (an automatch pairing
     * can reach this before its `joined`). Only when it names us: an update
     * that raced our own `leave` must not re-seat us. */
    if (rnet_lobby__member_index_for_player(l, c->player_id) >= 0)
        c->in_lobby = 1;
    rnet_lobby__fill_peer_bind(l);
    maybe_auto_ready(l);
}

static void on_launch(RNetLobby *l, RNetJsonSpan m)
{
    RNetLobbyConn *c = &l->c;
    RNetLobbyJoinInfo *j = &c->join;
    char relay[RNET_LOBBY_ENDPOINT_LEN];
    int force_relay, peer_bad;
    read_endpoints(l, m);
    rnet_json_str(m, "relay_endpoint", relay, sizeof(relay));
    rnet_json_str(m, "transport", j->transport, sizeof(j->transport));
    j->player_count = rnet_json_int(m, "player_count", j->player_count);
    j->max_slots = rnet_json_int(m, "max_slots", j->max_slots);
    j->session_id = (uint32_t)rnet_json_i64(m, "session_id", (long long)j->session_id);
    read_host_player_id(l, m);
    ingest_caps(l, m);
    if (rnet_lobby__endpoint_usable(relay)) {
        char raw[RNET_LOBBY_ENDPOINT_LEN];
        copy_str(raw, sizeof(raw), relay);
        if (rnet_lobby__rewrite_relay_endpoint(l, relay, sizeof(relay)) > 0)
            LOBBY_INFO(l, "relay_endpoint %s -> %s (lobby host)", raw, relay);
        copy_str(j->host_endpoint, sizeof(j->host_endpoint), relay);
        copy_str(j->guest_endpoint, sizeof(j->guest_endpoint), relay);
        c->match_caps.valid = 1;
        c->match_caps.force_input_relay = 1;
    } else if (c->match_caps.valid) {
        /* A launch without a relay: the caps' flag is the host's toggle and
         * must not claim a relay that was not allocated. */
        c->match_caps.force_input_relay = 0;
    }
    rnet_lobby__parse_slots(l, m);
    /* This match's transport, decided HERE and recorded on the join. Re-read
     * later from the caps it was erased by any republish landing between the
     * launch and the start, and the match fell back to p2p. Restated on every
     * launch, both ways. */
    force_relay = rnet_lobby__using_server_input_relay(l, j);
    j->force_input_relay = force_relay ? 1 : 0;
    rnet_lobby__fill_peer_bind(l);
    peer_bad = !rnet_lobby__endpoint_usable(j->peer_hostport);
    if (force_relay) {
        if (peer_bad) {
            copy_str(j->last_error, sizeof(j->last_error), "missing_endpoints");
            c->launch_pending = 0;
            return;
        }
    } else if (l->cfg.require_server_relay && strcmp(j->transport, "ice_p2p") == 0) {
        LOBBY_ERROR(l, "launch transport=ice_p2p refused -- this title requires "
                       "the server relay (upgrade the lobby server)");
        copy_str(j->last_error, sizeof(j->last_error), "sfu_required");
        c->launch_pending = 0;
        return;
    } else if (!j->host_endpoint[0] || !j->bind_hostport[0] ||
               (!c->is_host && peer_bad)) {
        copy_str(j->last_error, sizeof(j->last_error), "missing_endpoints");
        c->launch_pending = 0;
        return;
    }
    /* A prior lobby error must not leave ok=0, or fill_launch refuses the
     * match forever while launch_pending stays set. */
    j->ok = 1;
    j->last_error[0] = '\0';
    c->launch_pending = 1;
    /* The match owns the ICE signals now. Anything queued so far was the
     * waiting room's (the server orders our launch ahead of any peer's
     * post-launch offer), so clearing loses nothing of the match's. */
    c->ice_rtt_suspended = 1;
    rnet_lobby_clear_signals(l);
    c->ice_signal_accept = 1;
    rnet_lobby__lat_on_launch(l);
}

static void on_chat(RNetLobby *l, RNetJsonSpan m, RNetLobbyChatRing *ring,
                    int allow_system)
{
    char text[RNET_LOBBY_CHAT_TEXT_LEN];
    char from_id[RNET_LOBBY_ID_LEN];
    char acct[RNET_LOBBY_ID_LEN];
    char from[RNET_LOBBY_NAME_LEN];
    char country[4];
    char mid[RNET_LOBBY_MID_LEN];
    rnet_json_str(m, "text", text, sizeof(text));
    rnet_json_str(m, "from_player_id", from_id, sizeof(from_id));
    rnet_json_str(m, "from_account", acct, sizeof(acct));
    rnet_json_str(m, "from", from, sizeof(from));
    rnet_json_str(m, "country", country, sizeof(country));
    rnet_json_str(m, "mid", mid, sizeof(mid));
    rnet_lobby__chat_push(l, ring, from_id, acct, from, country, text, mid,
                          allow_system ? rnet_json_bool(m, "system", 0) : 0);
}

static void on_signal(RNetLobby *l, RNetJsonSpan m)
{
    char text[RNET_LOBBY_SIG_TEXT];
    char from[RNET_LOBBY_ID_LEN];
    int type = rnet_json_int(m, "type", 0);
    int flag = rnet_json_int(m, "flag", 0);
    rnet_json_str(m, "text", text, sizeof(text));
    rnet_json_str(m, "from_player_id", from, sizeof(from));
    if (rnet_lobby__lat_on_signal(l, type, text, from))
        return;
    if (rnet_lobby__mod_on_signal(l, type, flag, text, from))
        return;
    if (!rnet_lobby__ice_signal_is_for_us(l, type, from))
        return;
    rnet_lobby__enqueue_signal(l, type, flag, text);
}

static void on_need_mods(RNetLobby *l, RNetJsonSpan m)
{
    /* The server answers a refused join with its own op carrying the list;
     * falling through to a generic handler dropped it entirely and the join
     * silently did nothing. A refusal has to explain itself. */
    RNetLobbyConn *c = &l->c;
    int i;
    c->need_mods_count = rnet_lobby__parse_mod_pkgs(m, "mods", c->need_mods,
                                                    RNET_LOBBY_MAX_MODS);
    c->need_mods_can_transfer = rnet_json_bool(m, "can_transfer", 0);
    rnet_json_str(m, "lobby_id", c->need_mods_lobby_id, sizeof(c->need_mods_lobby_id));
    rnet_json_str(m, "host_player_id", c->need_mods_host_player_id,
                  sizeof(c->need_mods_host_player_id));
    copy_str(c->join.last_error, sizeof(c->join.last_error), "need_mods");
    c->join.ok = 0;
    c->in_lobby = 0;
    LOBBY_WARN(l, "refused - this lobby needs %d mod(s) this build does not have "
                  "(server can_transfer=%d)",
               c->need_mods_count, c->need_mods_can_transfer);
    for (i = 0; i < c->need_mods_count; ++i)
        LOBBY_WARN(l, "  missing %s@%s%s%s", c->need_mods[i].id,
                   c->need_mods[i].ver, c->need_mods[i].name[0] ? " - " : "",
                   c->need_mods[i].name);
    if (c->need_mods_count == 0)
        LOBBY_WARN(l, "  the server named none: it read a plan it could not "
                      "parse -- check the host's build");
}

static int code_is(const char *code, const char *const *list, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (strcmp(code, list[i]) == 0)
            return 1;
    return 0;
}

static void on_error(RNetLobby *l, RNetJsonSpan m)
{
    static const char *const name_codes[] = {
        "name_rejected", "lobby_name_rejected", "password_invalid"
    };
    /* Create/join failures end the seat attempt; an in-lobby op refused on
     * an older server (not_host, bad_slot, ...) must not make the room look
     * abandoned. */
    static const char *const fatal[] = {
        "bad_password", "need_password", "full", "gone", "already_in_lobby",
        "lobby_limit", "version_mismatch", "game_mismatch", "disc_mismatch",
        "no_such_lobby"
    };
    RNetLobbyConn *c = &l->c;
    char code[RNET_LOBBY_ERROR_LEN];
    rnet_json_str(m, "code", code, sizeof(code));
    if (strcmp(code, "session_invalid") == 0) {
        /* Not a room failure: the connection simply stays a guest. */
        c->session_invalid = 1;
        LOBBY_WARN(l, "the server rejected the account session; continuing as "
                      "a guest");
        return;
    }
    copy_str(c->join.last_error, sizeof(c->join.last_error), code);
    if (code_is(code, name_codes, sizeof(name_codes) / sizeof(name_codes[0]))) {
        copy_str(c->name_refused, sizeof(c->name_refused), code);
        LOBBY_WARN(l, "the server refused a name or password (%s)", code);
    }
    if (rnet_lobby__am_claim_error(l, code, m))
        return;
    if (!c->in_lobby || code_is(code, fatal, sizeof(fatal) / sizeof(fatal[0])))
        c->join.ok = 0;
}

static void handle_server_json(RNetLobby *l, const char *json)
{
    RNetJsonSpan m = rnet_json_span(json);
    RNetLobbyConn *c = &l->c;
    char op[40];
    if (rnet_json_kind(m) != '{')
        return;
    rnet_json_str(m, "op", op, sizeof(op));
    if (!op[0])
        return;
    if (!strcmp(op, "welcome")) { on_welcome(l, m); return; }
    if (!strcmp(op, "hello_ok")) {
        rnet_json_str(m, "display_name", c->accepted_name, sizeof(c->accepted_name));
        return;
    }
    if (!strcmp(op, "turn_credentials")) { on_turn(l, m); return; }
    if (!strcmp(op, "lobby_list")) { parse_lobby_list(l, m); return; }
    if (!strcmp(op, "created")) { on_created(l, m); return; }
    if (!strcmp(op, "joined")) { on_joined(l, m); return; }
    if (!strcmp(op, "lobby_update")) { on_lobby_update(l, m); return; }
    if (!strcmp(op, "launch")) { on_launch(l, m); return; }
    if (!strcmp(op, "seat_swap_ask")) {
        c->swap_in_valid = 1;
        rnet_json_str(m, "asker_player_id", c->swap_in_asker_id,
                      sizeof(c->swap_in_asker_id));
        rnet_json_str(m, "asker_name", c->swap_in_asker_name,
                      sizeof(c->swap_in_asker_name));
        c->swap_in_from_slot = rnet_json_int(m, "from_slot", -1);
        return;
    }
    if (!strcmp(op, "seat_swap_result")) {
        c->swap_out = rnet_json_bool(m, "accept", 0) ? 2 : -1;
        return;
    }
    if (!strcmp(op, "chat")) { on_chat(l, m, &c->chat, 1); return; }
    if (!strcmp(op, "server_chat")) { on_chat(l, m, &c->schat, 0); return; }
    if (!strcmp(op, "chat_report_ok")) {
        rnet_json_str(m, "mid", c->last_report_ack, sizeof(c->last_report_ack));
        return;
    }
    if (!strcmp(op, "signal")) { on_signal(l, m); return; }
    if (!strcmp(op, "need_mods")) { on_need_mods(l, m); return; }
    if (rnet_lobby__am_handle_op(l, op, m))
        return;
    if (!strcmp(op, "error")) { on_error(l, m); return; }
    if (!strcmp(op, "lobby_closed") || !strcmp(op, "left") ||
        !strcmp(op, "kicked")) {
        if (!strcmp(op, "kicked"))
            copy_str(c->join.last_error, sizeof(c->join.last_error), "kicked");
        drop_room(l);
        {
            /* The join record goes too, but a kick's reason stays readable. */
            char keep[RNET_LOBBY_ERROR_LEN];
            copy_str(keep, sizeof(keep), c->join.last_error);
            memset(&c->join, 0, sizeof(c->join));
            if (!strcmp(op, "kicked"))
                copy_str(c->join.last_error, sizeof(c->join.last_error), keep);
        }
        return;
    }
    if (!strcmp(op, "host_endpoint_ok") || !strcmp(op, "path_report_ok") ||
        !strcmp(op, "pong"))
        return;
    if (!strcmp(op, "mod_xfer_pull") || !strcmp(op, "mod_signal") ||
        !strcmp(op, "mod_xfer_fail")) {
        /* The server-mediated pre-join transfer is for titles whose plan is
         * the seat-gating `mods` key; this client publishes `mod_plan` and
         * transfers over the seated relay instead. */
        LOBBY_DEBUG(l, "ignoring %s (server-mediated transfer not used)", op);
        return;
    }
    LOBBY_DEBUG(l, "unhandled op \"%s\"", op);
}

void rnet_lobby__ingest(RNetLobby *l, const char *json)
{
    if (l && json)
        handle_server_json(l, json);
}

int rnet_lobby__rx_feed(RNetLobby *l, const void *bytes, size_t n)
{
    RNetLobbyConn *c;
    if (!l || (!bytes && n))
        return -1;
    c = &l->c;
    if (n > sizeof(c->rx) - c->rx_len)
        return -1;
    memcpy(c->rx + c->rx_len, bytes, n);
    c->rx_len += n;
    rx_drain(l);
    return c->tx_failed ? 1 : 0;
}
