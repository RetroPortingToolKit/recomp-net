/* lobby_ws_loopback_test -- the lobby client's real transport against a
 * minimal WebSocket server on 127.0.0.1.
 *
 * The protocol suite drives the parsers through a capture seam; this drives
 * everything under them: the connect worker (async and blocking), the HTTP
 * upgrade and its Sec-WebSocket-Accept check, masked client frames, the frame
 * reader, ping -> pong, and the server's close. Loopback only; nothing leaves
 * the machine.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "platform/rnet_platform.h"
#include "recomp_net/lobby_client.h"
#include "recomp_net/rnet_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
typedef SOCKET tsock;
#define TSOCK_BAD INVALID_SOCKET
#define tclose closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int tsock;
#define TSOCK_BAD (-1)
#define tclose close
#endif

static int fails;

static void ck(int cond, const char *what)
{
    if (!cond) {
        printf("    FAIL %s\n", what);
        fails++;
    }
}

/* ── the server ──────────────────────────────────────────────────────────── */

typedef struct Server {
    tsock listener;
    int port;
    int bad_accept;         /* answer with a wrong Sec-WebSocket-Accept */
    int silent;             /* accept TCP, never answer the upgrade */
    int got_hello;
    int got_pong;
    int got_list;
    char hello[512];
    volatile int done;
#if defined(_WIN32)
    HANDLE th;
#else
    pthread_t th;
#endif
} Server;

static int send_all(tsock s, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n) {
        int k = (int)send(s, p, (int)n, 0);
        if (k <= 0)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static int recv_all(tsock s, void *buf, size_t n)
{
    char *p = (char *)buf;
    while (n) {
        int k = (int)recv(s, p, (int)n, 0);
        if (k <= 0)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static int send_frame(tsock s, int opcode, const char *payload, size_t n)
{
    unsigned char h[10];
    size_t hl = 2;
    h[0] = (unsigned char)(0x80 | opcode);
    if (n < 126) {
        h[1] = (unsigned char)n;
    } else if (n < 65536) {
        h[1] = 126;
        h[2] = (unsigned char)(n >> 8);
        h[3] = (unsigned char)n;
        hl = 4;
    } else {
        return -1;
    }
    if (send_all(s, h, hl) != 0)
        return -1;
    return n ? send_all(s, payload, n) : 0;
}

/* One client frame: must be masked (RFC 6455 5.3). Returns the opcode. */
static int read_frame(tsock s, char *out, size_t cap, size_t *len)
{
    unsigned char h[2], mask[4], ext[8];
    size_t n, i;
    if (recv_all(s, h, 2) != 0)
        return -1;
    n = h[1] & 0x7f;
    if (n == 126) {
        if (recv_all(s, ext, 2) != 0)
            return -1;
        n = ((size_t)ext[0] << 8) | ext[1];
    } else if (n == 127) {
        return -1;
    }
    if (!(h[1] & 0x80))
        return -2; /* unmasked client frame: a protocol error */
    if (recv_all(s, mask, 4) != 0 || n + 1 > cap || recv_all(s, out, n) != 0)
        return -1;
    for (i = 0; i < n; ++i)
        out[i] = (char)(out[i] ^ mask[i & 3]);
    out[n] = '\0';
    *len = n;
    return h[0] & 0x0f;
}

static void serve_one(Server *sv)
{
    char req[4096];
    size_t got = 0;
    char *key, *eol, acc[32], resp[256];
    char frame[8192];
    size_t flen;
    int op, i;
    tsock c = accept(sv->listener, NULL, NULL);
    if (c == TSOCK_BAD)
        return;
    if (sv->silent) {
        /* Hold the connection open, answering nothing, until the client
         * gives up and closes it. */
        while (recv(c, req, (int)sizeof(req), 0) > 0) {
        }
        goto out;
    }
    while (got + 1 < sizeof(req)) {
        int k = (int)recv(c, req + got, (int)(sizeof(req) - 1 - got), 0);
        if (k <= 0)
            goto out;
        got += (size_t)k;
        req[got] = '\0';
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    key = strstr(req, "Sec-WebSocket-Key: ");
    if (!key)
        goto out;
    key += 19;
    eol = strstr(key, "\r\n");
    if (!eol)
        goto out;
    *eol = '\0';
    rnet_ws_accept_key(key, acc);
    if (sv->bad_accept)
        acc[0] = acc[0] == 'A' ? 'B' : 'A';
    snprintf(resp, sizeof(resp),
             "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
             "Connection: Upgrade\r\nsec-websocket-accept: %s\r\n\r\n", acc);
    /* The welcome rides in the same segment as the 101: the client must keep
     * the bytes that arrive after the header. */
    {
        const char *w = "{\"op\":\"welcome\",\"player_id\":\"loop-1\",\"ok\":true}";
        char both[512];
        size_t rl = strlen(resp), wl = strlen(w);
        memcpy(both, resp, rl);
        both[rl] = (char)0x81;
        both[rl + 1] = (char)wl;
        memcpy(both + rl + 2, w, wl);
        if (send_all(c, both, rl + 2 + wl) != 0)
            goto out;
    }
    if (sv->bad_accept) {
        /* Hold the socket open: the CLIENT must be the one that walks away,
         * or this case would pass with the accept check deleted. */
        while (recv(c, req, (int)sizeof(req), 0) > 0) {
        }
        goto out;
    }
    /* The client's first frames: hello, list, get_turn_credentials. */
    for (i = 0; i < 3; ++i) {
        op = read_frame(c, frame, sizeof(frame), &flen);
        if (op != 1)
            goto out;
        if (strstr(frame, "\"op\":\"hello\"")) {
            sv->got_hello = 1;
            snprintf(sv->hello, sizeof(sv->hello), "%.500s", frame);
        }
        if (strstr(frame, "\"op\":\"list\""))
            sv->got_list = 1;
    }
    {
        const char *list =
            "{\"op\":\"lobby_list\",\"lobbies\":[{\"lobby_id\":\"L1\",\"name\":\"Loop\","
            "\"game_name\":\"Loop Title\",\"game_version\":\"dev\",\"player_count\":1,"
            "\"max_slots\":2}],\"players\":[]}";
        send_frame(c, 1, list, strlen(list));
    }
    send_frame(c, 9, "abc", 3); /* ping */
    op = read_frame(c, frame, sizeof(frame), &flen);
    if (op == 10 && flen == 3 && !memcmp(frame, "abc", 3))
        sv->got_pong = 1;
    send_frame(c, 8, "", 0); /* close */
out:
    tclose(c);
}

#if defined(_WIN32)
static unsigned __stdcall server_main(void *arg)
#else
static void *server_main(void *arg)
#endif
{
    Server *sv = (Server *)arg;
    serve_one(sv);
    sv->done = 1;
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static int server_start(Server *sv)
{
    struct sockaddr_in a;
#if defined(_WIN32)
    int alen = (int)sizeof(a);
#else
    socklen_t alen = (socklen_t)sizeof(a);
#endif
    rnet_os_startup();
    sv->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (sv->listener == TSOCK_BAD)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(sv->listener, (struct sockaddr *)&a, (socklen_t)sizeof(a)) != 0 ||
        listen(sv->listener, 1) != 0 ||
        getsockname(sv->listener, (struct sockaddr *)&a, &alen) != 0)
        return -1;
    sv->port = ntohs(a.sin_port);
#if defined(_WIN32)
    sv->th = (HANDLE)_beginthreadex(NULL, 0, server_main, sv, 0, NULL);
    return sv->th ? 0 : -1;
#else
    return pthread_create(&sv->th, NULL, server_main, sv) == 0 ? 0 : -1;
#endif
}

static void server_stop(Server *sv)
{
    tclose(sv->listener);
#if defined(_WIN32)
    WaitForSingleObject(sv->th, INFINITE);
    CloseHandle(sv->th);
#else
    pthread_join(sv->th, NULL);
#endif
}

/* ── the client ──────────────────────────────────────────────────────────── */

static char g_errlog[1024];

static void capture_log(void *user, RNetLobbyLogLevel lv, const char *line)
{
    (void)user;
    if (lv >= RNET_LOBBY_LOG_ERROR)
        snprintf(g_errlog, sizeof(g_errlog), "%s", line);
}

static RNetLobby *client_ex(int blocking, int handshake_timeout_ms)
{
    RNetLobbyConfig cfg;
    RNetLobby *l = NULL;
    rnet_lobby_config_init(&cfg);
    cfg.handshake_timeout_ms = handshake_timeout_ms;
    cfg.game_name = "Loop Title";
    cfg.blocking_connect = blocking;
    cfg.waiting_room_rtt = RNET_LOBBY_RTT_OFF;
    cfg.list_latency = 0;
    cfg.lan_beacon = 0;
    cfg.host_advertise = 0;
    cfg.log = capture_log;
    cfg.log_min_level = RNET_LOBBY_LOG_ERROR;
    if (rnet_lobby_open(&l, &cfg) != 0)
        exit(2);
    rnet_lobby_set_display_name(l, "Loopy");
    return l;
}

static RNetLobby *client(int blocking)
{
    return client_ex(blocking, 0);
}

/* Pump until `cond` or ~5 s. */
#define PUMP_UNTIL(l, cond)                                                   \
    do {                                                                      \
        int _i;                                                               \
        for (_i = 0; _i < 500 && !(cond); ++_i) {                             \
            rnet_lobby_pump(l);                                               \
            rnet_os_sleep_micros(10000);                                      \
        }                                                                     \
    } while (0)

static void run_session(int blocking)
{
    Server sv;
    RNetLobby *l = client(blocking);
    char url[64];
    RNetLobbyRow row;
    printf("  %s connect\n", blocking ? "blocking" : "async");
    memset(&sv, 0, sizeof(sv));
    if (server_start(&sv) != 0) {
        ck(0, "loopback server starts");
        rnet_lobby_close(&l);
        return;
    }
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d/", sv.port);
    ck(rnet_lobby_connect(l, url) == 0, "connect returns 0");
    if (!blocking)
        ck(rnet_lobby_connecting(l) || rnet_lobby_connected(l),
           "async connect is in flight (or already done)");
    else
        ck(rnet_lobby_connected(l) == 1, "blocking connect is up on return");
    PUMP_UNTIL(l, rnet_lobby_ready(l));
    ck(rnet_lobby_ready(l) == 1, "the welcome that rode with the 101 is read");
    ck(!strcmp(rnet_lobby_player_id(l), "loop-1"), "player id from welcome");
    PUMP_UNTIL(l, rnet_lobby_list_count(l) > 0);
    ck(rnet_lobby_list_get(l, 0, &row) && !strcmp(row.lobby_id, "L1"),
       "a server frame after the handshake is read");
    PUMP_UNTIL(l, sv.done || !rnet_lobby_connected(l));
    PUMP_UNTIL(l, !rnet_lobby_connected(l));
    ck(sv.got_hello && strstr(sv.hello, "\"display_name\":\"Loopy\"") &&
           strstr(sv.hello, "\"game_name\":\"Loop Title\""),
       "the server read a masked hello");
    ck(sv.got_list, "and the list request");
    ck(sv.got_pong, "a ping is answered with a masked pong echoing its payload");
    ck(rnet_lobby_connected(l) == 0, "the server's close disconnects the client");
    server_stop(&sv);
    rnet_lobby_close(&l);
}

static void run_bad_accept(void)
{
    Server sv;
    RNetLobby *l = client(0);
    char url[64];
    printf("  wrong Sec-WebSocket-Accept\n");
    memset(&sv, 0, sizeof(sv));
    sv.bad_accept = 1;
    if (server_start(&sv) != 0) {
        ck(0, "loopback server starts");
        rnet_lobby_close(&l);
        return;
    }
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d/", sv.port);
    rnet_lobby_connect(l, url);
    g_errlog[0] = '\0';
    {
        int i, was_ready = 0;
        for (i = 0; i < 300; ++i) {
            rnet_lobby_pump(l);
            if (rnet_lobby_ready(l))
                was_ready = 1;
            if (g_errlog[0] && !rnet_lobby_connected(l) && !rnet_lobby_connecting(l))
                break;
            rnet_os_sleep_micros(10000);
        }
        ck(!was_ready, "the welcome behind a wrong accept key is never acted on");
        ck(strstr(g_errlog, "Sec-WebSocket-Accept") != NULL,
           "the client refuses the upgrade itself, and says why");
        ck(rnet_lobby_connected(l) == 0, "and drops the connection");
    }
    rnet_lobby_close(&l);
    server_stop(&sv);
}

static void run_refused(void)
{
    Server sv;
    RNetLobby *l = client(0);
    char url[64];
    int port;
    printf("  refused port\n");
    memset(&sv, 0, sizeof(sv));
    /* Take a port, then free it: nothing listens there. */
    if (server_start(&sv) != 0) {
        ck(0, "loopback server starts");
        rnet_lobby_close(&l);
        return;
    }
    port = sv.port;
    sv.bad_accept = 1;
    {
        /* Unblock the accept so the server thread exits. */
        RNetLobby *poke = client(1);
        snprintf(url, sizeof(url), "ws://127.0.0.1:%d/", port);
        rnet_lobby_connect(poke, url);
        rnet_lobby_close(&poke);
        server_stop(&sv);
    }
    rnet_lobby_connect(l, url);
    PUMP_UNTIL(l, !rnet_lobby_connecting(l));
    ck(rnet_lobby_connecting(l) == 0 && rnet_lobby_connected(l) == 0,
       "a refused connect ends cleanly");
    /* Close with a connect in flight must not block or leak. */
    rnet_lobby_connect(l, "ws://10.255.255.1:9/");
    rnet_lobby_close(&l);
    ck(l == NULL, "close abandons an in-flight connect without waiting");
}

static void run_silent(void)
{
    Server sv;
    RNetLobby *l = client_ex(0, 300);
    char url[64];
    printf("  silent server (handshake timeout)\n");
    memset(&sv, 0, sizeof(sv));
    sv.silent = 1;
    if (server_start(&sv) != 0) {
        ck(0, "loopback server starts");
        rnet_lobby_close(&l);
        return;
    }
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d/", sv.port);
    rnet_lobby_connect(l, url);
    PUMP_UNTIL(l, rnet_lobby_connected(l));
    ck(rnet_lobby_connected(l) == 1, "TCP comes up");
    PUMP_UNTIL(l, !rnet_lobby_connected(l));
    ck(rnet_lobby_connected(l) == 0 && rnet_lobby_ready(l) == 0,
       "an upgrade that never comes is abandoned at the handshake timeout");
    server_stop(&sv);
    rnet_lobby_close(&l);
}

int main(void)
{
    run_session(0);
    run_session(1);
    run_bad_accept();
    run_silent();
    run_refused();
    printf(fails ? "\n%d failure(s)\n" : "\nlobby_ws_loopback_test: all passed\n", fails);
    return fails != 0;
}
