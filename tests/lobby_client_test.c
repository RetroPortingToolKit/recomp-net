/* lobby_client_test -- the shared lobby client against canned server frames.
 *
 * Every inbound frame here has the shape recomp-net-server builds
 * (docs/WS_LOBBY.md, docs/AUTOMATCH.md, src/ws_lobby.rs as of bdd39006);
 * every outbound assertion reads the frame the client handed to the socket
 * seam. No case opens a socket: the handle is attached to a capture sink,
 * and every config toggle that would open one is off.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv */
#endif
#include "recomp_net/lobby_client.h"
#include "lobby/rnet_lobby_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void ck(int cond, const char *what)
{
    if (!cond) {
        printf("    FAIL %s\n", what);
        fails++;
    }
}

/* ── outbound capture ────────────────────────────────────────────────────── */

#define MAX_TX 64
static char *g_tx[MAX_TX];
static int g_tx_n;

static void tx_clear(void)
{
    int i;
    for (i = 0; i < g_tx_n; ++i)
        free(g_tx[i]);
    g_tx_n = 0;
}

static void tx_sink(void *user, const char *frame)
{
    size_t n;
    (void)user;
    if (g_tx_n >= MAX_TX)
        return;
    n = strlen(frame);
    g_tx[g_tx_n] = (char *)malloc(n + 1);
    memcpy(g_tx[g_tx_n], frame, n + 1);
    g_tx_n++;
}

/* The last frame whose op is `op`, or NULL. */
static const char *tx_op(const char *op)
{
    int i;
    char want[64];
    snprintf(want, sizeof(want), "\"op\":\"%s\"", op);
    for (i = g_tx_n - 1; i >= 0; --i)
        if (strstr(g_tx[i], want))
            return g_tx[i];
    return NULL;
}

static int tx_count(const char *op)
{
    int i, n = 0;
    char want[64];
    snprintf(want, sizeof(want), "\"op\":\"%s\"", op);
    for (i = 0; i < g_tx_n; ++i)
        if (strstr(g_tx[i], want))
            ++n;
    return n;
}

/* ── handles ─────────────────────────────────────────────────────────────── */

static void quiet_cfg(RNetLobbyConfig *cfg)
{
    rnet_lobby_config_init(cfg);
    cfg->game_name = "Test Title";
    cfg->game_version = "1.2.0";
    cfg->platform = "test";
    cfg->max_players = 4;
    cfg->max_spectators = 4;
    cfg->waiting_room_rtt = RNET_LOBBY_RTT_OFF;
    cfg->list_latency = 0;
    cfg->lan_beacon = 0;
    cfg->host_advertise = 0;
    cfg->log_min_level = RNET_LOBBY_LOG_ERROR + 1;
}

static RNetLobby *open_with(const RNetLobbyConfig *cfg, const char *pid)
{
    RNetLobby *l = NULL;
    if (rnet_lobby_open(&l, cfg) != 0 || !l) {
        printf("could not open a lobby handle\n");
        exit(2);
    }
    tx_clear();
    rnet_lobby__test_attach(l, pid, tx_sink, NULL);
    return l;
}

static RNetLobby *open_default(const char *pid)
{
    RNetLobbyConfig cfg;
    quiet_cfg(&cfg);
    return open_with(&cfg, pid);
}

/* The server's `joined` carries no seat arrays, host id or counts
 * (ws_lobby.rs: lobby_id, session_id, local_slot, spectator,
 * spectator_slot_base, endpoints, match_caps); the lobby_update that
 * follows does. Frames here follow that shape. */
static void seat_in(RNetLobby *l, const char *joined_extra, const char *host,
                    const char *slots, const char *spectators)
{
    char j[1024], u[2048];
    snprintf(j, sizeof(j),
             "{\"op\":\"joined\",\"ok\":true,\"lobby_id\":\"L\",\"session_id\":4,"
             "\"spectator_slot_base\":64%s%s}",
             joined_extra && joined_extra[0] ? "," : "", joined_extra ? joined_extra : "");
    rnet_lobby__ingest(l, j);
    snprintf(u, sizeof(u),
             "{\"op\":\"lobby_update\",\"lobby_id\":\"L\",\"session_id\":4,"
             "\"player_count\":2,\"max_slots\":2,\"host_player_id\":\"%s\","
             "\"all_ready\":false,\"spectator_slot_base\":64,\"slots\":[%s],"
             "\"spectators\":[%s]}",
             host, slots, spectators ? spectators : "");
    rnet_lobby__ingest(l, u);
}

#define SLOTS_HG "{\"slot\":0,\"player_id\":\"h\"},{\"slot\":1,\"player_id\":\"g\"}"

/* ── config / identity ───────────────────────────────────────────────────── */

static void case_config_defaults(void)
{
    RNetLobbyConfig cfg;
    RNetLobby *l = NULL;
    printf("  config defaults\n");
    rnet_lobby_config_init(&cfg);
    ck(cfg.max_players == 2 && cfg.max_spectators == 4, "seat defaults");
    ck(cfg.spectator_slot_base == 64, "gallery base default");
    ck(cfg.host_port == 7777 && cfg.guest_port == 7778, "port defaults");
    ck(cfg.waiting_room_rtt == RNET_LOBBY_RTT_PEER_PATH, "latency default");
    ck(cfg.fingerprint_in_rooms == 1 && cfg.host_bind_all_interfaces == 1,
       "policy defaults");
    cfg.max_players = 99;
    cfg.max_spectators = 99;
    cfg.log_min_level = RNET_LOBBY_LOG_ERROR + 1;
    ck(rnet_lobby_open(&l, &cfg) == 0 && l, "open");
    ck(l->cfg.max_players == RNET_LOBBY_MAX_PLAYERS, "max_players clamps to the table");
    ck(l->cfg.max_spectators == RNET_LOBBY_MAX_SPECTATORS, "max_spectators clamps");
    ck(!strcmp(rnet_lobby_default_url(l), RNET_LOBBY_DEFAULT_URL),
       "default URL is ws://netplay.retcomm.net:8765");
    ck(!strcmp(rnet_lobby_game_version(l), "dev"), "no version -> dev");
    ck(rnet_lobby_connected(l) == 0 && rnet_lobby_connecting(l) == 0,
       "open does not connect");
    ck(rnet_lobby_connect(l, "wss://secure.example/") == -1, "wss:// is refused (no TLS)");
    ck(rnet_lobby_connect(l, "ws://:99999/") == -1, "a bad URL is refused");
    rnet_lobby_close(&l);
    ck(rnet_lobby_open(NULL, &cfg) < 0, "open(NULL) refuses");
}

static void case_env_overrides(void)
{
    RNetLobbyConfig cfg;
    RNetLobby *l;
    printf("  env overrides\n");
    quiet_cfg(&cfg);
    cfg.url_env_var = "RNET_LOBBY_TEST_URL";
    cfg.version_env_var = "RNET_LOBBY_TEST_VERSION";
    cfg.default_url = "ws://configured.example:9000/ws";
#if defined(_WIN32)
    _putenv("RNET_LOBBY_TEST_URL=");
    _putenv("RNET_LOBBY_TEST_VERSION=");
#else
    unsetenv("RNET_LOBBY_TEST_URL");
    unsetenv("RNET_LOBBY_TEST_VERSION");
#endif
    l = open_with(&cfg, "me");
    ck(!strcmp(rnet_lobby_default_url(l), "ws://configured.example:9000/ws"),
       "the configured URL is the default");
    ck(!strcmp(rnet_lobby_game_version(l), "1.2.0"), "the configured pin is presented");
    ck(rnet_lobby_version_filter_strict(l) == 1, "a release pin filters the list");
#if defined(_WIN32)
    _putenv("RNET_LOBBY_TEST_URL=ws://env.example:1234");
    _putenv("RNET_LOBBY_TEST_VERSION=forced-9");
#else
    setenv("RNET_LOBBY_TEST_URL", "ws://env.example:1234", 1);
    setenv("RNET_LOBBY_TEST_VERSION", "forced-9", 1);
#endif
    ck(!strcmp(rnet_lobby_default_url(l), "ws://env.example:1234"),
       "the consumer's env var overrides the URL");
    rnet_lobby_set_game_identity(l, "Test Title", "1.2.0");
    ck(!strcmp(rnet_lobby_game_version(l), "forced-9"),
       "the version env var forces the presented pin");
    ck(rnet_lobby_version_filter_strict(l) == 0,
       "a forced pin lists unfiltered (the one-machine mistake)");
#if defined(_WIN32)
    _putenv("RNET_LOBBY_TEST_URL=");
    _putenv("RNET_LOBBY_TEST_VERSION=");
#else
    unsetenv("RNET_LOBBY_TEST_URL");
    unsetenv("RNET_LOBBY_TEST_VERSION");
#endif
    rnet_lobby_close(&l);
    tx_clear();
}

static void case_version_rule(void)
{
    printf("  release rule\n");
    ck(rnet_lobby_version_is_release("0.1.5") == 1, "a clean release filters");
    ck(rnet_lobby_version_is_release("dev") == 0, "dev lists everything");
    ck(rnet_lobby_version_is_release("dev+abc12345") == 0, "a dev qualifier too");
    ck(rnet_lobby_version_is_release("0.1.5+abc12345-dirty.1234abcd") == 0,
       "a Release-type build of a dirty tree is NOT a release");
    ck(rnet_lobby_version_is_release("") == 0 && rnet_lobby_version_is_release(NULL) == 0,
       "empty is not a release");
}

static void case_fingerprint(void)
{
    RNetLobby *l = open_default("me");
    printf("  fingerprint\n");
    rnet_lobby_set_fp(l, "00112233445566778899AABBCCDDEEFF00112233445566778899aabbccddeeff");
    ck(!strcmp(rnet_lobby_fp(l),
               "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"),
       "upper case folds to the wire's lower case");
    rnet_lobby_set_fp(l, "xyz");
    ck(rnet_lobby_fp(l)[0] == '\0', "a malformed fingerprint clears rather than lies");
    rnet_lobby_set_fp(l, "0011223344556677889900112233445566778899001122334455667788990g");
    ck(rnet_lobby_fp(l)[0] == '\0', "a non-hex character clears");
    rnet_lobby_close(&l);
}

/* ── welcome / hello / names ─────────────────────────────────────────────── */

static void case_welcome(void)
{
    RNetLobby *l = open_default("");
    printf("  welcome\n");
    rnet_lobby_set_display_name(l, "Al\"ex");
    rnet_lobby_set_blocks(l, "acct-one;acct-two;bad\"id;");
    tx_clear();
    rnet_lobby__ingest(l, "{\"op\":\"welcome\",\"player_id\":\"p-123\",\"ok\":true}");
    ck(!strcmp(rnet_lobby_player_id(l), "p-123"), "player_id from welcome");
    ck(rnet_lobby_ready(l) == 1, "ready after welcome");
    ck(tx_op("hello") &&
           strstr(tx_op("hello"), "\"display_name\":\"Al\\\"ex\"") &&
           strstr(tx_op("hello"), "\"game_name\":\"Test Title\""),
       "hello names us (escaped) and our title");
    ck(tx_op("hello") && !strstr(tx_op("hello"), "\"session\""),
       "a guest's hello carries no session");
    ck(tx_op("list") != NULL, "a list request follows");
    ck(tx_op("list") && strstr(tx_op("list"), "\"game_version\":\"1.2.0\""),
       "a release build lists filtered by its pin");
    ck(tx_op("get_turn_credentials") != NULL, "TURN credentials are prefetched");
    ck(tx_op("set_blocks") &&
           strstr(tx_op("set_blocks"), "\"accounts\":[\"acct-one\",\"acct-two\"]"),
       "the block set is re-sent on every new connection, bad ids dropped");

    tx_clear();
    rnet_lobby_set_display_name(l, "Alex");
    ck(tx_count("hello") == 1, "a rename re-sends hello");
    rnet_lobby_set_display_name(l, "Alex");
    ck(tx_count("hello") == 1, "an unchanged name does not");
    rnet_lobby__ingest(l, "{\"op\":\"hello_ok\",\"ok\":true,\"display_name\":\"Alex (2)\"}");
    ck(!strcmp(rnet_lobby_accepted_name(l), "Alex (2)"), "the accepted name is kept");
    rnet_lobby__ingest(l, "{\"op\":\"error\",\"code\":\"name_rejected\",\"ok\":false}");
    {
        char code[32];
        ck(rnet_lobby_name_refused(l, code, sizeof(code)) == 1 &&
               !strcmp(code, "name_rejected"),
           "a refused name is reported with its code");
    }
    rnet_lobby_set_display_name(l, "Someone Else");
    ck(rnet_lobby_name_refused(l, NULL, 0) == 0, "renaming clears the refusal");
    rnet_lobby_clear_last_error(l);
    rnet_lobby__ingest(l, "{\"op\":\"error\",\"code\":\"session_invalid\",\"ok\":false}");
    ck(rnet_lobby_session_invalid(l) == 1, "a rejected session is reported");
    ck(rnet_lobby_join_info(l)->last_error[0] == '\0',
       "and is not mistaken for a room failure");
    rnet_lobby_close(&l);
}

static const char *g_session = "tok.en";
static const char *test_session(void *u)
{
    (void)u;
    return g_session;
}

static void case_utf8_names(void)
{
    RNetLobby *l = open_default("me");
    char name[256];
    const char *dn;
    size_t n, i;
    printf("  UTF-8 truncation\n");
    name[0] = '\0';
    for (i = 0; i < 30; ++i)
        strcat(name, "\xf0\x9f\x98\x80"); /* 120 bytes of 4-byte code points */
    rnet_lobby_set_display_name(l, name);
    dn = rnet_lobby_display_name(l);
    n = strlen(dn);
    ck(n > 0 && n < RNET_LOBBY_NAME_LEN && n % 4 == 0,
       "a name truncated to the field ends on a code point boundary");
    tx_clear();
    rnet_lobby__ingest(l, "{\"op\":\"welcome\",\"player_id\":\"me\",\"ok\":true}");
    ck(tx_op("hello") && strstr(tx_op("hello"), dn) != NULL,
       "and goes out whole in hello");
    rnet_lobby_close(&l);
}

static void case_session_hello(void)
{
    RNetLobbyConfig cfg;
    RNetLobby *l;
    printf("  session hello\n");
    quiet_cfg(&cfg);
    cfg.session = test_session;
    l = open_with(&cfg, "");
    rnet_lobby__ingest(l, "{\"op\":\"welcome\",\"player_id\":\"p\",\"ok\":true}");
    ck(tx_op("hello") && strstr(tx_op("hello"), "\"session\":\"tok.en\""),
       "a signed-in hello carries the session from the consumer's supplier");
    rnet_lobby_close(&l);
}

/* ── lobby list ──────────────────────────────────────────────────────────── */

static const char *k_list =
    "{\"op\":\"lobby_list\",\"lobbies\":["
    "{\"lobby_id\":\"L1\",\"name\":\"Friday Fights\",\"game_name\":\"Test Title\","
    "\"game_version\":\"1.2.0\",\"player_count\":1,\"max_slots\":2,"
    "\"has_password\":true,\"lobby_kind\":1,\"host_endpoint\":\"203.0.113.10:7777\","
    "\"lan_endpoints\":[\"192.168.1.42:7777\",\"10.0.0.5:7777\"],"
    "\"host_country\":\"JP\",\"allow_spectators\":true,\"max_spectators\":4,"
    "\"spectator_count\":1},"
    "{\"lobby_id\":\"L2\",\"name\":\"Other game\",\"game_name\":\"Another Title\","
    "\"game_version\":\"1.2.0\",\"player_count\":1,\"max_slots\":2},"
    "{\"lobby_id\":\"L3\",\"name\":\"Old build\",\"game_name\":\"Test Title\","
    "\"game_version\":\"1.1.0\",\"player_count\":1,\"max_slots\":2},"
    "{\"lobby_id\":\"L4\",\"name\":\"Old server\",\"game_name\":\"Test Title\","
    "\"player_count\":2,\"max_slots\":4}],"
    "\"players\":["
    "{\"display_name\":\"Marisa\",\"country\":\"JP\",\"lobby_id\":\"L1\","
    "\"lobby_name\":\"Friday Fights\",\"hosting\":true,\"tag\":\"3f9a1c02\","
    "\"account\":\"acct-m\",\"game_name\":\"Test Title\"},"
    "{\"display_name\":\"Reimu\",\"country\":\"DE\",\"lobby_id\":\"\","
    "\"lobby_name\":\"\",\"hosting\":false,\"tag\":\"b71e40d9\",\"account\":\"\","
    "\"game_name\":\"Another Title\"},"
    "{\"display_name\":\"Sanae\",\"country\":\"\",\"lobby_id\":\"\","
    "\"lobby_name\":\"\",\"hosting\":false,\"tag\":\"c0ffee00\",\"account\":\"\","
    "\"game_name\":\"\"},"
    "{\"display_name\":\"\",\"tag\":\"nameless\"}]}";

static void case_list(void)
{
    RNetLobby *l = open_default("me");
    RNetLobbyRow r;
    RNetLobbyOnlinePlayer p;
    printf("  lobby list\n");
    rnet_lobby__ingest(l, k_list);
    ck(rnet_lobby_list_count(l) == 2, "other titles and other releases are dropped");
    ck(rnet_lobby_list_get(l, 0, &r) && !strcmp(r.lobby_id, "L1"), "row 0 is L1");
    ck(!strcmp(r.name, "Friday Fights") && r.player_count == 1 && r.max_slots == 2,
       "row fields");
    ck(r.has_password == 1 && r.lobby_kind == 1 && !strcmp(r.host_country, "JP"),
       "password, kind, country");
    ck(r.allow_spectators == 1 && r.max_spectators == 4 && r.spectator_count == 1,
       "gallery columns");
    ck(!strcmp(r.host_endpoint, "203.0.113.10:7777"), "host endpoint");
    ck(r.lan_count == 2 && !strcmp(r.lan_endpoints[1], "10.0.0.5:7777"),
       "legacy lan_endpoints");
    ck(r.latency_ms == -1, "latency unknown until probed");
    ck(rnet_lobby_list_get(l, 1, &r) && !strcmp(r.lobby_id, "L4") &&
           !strcmp(r.game_version, "dev"),
       "a row with NO version is kept (old server) and shown as dev");
    ck(!rnet_lobby_list_get(l, 2, &r), "past the end is refused");
    ck(rnet_lobby_online_count(l) == 2, "players of other titles and nameless rows drop");
    ck(rnet_lobby_online_get(l, 0, &p) && !strcmp(p.display_name, "Marisa") &&
           p.hosting == 1 && !strcmp(p.account, "acct-m") && !strcmp(p.tag, "3f9a1c02"),
       "online row fields");
    ck(rnet_lobby_online_get(l, 1, &p) && !strcmp(p.display_name, "Sanae"),
       "a player with no title yet is kept");

    /* A dev build lists everything of its title. */
    rnet_lobby_set_game_identity(l, "Test Title", "dev+abc");
    rnet_lobby__ingest(l, k_list);
    ck(rnet_lobby_list_count(l) == 3, "a dev build sees other releases of its title");
    tx_clear();
    rnet_lobby_request_list(l);
    ck(tx_op("list") && !strstr(tx_op("list"), "game_version"),
       "and asks for its title only");
    rnet_lobby_close(&l);
}

static void case_big_list_frames(void)
{
    /* The engine copies held 4 KB of receive buffer; a busy hub's list is
     * bigger than that and disconnected them. Frames of 64 KB+ (16-bit and
     * 64-bit lengths), fragmentation, masking tolerance, close. */
    RNetLobby *l = open_default("me");
    static char json[70000];
    static unsigned char frame[70100];
    size_t o = 0, n, h;
    int i, rc;
    printf("  large and fragmented frames\n");
    o += (size_t)snprintf(json + o, sizeof(json) - o, "{\"op\":\"lobby_list\",\"lobbies\":[");
    for (i = 0; i < 32; ++i)
        o += (size_t)snprintf(json + o, sizeof(json) - o,
                              "%s{\"lobby_id\":\"L%02d\",\"name\":\"Room %d\","
                              "\"game_name\":\"Test Title\",\"game_version\":\"1.2.0\","
                              "\"player_count\":1,\"max_slots\":2}",
                              i ? "," : "", i, i);
    o += (size_t)snprintf(json + o, sizeof(json) - o, "],\"players\":[");
    for (i = 0; o < 66000; ++i)
        o += (size_t)snprintf(json + o, sizeof(json) - o,
                              "%s{\"display_name\":\"Player %d\",\"tag\":\"t%d\","
                              "\"game_name\":\"Test Title\"}",
                              i ? "," : "", i, i);
    o += (size_t)snprintf(json + o, sizeof(json) - o, "]}");
    n = o;
    /* One 64-bit-length text frame. */
    frame[0] = 0x81;
    frame[1] = 127;
    for (i = 0; i < 8; ++i)
        frame[2 + i] = (unsigned char)((uint64_t)n >> (56 - 8 * i));
    h = 10;
    memcpy(frame + h, json, n);
    rc = rnet_lobby__rx_feed(l, frame, h + n);
    ck(rc == 0, "a 64 KB+ frame is read, not fatal");
    ck(rnet_lobby_list_count(l) == 32, "all 32 rooms parse from the big frame");
    ck(rnet_lobby_online_count(l) == RNET_LOBBY_MAX_ONLINE, "players fill the table");

    /* Fragmented + split across feeds, second fragment masked. */
    {
        const char *a = "{\"op\":\"hello_ok\",\"display_";
        const char *b = "name\":\"Frag\"}";
        unsigned char f1[64], f2[64];
        size_t la = strlen(a), lb = strlen(b), k;
        const unsigned char mask[4] = { 1, 2, 3, 4 };
        f1[0] = 0x01; /* text, not FIN */
        f1[1] = (unsigned char)la;
        memcpy(f1 + 2, a, la);
        f2[0] = 0x80; /* continuation, FIN */
        f2[1] = (unsigned char)(0x80 | lb);
        memcpy(f2 + 2, mask, 4);
        for (k = 0; k < lb; ++k)
            f2[6 + k] = (unsigned char)(b[k] ^ mask[k & 3]);
        rc = rnet_lobby__rx_feed(l, f1, 5);
        rc |= rnet_lobby__rx_feed(l, f1 + 5, 2 + la - 5);
        ck(rnet_lobby_accepted_name(l)[0] == '\0', "nothing dispatched mid-message");
        rc |= rnet_lobby__rx_feed(l, f2, 6 + lb);
        ck(rc == 0 && !strcmp(rnet_lobby_accepted_name(l), "Frag"),
           "fragments reassemble, split feeds and a masked frame included");
    }
    /* A ping in between is answered on a real socket; here it is simply not
     * dispatched as a message. A close is fatal. */
    {
        unsigned char ping[4] = { 0x89, 2, 'h', 'i' };
        unsigned char close_f[2] = { 0x88, 0 };
        ck(rnet_lobby__rx_feed(l, ping, sizeof(ping)) == 0, "ping is not fatal");
        ck(rnet_lobby__rx_feed(l, close_f, sizeof(close_f)) == 1, "close is");
    }
    rnet_lobby_close(&l);
}

/* ── rooms ───────────────────────────────────────────────────────────────── */

static void case_create_frames(void)
{
    RNetLobbyConfig cfg;
    RNetLobby *l;
    RNetLobbyMatchCaps caps;
    const char *f;
    char longpw[200];
    printf("  create / join frames\n");
    quiet_cfg(&cfg);
    cfg.content_fp = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    l = open_with(&cfg, "me");
    rnet_lobby_set_display_name(l, "Host");
    rnet_lobby_set_allow_spectators(l, 1);
    rnet_lobby_match_caps_init(l, &caps);
    caps.valid = 1;
    caps.input_delay = 3;
    snprintf(caps.game_json, sizeof(caps.game_json), "\"widescreen\":true,\"ws_extra\":8");
    tx_clear();
    ck(rnet_lobby_create(l, "Room \"1\"", NULL, NULL, "pw", NULL, &caps, 9) == 0,
       "create sent");
    f = tx_op("create");
    ck(f && strstr(f, "\"name\":\"Room \\\"1\\\"\""), "room name escaped");
    ck(f && strstr(f, "\"max_slots\":4"), "max_slots clamps to the title's players");
    ck(f && strstr(f, "\"allow_spectators\":true"), "the gallery preference rides along");
    ck(f && strstr(f, "\"host_bind\":\"0.0.0.0:7777\""), "default host bind");
    ck(f && strstr(f, "\"disc_fp\":\"0011"), "the fingerprint is sent (fingerprint_in_rooms)");
    ck(f && strstr(f, "\"match_caps\":{\"v\":1,\"input_delay\":3"), "caps attached");
    ck(f && strstr(f, "\"widescreen\":true,\"ws_extra\":8}"),
       "the title's members are carried verbatim");
    ck(rnet_lobby_match_caps(l)->valid && rnet_lobby_match_caps(l)->input_delay == 3,
       "the host adopts its own caps immediately");
    memset(longpw, 'p', sizeof(longpw) - 1);
    longpw[sizeof(longpw) - 1] = '\0';
    ck(rnet_lobby_create(l, "x", NULL, NULL, longpw, NULL, NULL, 2) < 0,
       "an over-long password is refused locally");
    ck(!strcmp(rnet_lobby_join_info(l)->last_error, "password_invalid"),
       "with the server's own code");
    tx_clear();
    ck(rnet_lobby_join(l, "L1", NULL, "0.0.0.0:7790") == 0, "join sent");
    f = tx_op("join");
    ck(f && strstr(f, "\"guest_bind\":\"0.0.0.0:7790\""), "explicit guest bind kept");
    ck(f && strstr(f, "\"game_version\":\"1.2.0\"") && strstr(f, "\"disc_fp\":\"0011"),
       "join carries the pin and the fingerprint");
    ck(f && !strstr(f, "mod_offer"), "no supplier, no offer");
    rnet_lobby_close(&l);

    quiet_cfg(&cfg);
    cfg.content_fp = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    cfg.fingerprint_in_rooms = 0;
    l = open_with(&cfg, "me");
    rnet_lobby_create(l, "Room", NULL, NULL, NULL, "192.168.1.5:7000", NULL, 0);
    ck(tx_op("create") && !strstr(tx_op("create"), "disc_fp"),
       "fingerprint_in_rooms=0 keeps the historic snesrecomp wire");
    ck(tx_op("create") && strstr(tx_op("create"), "\"max_slots\":2"),
       "max_slots 0 takes the default");
    rnet_lobby_close(&l);
}

static void case_created_and_binds(void)
{
    RNetLobbyConfig cfg;
    RNetLobby *l;
    RNetLobbyJoinInfo ji;
    printf("  created / binds\n");
    quiet_cfg(&cfg);
    cfg.auto_ready = 1;
    l = open_with(&cfg, "h");
    rnet_lobby_create(l, "Room", NULL, NULL, NULL, "192.168.1.5:7000", NULL, 2);
    tx_clear();
    rnet_lobby__ingest(l,
        "{\"op\":\"created\",\"ok\":true,\"lobby_id\":\"L9\",\"session_id\":1,"
        "\"local_slot\":0,\"host_endpoint\":\"203.0.113.9:7000\","
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"Host\"}]}");
    ck(rnet_lobby_in_lobby(l) && rnet_lobby_is_host(l), "created seats us as host");
    ck(!strcmp(rnet_lobby_host_player_id(l), "h"), "host id defaults to ours");
    ck(!strcmp(rnet_lobby_join_info(l)->bind_hostport, "0.0.0.0:7000"),
       "the host listens on every interface at its port");
    ck(tx_op("set_ready") != NULL, "auto_ready arms Ready on create");
    tx_clear();
    rnet_lobby__ingest(l,
        "{\"op\":\"lobby_update\",\"lobby_id\":\"L9\",\"session_id\":1,"
        "\"host_endpoint\":\"203.0.113.9:7000\",\"guest_endpoint\":\"198.51.100.2:0\","
        "\"player_count\":2,\"max_slots\":2,\"host_player_id\":\"h\",\"all_ready\":false,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"Host\",\"ready\":false},"
        "{\"slot\":1,\"player_id\":\"g\",\"account\":\"acct-g\",\"display_name\":\"Guest\","
        "\"ready\":false,\"country\":\"DE\",\"bios_offer\":{\"v\":1,\"prefer\":\"openbios\"}}]}");
    ck(rnet_lobby_join_info(l)->peer_hostport[0] == '\0',
       "a guest endpoint with port 0 leaves the host's peer empty (accept first)");
    ck(tx_op("set_ready") != NULL, "an update that cleared Ready re-arms it");
    {
        RNetLobbyMember m;
        const char *raw;
        char prefer[16];
        ck(rnet_lobby_member_get(l, 1, &m) && !strcmp(m.account, "acct-g") &&
               !strcmp(m.country, "DE"),
           "member account and country");
        raw = rnet_lobby_member_json(l, 1);
        ck(raw && rnet_lobby_json_get_raw(raw, "bios_offer", prefer, sizeof(prefer)) == 0,
           "a title-owned object bigger than the buffer is refused whole");
        {
            char obj[128];
            ck(raw && rnet_lobby_json_get_raw(raw, "bios_offer", obj, sizeof(obj)) == 1 &&
                   rnet_lobby_json_get_str(obj, "prefer", prefer, sizeof(prefer)) == 1 &&
                   !strcmp(prefer, "openbios"),
               "the title reads its own ready extras from the raw row");
        }
        ck(rnet_lobby_member_is_host(l, &m) == 0, "the guest row is not the host");
        ck(rnet_lobby_member_get(l, 0, &m) && rnet_lobby_member_is_host(l, &m) == 1,
           "the host row is");
    }
    /* Three seats without the relay: host hub, guests ephemeral. */
    rnet_lobby__ingest(l,
        "{\"op\":\"lobby_update\",\"player_count\":3,\"max_slots\":3,"
        "\"host_endpoint\":\"203.0.113.9:7000\",\"guest_endpoint\":\"198.51.100.2:7778\","
        "\"host_player_id\":\"h\",\"slots\":["
        "{\"slot\":0,\"player_id\":\"h\"},{\"slot\":1,\"player_id\":\"g\"},"
        "{\"slot\":2,\"player_id\":\"g2\"}]}");
    ck(rnet_lobby_join_info(l)->peer_hostport[0] == '\0',
       "a 3-seat host is a hub (no single peer)");
    /* Host migration. */
    rnet_lobby__ingest(l,
        "{\"op\":\"lobby_update\",\"player_count\":2,\"max_slots\":2,"
        "\"host_player_id\":\"g\",\"slots\":[{\"slot\":0,\"player_id\":\"g\"},"
        "{\"slot\":1,\"player_id\":\"h\"}]}");
    ck(rnet_lobby_is_host(l) == 0, "a host_player_id naming someone else demotes us");
    ck(rnet_lobby_try_fill_launch(l, &ji) == 0, "no launch pending, nothing to fill");
    rnet_lobby_close(&l);
}

static void case_joined_guest(void)
{
    RNetLobby *l = open_default("g");
    RNetLobbyJoinInfo ji;
    printf("  joined / launch (guest)\n");
    rnet_lobby_join(l, "L9", "pw", "0.0.0.0:7790");
    rnet_lobby__ingest(l,
        "{\"op\":\"joined\",\"ok\":true,\"lobby_id\":\"L9\",\"session_id\":4,"
        "\"local_slot\":1,\"spectator\":false,\"spectator_slot_base\":64,"
        "\"host_endpoint\":\"203.0.113.9:7000\","
        "\"guest_endpoint\":\"198.51.100.2:7790\","
        "\"match_caps\":{\"v\":1,\"input_delay\":25,\"rollback\":false,"
        "\"widescreen\":true,\"mod_plan\":[]}}");
    ck(rnet_lobby_join_info(l)->local_slot == 1 && !rnet_lobby_local_is_spectator(l),
       "joined alone gives our seat and role");
    rnet_lobby__ingest(l,
        "{\"op\":\"lobby_update\",\"lobby_id\":\"L9\",\"session_id\":4,"
        "\"host_endpoint\":\"203.0.113.9:7000\",\"guest_endpoint\":\"198.51.100.2:7790\","
        "\"player_count\":2,\"max_slots\":2,\"host_player_id\":\"h\","
        "\"match_caps\":{\"v\":1,\"input_delay\":25,\"rollback\":false,"
        "\"widescreen\":true,\"mod_plan\":[]},"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\"},{\"slot\":1,\"player_id\":\"g\"}]}");
    ck(rnet_lobby_in_lobby(l) && !rnet_lobby_is_host(l), "joined seats us as a guest");
    ck(rnet_lobby_join_info(l)->local_slot == 1, "our seat");
    ck(!strcmp(rnet_lobby_join_info(l)->bind_hostport, "0.0.0.0:7790") &&
           !strcmp(rnet_lobby_join_info(l)->peer_hostport, "203.0.113.9:7000"),
       "a 2-seat guest binds its advertised port and dials the host");
    ck(rnet_lobby_match_caps(l)->input_delay == 20, "input_delay clamps to the config max");
    ck(rnet_lobby_match_caps(l)->rollback == 0, "rollback read");
    ck(strstr(rnet_lobby_match_caps(l)->game_json, "\"widescreen\":true") != NULL,
       "the title's key lands in game_json");
    rnet_lobby__ingest(l,
        "{\"op\":\"launch\",\"ok\":true,\"lobby_id\":\"L9\",\"session_id\":5,"
        "\"host_endpoint\":\"203.0.113.9:7000\",\"guest_endpoint\":\"198.51.100.2:7790\","
        "\"transport\":\"ice_p2p\",\"player_count\":2,\"max_slots\":2,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\"},{\"slot\":1,\"player_id\":\"g\"}]}");
    ck(rnet_lobby_launch_pending(l) == 1, "a p2p launch is accepted by default");
    ck(rnet_lobby_try_fill_launch(l, &ji) == 1 && ji.session_id == 5 &&
           ji.force_input_relay == 0 && !strcmp(ji.transport, "ice_p2p"),
       "try_fill_launch copies the new session and transport");
    rnet_lobby_clear_launch_pending(l);
    ck(rnet_lobby_launch_pending(l) == 0, "clear_launch_pending");
    rnet_lobby__ingest(l, "{\"op\":\"kicked\",\"ok\":true,\"lobby_id\":\"L9\"}");
    ck(!rnet_lobby_in_lobby(l) && rnet_lobby_member_count(l) == 0, "kicked drops the room");
    ck(!strcmp(rnet_lobby_join_info(l)->last_error, "kicked"), "and says why");
    ck(rnet_lobby_match_caps(l)->valid == 0, "the room's caps go with it");
    rnet_lobby_close(&l);
}

static void case_relay_policy(void)
{
    RNetLobbyConfig cfg;
    RNetLobby *l;
    printf("  relay policy\n");
    quiet_cfg(&cfg);
    cfg.require_server_relay = 1;
    l = open_with(&cfg, "g");
    rnet_lobby_join(l, "L9", NULL, "0.0.0.0:7790");
    seat_in(l, "\"local_slot\":1,\"spectator\":false,"
               "\"host_endpoint\":\"203.0.113.9:7000\"", "h", SLOTS_HG, NULL);
    rnet_lobby__ingest(l,
        "{\"op\":\"launch\",\"ok\":true,\"host_endpoint\":\"203.0.113.9:7000\","
        "\"guest_endpoint\":\"198.51.100.2:7790\",\"transport\":\"ice_p2p\","
        "\"player_count\":2,\"max_slots\":2,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\"},{\"slot\":1,\"player_id\":\"g\"}]}");
    ck(rnet_lobby_launch_pending(l) == 0 &&
           !strcmp(rnet_lobby_join_info(l)->last_error, "sfu_required"),
       "require_server_relay refuses an ice_p2p launch");
    /* A loopback relay advertise rewritten to the lobby's WS peer. */
    snprintf(l->c.peer_ip, sizeof(l->c.peer_ip), "192.168.1.20");
    rnet_lobby__ingest(l,
        "{\"op\":\"launch\",\"ok\":true,\"relay_endpoint\":\"127.0.0.1:8777\","
        "\"transport\":\"sfu\",\"player_count\":2,\"max_slots\":2,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\"},{\"slot\":1,\"player_id\":\"g\"}]}");
    ck(rnet_lobby_launch_pending(l) == 1, "an SFU launch is accepted");
    ck(!strcmp(rnet_lobby_join_info(l)->peer_hostport, "192.168.1.20:8777"),
       "a loopback relay advertise is rewritten to the private WS peer");
    /* An RFC1918 advertise is the server's LAN pick: kept. */
    rnet_lobby__ingest(l,
        "{\"op\":\"launch\",\"ok\":true,\"relay_endpoint\":\"10.1.2.3:8777\","
        "\"transport\":\"sfu\",\"player_count\":2,\"max_slots\":2,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\"},{\"slot\":1,\"player_id\":\"g\"}]}");
    ck(!strcmp(rnet_lobby_join_info(l)->peer_hostport, "10.1.2.3:8777"),
       "a private relay advertise is never clobbered");
    /* Spectator wire slot. */
    rnet_lobby__ingest(l,
        "{\"op\":\"launch\",\"ok\":true,\"relay_endpoint\":\"10.1.2.3:8777\","
        "\"transport\":\"sfu\",\"player_count\":2,\"max_slots\":2,"
        "\"spectator_relay_base\":2,\"spectator_slot_base\":64,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\"},{\"slot\":1,\"player_id\":\"x\"}],"
        "\"spectators\":[{\"slot\":65,\"player_id\":\"g\"}]}");
    ck(rnet_lobby_local_is_spectator(l) == 1 && rnet_lobby_local_wire_slot(l) == 3,
       "a spectator's relay slot is relay base + gallery index");
    rnet_lobby_close(&l);
}

static void case_errors(void)
{
    RNetLobby *l = open_default("g");
    printf("  errors\n");
    rnet_lobby__ingest(l, "{\"op\":\"error\",\"code\":\"bad_password\",\"ok\":false}");
    ck(!strcmp(rnet_lobby_join_info(l)->last_error, "bad_password") &&
           rnet_lobby_join_info(l)->ok == 0,
       "a join failure is recorded");
    seat_in(l, "\"local_slot\":1,\"spectator\":false", "h", SLOTS_HG, NULL);
    rnet_lobby__ingest(l, "{\"op\":\"error\",\"code\":\"not_host\",\"ok\":false}");
    ck(rnet_lobby_join_info(l)->ok == 1, "an in-lobby refusal does not abandon the room");
    rnet_lobby__ingest(l, "{\"op\":\"error\",\"code\":\"disc_mismatch\",\"ok\":false}");
    ck(rnet_lobby_join_info(l)->ok == 0, "a fatal code does");
    rnet_lobby_clear_last_error(l);
    ck(rnet_lobby_join_info(l)->last_error[0] == '\0', "clear_last_error");
    rnet_lobby__ingest(l,
        "{\"op\":\"need_mods\",\"ok\":false,\"code\":\"need_mods\",\"lobby_id\":\"L\","
        "\"host_player_id\":\"h\",\"mods\":[{\"id\":\"psx.foo\",\"ver\":\"1.0.0\","
        "\"n\":\"Foo\",\"f\":\"wide\",\"b\":true,\"size\":0}],\"can_transfer\":true}");
    ck(rnet_lobby_need_mods_count(l) == 1 && rnet_lobby_need_mods_can_transfer(l) == 1,
       "need_mods lists what is missing");
    ck(rnet_lobby_need_mods_get(l, 0) &&
           !strcmp(rnet_lobby_need_mods_get(l, 0)->name, "Foo"),
       "with display names");
    ck(!strcmp(rnet_lobby_join_info(l)->last_error, "need_mods") && !rnet_lobby_in_lobby(l),
       "and is a refusal");
    rnet_lobby_close(&l);
}

/* ── chat / report / seats ───────────────────────────────────────────────── */

static void case_chat(void)
{
    RNetLobby *l = open_default("me");
    RNetLobbyChatMsg m;
    const char *mids[3];
    const char *f;
    printf("  chat\n");
    seat_in(l, "\"local_slot\":1,\"spectator\":false", "h",
            "{\"slot\":0,\"player_id\":\"h\"},{\"slot\":1,\"player_id\":\"me\"}", NULL);
    rnet_lobby__ingest(l,
        "{\"op\":\"chat\",\"lobby_id\":\"L\",\"from_player_id\":\"h\","
        "\"from_account\":\"acct-h\",\"from\":\"Marisa\",\"mid\":\"m-1\","
        "\"text\":\"what the fuck\"}");
    rnet_lobby__ingest(l,
        "{\"op\":\"chat\",\"lobby_id\":\"L\",\"from_player_id\":\"\",\"from\":\"\","
        "\"system\":true,\"mid\":\"m-2\",\"text\":\"Marisa has joined.\"}");
    rnet_lobby__ingest(l,
        "{\"op\":\"chat\",\"from_player_id\":\"me\",\"from\":\"Me\",\"mid\":\"m-3\","
        "\"text\":\"gg\"}");
    ck(rnet_lobby_chat_count(l) == 3, "three lines");
    ck(rnet_lobby_chat_get(l, 0, &m) && !strcmp(m.mid, "m-1") &&
           !strcmp(m.account, "acct-h") && !strcmp(m.text, "what the ****"),
       "a line keeps its mid and account and is masked on arrival");
    ck(rnet_lobby_chat_get(l, 1, &m) && m.is_system && m.mid[0] == '\0',
       "a system line is unreportable (no mid)");
    ck(rnet_lobby_chat_get(l, 2, &m) && m.is_local, "our echo is ours");
    rnet_lobby__ingest(l,
        "{\"op\":\"server_chat\",\"game_name\":\"Test Title\",\"from_player_id\":\"x\","
        "\"from\":\"Reimu\",\"country\":\"DE\",\"mid\":\"s-1\",\"system\":true,"
        "\"text\":\"anyone up?\"}");
    ck(rnet_lobby_server_chat_count(l) == 1 && rnet_lobby_server_chat_get(l, 0, &m) &&
           !strcmp(m.country, "DE") && !m.is_system,
       "server chat has its own ring, carries country, and has no system lines");
    tx_clear();
    ck(rnet_lobby_send_chat(l, "hi \"all\"") == 0 && tx_op("chat") &&
           strstr(tx_op("chat"), "\"text\":\"hi \\\"all\\\"\""),
       "chat send escapes");
    ck(rnet_lobby_chat_count(l) == 3, "a send is not appended locally (the echo is)");
    ck(rnet_lobby_send_server_chat(l, "yo") == 0 && tx_op("server_chat") &&
           strstr(tx_op("server_chat"), "\"game_name\":\"Test Title\""),
       "server chat carries the title");
    mids[0] = "m-1";
    mids[1] = "";
    mids[2] = "m-4";
    ck(rnet_lobby_report_chat(l, mids, 3, "harassment", "burst") == 0, "report sent");
    f = tx_op("chat_report");
    ck(f && strstr(f, "m-1") && strstr(f, "m-4") && !strstr(f, "what the"),
       "the report names mids, never the text");
    ck(f && strstr(f, "\"test\"") != NULL, "and the configured platform");
    rnet_lobby__ingest(l, "{\"op\":\"chat_report_ok\",\"ok\":true,\"mid\":\"m-1\"}");
    ck(!strcmp(rnet_lobby_last_report_ack(l), "m-1"), "the ack is recorded");
    rnet_lobby__ingest(l, "{\"op\":\"left\",\"ok\":true}");
    ck(rnet_lobby_chat_count(l) == 0 && rnet_lobby_server_chat_count(l) == 1,
       "leaving clears room chat, not server chat");
    rnet_lobby_close(&l);
}

static void case_seats(void)
{
    RNetLobby *l = open_default("h");
    char who[32];
    int from = -1;
    printf("  seats\n");
    rnet_lobby__ingest(l,
        "{\"op\":\"created\",\"ok\":true,\"lobby_id\":\"L\",\"local_slot\":0,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\"}]}");
    tx_clear();
    ck(rnet_lobby_kick(l, 1) == 0 && strstr(tx_op("kick"), "\"slot\":1"), "kick");
    ck(rnet_lobby_kick(l, 9) < 0, "kick of a seat outside both tables is refused");
    ck(rnet_lobby_move(l, 1, 65) == 0 &&
           strstr(tx_op("move"), "\"from_slot\":1,\"to_slot\":65"),
       "move into the gallery");
    ck(rnet_lobby_move(l, 1, 1) < 0, "a move to the same seat is refused");
    ck(rnet_lobby_seat_move_self(l, 2) == 0 && tx_op("seat_move"), "seat_move");
    ck(rnet_lobby_seat_swap_request(l, 1) == 0 && rnet_lobby_seat_swap_outgoing(l) == 1,
       "swap request waits");
    ck(rnet_lobby_seat_swap_request(l, 2) < 0, "one ask at a time");
    rnet_lobby__ingest(l, "{\"op\":\"seat_swap_result\",\"ok\":true,\"accept\":false}");
    ck(rnet_lobby_seat_swap_outgoing(l) == -1, "declined");
    rnet_lobby_seat_swap_clear(l);
    ck(rnet_lobby_seat_swap_outgoing(l) == 0, "cleared");
    rnet_lobby__ingest(l,
        "{\"op\":\"seat_swap_ask\",\"asker_player_id\":\"g\",\"asker_name\":\"Guest\","
        "\"from_slot\":1}");
    ck(rnet_lobby_seat_swap_incoming(l, who, sizeof(who), &from) == 1 &&
           !strcmp(who, "Guest") && from == 1,
       "an incoming ask names the asker and seat");
    tx_clear();
    ck(rnet_lobby_seat_swap_respond(l, 1) == 0 &&
           strstr(tx_op("seat_swap_answer"), "\"asker_player_id\":\"g\""),
       "the answer names the asker");
    ck(rnet_lobby_seat_swap_incoming(l, NULL, 0, NULL) == 0, "and consumes the ask");
    rnet_lobby_close(&l);
}

static void case_caps(void)
{
    RNetLobby *l = open_default("h");
    RNetLobbyMatchCaps c, back;
    char out[RNET_LOBBY_CAPS_JSON_LEN];
    size_t n;
    int i;
    printf("  match caps\n");
    ck(rnet_lobby_match_caps_decode(l, "{\"v\":1}", &back) == 1, "an empty blob decodes");
    ck(back.input_delay == 6 && back.rollback == 1 && back.input_prediction == 10,
       "absent keys read as the configured defaults");
    ck(rnet_lobby_match_caps_decode(l, "[1]", &back) == 0, "a non-object is refused");
    ck(rnet_lobby_match_caps_decode(l,
           "{\"v\":1,\"input_delay\":-5,\"input_prediction\":99,\"force_turn\":true,"
           "\"force_input_relay\":true,\"aspect_num\":16,\"session_bios\":\"openbios\"}",
           &back) == 1, "decode");
    ck(back.input_delay == 0 && back.input_prediction == 16, "clamps");
    ck(back.force_turn && back.force_input_relay, "transport flags");
    ck(!strcmp(back.game_json, "\"aspect_num\":16,\"session_bios\":\"openbios\""),
       "every title-owned member is carried verbatim, library keys are not");
    ck(strstr(back.json, "\"aspect_num\":16") != NULL, "the whole object is kept too");
    rnet_lobby_match_caps_init(l, &c);
    c.valid = 1;
    c.input_prediction = 0;
    n = rnet_lobby_match_caps_encode(l, &c, out, sizeof(out));
    ck(n > 0 && !strstr(out, "input_prediction"),
       "input_prediction 0 is not emitted (a title that does not use it)");
    /* Over the server's 4096-byte limit: nothing is published. */
    for (i = 0; i < (int)sizeof(c.game_json) - 64; i += 20)
        memcpy(c.game_json + i, "\"k00000000000000\":1,", 20);
    c.game_json[i - 1] = '\0';
    c.mod_count = RNET_LOBBY_MAX_MODS;
    for (i = 0; i < RNET_LOBBY_MAX_MODS; ++i) {
        snprintf(c.mods[i].id, sizeof(c.mods[i].id), "pkg.%02d", i);
        snprintf(c.mods[i].ver, sizeof(c.mods[i].ver), "1.0.0");
        snprintf(c.mods[i].feats, sizeof(c.mods[i].feats), "%080d", i);
    }
    ck(rnet_lobby_match_caps_encode(l, &c, out, sizeof(out)) == 0,
       "caps over 4000 bytes publish nothing");
    /* Host republish and start carry them; an unencodable blob is refused. */
    rnet_lobby__ingest(l,
        "{\"op\":\"created\",\"ok\":true,\"lobby_id\":\"L\",\"local_slot\":0,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\"}]}");
    tx_clear();
    ck(rnet_lobby_set_match_caps(l, &c) < 0, "an oversized republish is refused");
    ck(rnet_lobby_match_caps(l)->mod_count == 0,
       "and is not adopted locally: the host keeps running what the room was told");
    rnet_lobby_match_caps_init(l, &c);
    c.valid = 1;
    ck(rnet_lobby_set_match_caps(l, &c) == 0 && tx_op("set_match_caps") &&
           strstr(tx_op("set_match_caps"), "\"match_caps\":{\"v\":1"),
       "set_match_caps");
    ck(rnet_lobby_request_start(l, &c) == 0 && tx_op("start") &&
           strstr(tx_op("start"), "\"match_caps\":{"),
       "start freezes the caps");
    rnet_lobby_close(&l);
}

/* The server keeps a spectator's ready false and answers every set_ready
 * with a lobby_update: auto_ready re-arming on each update (the snesrecomp
 * behaviour) is an endless set_ready / lobby_update loop for the gallery. */
static void case_auto_ready_gallery(void)
{
    RNetLobbyConfig cfg;
    RNetLobby *l;
    const char *upd =
        "{\"op\":\"lobby_update\",\"player_count\":2,\"max_slots\":2,"
        "\"spectator_slot_base\":64,\"allow_spectators\":true,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"ready\":true},"
        "{\"slot\":1,\"player_id\":\"g\",\"ready\":true}],"
        "\"spectators\":[{\"slot\":64,\"player_id\":\"s\",\"ready\":false}]}";
    printf("  auto_ready in the gallery\n");
    quiet_cfg(&cfg);
    cfg.auto_ready = 1;
    l = open_with(&cfg, "s");
    rnet_lobby__ingest(l,
        "{\"op\":\"joined\",\"ok\":true,\"lobby_id\":\"L\",\"session_id\":1,"
        "\"local_slot\":64,\"spectator\":true,\"spectator_slot_base\":64,"
        "\"host_endpoint\":\"203.0.113.9:7000\",\"guest_endpoint\":\"\"}");
    ck(rnet_lobby_local_is_spectator(l) == 1,
       "joined says the role outright, before any lobby_update");
    ck(tx_count("set_ready") == 1, "a spectator announces its offer once on joining");
    rnet_lobby__ingest(l, upd);
    rnet_lobby__ingest(l, upd);
    ck(tx_count("set_ready") == 1,
       "and does not answer each update with another set_ready (no loop)");
    rnet_lobby__ingest(l,
        "{\"op\":\"lobby_update\",\"player_count\":2,\"max_slots\":2,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"ready\":true},"
        "{\"slot\":1,\"player_id\":\"s\",\"ready\":false}],\"spectators\":[]}");
    ck(tx_count("set_ready") == 2, "promoted to a player, it arms Ready");
    rnet_lobby_close(&l);
}

static void case_ready_extras(void)
{
    RNetLobby *l = open_default("g");
    char big[RNET_LOBBY_READY_EXTRA_LEN + 8];
    printf("  ready extras\n");
    seat_in(l, "\"local_slot\":1,\"spectator\":false", "h", SLOTS_HG, NULL);
    ck(rnet_lobby_set_ready_extra_json(l,
           "\"bios_offer\":{\"v\":1,\"prefer\":\"openbios\",\"can_openbios\":true}") == 0,
       "extras set");
    tx_clear();
    ck(rnet_lobby_set_ready(l, 1) == 0, "set_ready");
    ck(tx_op("set_ready") &&
           strstr(tx_op("set_ready"), "\"ready\":true,\"bios_offer\":{\"v\":1"),
       "every set_ready carries the title's extras");
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    ck(rnet_lobby_set_ready_extra_json(l, big) < 0, "over-long extras are refused");
    rnet_lobby_set_ready_extra_json(l, NULL);
    tx_clear();
    rnet_lobby_set_ready(l, 0);
    ck(tx_op("set_ready") && !strstr(tx_op("set_ready"), "bios_offer"), "extras cleared");
    rnet_lobby_close(&l);
}

/* ── signals ─────────────────────────────────────────────────────────────── */

static void case_signal_gate(void)
{
    RNetLobbyConfig cfg;
    RNetLobby *l;
    int type = 0, flag = 0;
    char text[64];
    printf("  signal queue and gate\n");
    l = open_default("g");
    seat_in(l, "\"local_slot\":1,\"spectator\":false", "h", SLOTS_HG, NULL);
    rnet_lobby__ingest(l,
        "{\"op\":\"signal\",\"from_player_id\":\"h\",\"type\":1,\"flag\":0,"
        "\"text\":\"v=0 sdp\"}");
    ck(rnet_lobby_poll_signal(l, &type, &flag, text, sizeof(text)) == 1 && type == 1 &&
           !strcmp(text, "v=0 sdp"),
       "outside PEER_PATH the gameplay queue is open");
    tx_clear();
    ck(rnet_lobby_send_signal(l, 3, 0, "cand") == 0 && tx_op("signal") &&
           strstr(tx_op("signal"), "\"to_player_id\":\"\"") &&
           strstr(tx_op("signal"), "\"lobby_id\":\"L\""),
       "a broadcast signal names the lobby and no peer");
    rnet_lobby_close(&l);

    quiet_cfg(&cfg);
    cfg.waiting_room_rtt = RNET_LOBBY_RTT_PEER_PATH;
    l = open_with(&cfg, "g");
    rnet_lobby_join(l, "L", NULL, "0.0.0.0:7790");
    seat_in(l, "\"local_slot\":1,\"spectator\":false", "h", SLOTS_HG, NULL);
    rnet_lobby__ingest(l, "{\"op\":\"signal\",\"from_player_id\":\"h\",\"type\":1,"
                          "\"text\":\"stale\"}");
    ck(rnet_lobby_poll_signal(l, &type, &flag, text, sizeof(text)) == 0,
       "under PEER_PATH waiting-room ICE is gated");
    rnet_lobby__ingest(l,
        "{\"op\":\"launch\",\"ok\":true,\"host_endpoint\":\"203.0.113.9:7000\","
        "\"guest_endpoint\":\"198.51.100.2:7790\",\"transport\":\"ice_p2p\","
        "\"player_count\":2,\"max_slots\":2,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\"},{\"slot\":1,\"player_id\":\"g\"}]}");
    rnet_lobby__ingest(l, "{\"op\":\"signal\",\"from_player_id\":\"h\",\"type\":1,"
                          "\"text\":\"match\"}");
    ck(rnet_lobby_poll_signal(l, &type, &flag, text, sizeof(text)) == 1 &&
           !strcmp(text, "match"),
       "a launch opens the gate for the match");
    /* RTT signals never reach the gameplay queue. */
    rnet_lobby__ingest(l, "{\"op\":\"signal\",\"from_player_id\":\"h\",\"type\":102,"
                          "\"text\":\"42\"}");
    ck(rnet_lobby_poll_signal(l, &type, &flag, text, sizeof(text)) == 0,
       "an RTT report is consumed by the latency layer");
    ck(rnet_lobby_member_latency_ms(l, 0) == 42, "and stored under the reporter's seat");
    rnet_lobby__ingest(l, "{\"op\":\"signal\",\"from_player_id\":\"h\",\"type\":102,"
                          "\"text\":\"30\"}");
    ck(rnet_lobby_member_latency_ms(l, 0) == 42, "PEER_PATH keeps the pessimistic sample");
    ck(rnet_lobby_member_latency_ms(l, 1) == -1, "never our own seat");
    rnet_lobby_close(&l);
}

static void case_ws_rtt(void)
{
    RNetLobbyConfig cfg;
    RNetLobby *l;
    char ts[192];
    printf("  WS-signal latency\n");
    quiet_cfg(&cfg);
    cfg.waiting_room_rtt = RNET_LOBBY_RTT_WS_SIGNAL;
    l = open_with(&cfg, "h");
    rnet_lobby__ingest(l,
        "{\"op\":\"created\",\"ok\":true,\"lobby_id\":\"L\",\"local_slot\":0,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\"},{\"slot\":1,\"player_id\":\"g\"}]}");
    tx_clear();
    rnet_lobby__ingest(l, "{\"op\":\"signal\",\"from_player_id\":\"g\",\"type\":100,"
                          "\"text\":\"12345\"}");
    ck(tx_op("signal") && strstr(tx_op("signal"), "\"type\":101") &&
           strstr(tx_op("signal"), "\"to_player_id\":\"g\"") &&
           strstr(tx_op("signal"), "\"text\":\"12345\""),
       "the host answers a ping to the asker only, echoing its stamp");
    rnet_lobby__ingest(l, "{\"op\":\"signal\",\"from_player_id\":\"g\",\"type\":102,"
                          "\"text\":\"80\"}");
    rnet_lobby__ingest(l, "{\"op\":\"signal\",\"from_player_id\":\"g\",\"type\":102,"
                          "\"text\":\"50\"}");
    ck(rnet_lobby_member_latency_ms(l, 1) == 50, "WS mode keeps the latest report");
    ck(rnet_lobby_member_latency_ms(l, 0) == -1, "the host row has no measurement");
    rnet_lobby_close(&l);

    l = open_with(&cfg, "g");
    seat_in(l, "\"local_slot\":1,\"spectator\":false", "h", SLOTS_HG, NULL);
    tx_clear();
    snprintf(ts, sizeof(ts), "{\"op\":\"signal\",\"from_player_id\":\"h\","
                             "\"type\":101,\"text\":\"%llu\"}",
             (unsigned long long)rnet_lobby__now_ms());
    rnet_lobby__ingest(l, ts);
    ck(rnet_lobby_member_latency_ms(l, 1) >= 0, "a pong is timed onto our own row");
    ck(tx_op("signal") && strstr(tx_op("signal"), "\"type\":102"),
       "and reported to the room");
    tx_clear();
    rnet_lobby_pump(l);
    ck(tx_op("signal") && strstr(tx_op("signal"), "\"type\":100") &&
           strstr(tx_op("signal"), "\"to_player_id\":\"h\""),
       "the pump pings the host");
    rnet_lobby_close(&l);
}

static void case_mod_signal_hold(void)
{
    RNetLobby *l = open_default("h");
    int type = 0;
    char text[32];
    printf("  mod transfer signals\n");
    rnet_lobby__ingest(l,
        "{\"op\":\"created\",\"ok\":true,\"lobby_id\":\"L\",\"local_slot\":0,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\"},{\"slot\":1,\"player_id\":\"g\"}]}");
    rnet_lobby__ingest(l, "{\"op\":\"signal\",\"from_player_id\":\"g\",\"type\":121,"
                          "\"text\":\"offer\"}");
    ck(l->c.sig_hold_n == 1 && !strcmp(l->c.sig_hold_from, "g") &&
           l->c.sig_hold[0].type == RNET_SIGNAL_REMOTE_SDP,
       "an early transfer offer is held, already translated to REMOTE_SDP");
    ck(rnet_lobby_poll_signal(l, &type, NULL, text, sizeof(text)) == 0,
       "and never reaches the gameplay queue");
    tx_clear();
    rnet_lobby__ingest(l, "{\"op\":\"signal\",\"from_player_id\":\"g\",\"type\":110,"
                          "\"text\":\"not.in.plan@1.0\"}");
    ck(tx_op("signal") && strstr(tx_op("signal"), "\"type\":111") &&
           strstr(tx_op("signal"), "not part of this lobby"),
       "a request outside the plan is refused to the asker");
    rnet_lobby_close(&l);
}

/* ── TURN ────────────────────────────────────────────────────────────────── */

static void case_turn(void)
{
    RNetLobby *l = open_default("me");
    const RNetLobbyTurnCredentials *t;
    printf("  TURN credentials\n");
    rnet_lobby__ingest(l, "{\"op\":\"turn_credentials\",\"ok\":false,"
                          "\"error\":\"coturn_unconfigured\"}");
    ck(rnet_lobby_turn_credentials(l)->valid == 0, "an unconfigured server mints nothing");
    ck(l->c.turn_retry_ms > rnet_lobby__now_ms() + 20000u,
       "and automatic re-asks back off (no request per pump)");
    rnet_lobby__ingest(l,
        "{\"op\":\"turn_credentials\",\"ok\":true,\"stun_host\":\"coturn.example.com\","
        "\"stun_port\":3478,\"turn_host\":\"coturn.example.com\",\"turn_port\":3479,"
        "\"turns_port\":5349,\"realm\":\"recomp-net\",\"username\":\"1700000000:p\","
        "\"password\":\"c2VjcmV0\",\"ttl_secs\":86400}");
    t = rnet_lobby_turn_credentials(l);
    ck(t->valid && t->turn_port == 3479 && t->turns_port == 5349 &&
           !strcmp(t->realm, "recomp-net") && !strcmp(t->password, "c2VjcmV0"),
       "a mint parses whole");
    tx_clear();
    ck(rnet_lobby_request_turn_credentials(l) == 0 && tx_count("get_turn_credentials") == 0,
       "fresh credentials are not re-requested");
    l->c.turn.ttl_secs = 30; /* within the 60 s margin */
    ck(rnet_lobby_request_turn_credentials(l) == 0 && tx_count("get_turn_credentials") == 1,
       "credentials near expiry are refreshed");
    rnet_lobby_close(&l);
}

/* ── automatch ───────────────────────────────────────────────────────────── */

static void case_automatch(void)
{
    RNetLobbyConfig cfg;
    RNetLobby *l;
    RNetLobbyRuleset r;
    RNetLobbyAutomatchFound f;
    const char *q;
    printf("  automatch\n");
    quiet_cfg(&cfg);
    l = open_with(&cfg, "me");
    ck(rnet_lobby_automatch_available(l) == 0, "not asked yet is not available");
    tx_clear();
    ck(rnet_lobby_automatch_request_rulesets(l) == 0 &&
           rnet_lobby_automatch_request_rulesets(l) == 0 &&
           tx_count("automatch_rulesets") == 1,
       "one request in flight, however often the launcher polls");
    rnet_lobby__ingest(l,
        "{\"op\":\"automatch_rulesets_ok\",\"ok\":true,"
        "\"rulesets\":[{\"id\":\"standard\",\"label\":\"Standard\","
        "\"caps_summary\":\"Delay 2 - Rollback on\",\"game_version\":\"1.2.0\","
        "\"max_slots\":2,\"match_caps\":{\"v\":1,\"input_delay\":2,\"rollback\":true,"
        "\"widescreen\":false}}]}");
    ck(rnet_lobby_automatch_available(l) == 1 && rnet_lobby_automatch_ruleset_count(l) == 1,
       "one ruleset");
    ck(rnet_lobby_automatch_ruleset_get(l, 0, &r) && !strcmp(r.id, "standard") &&
           r.caps.valid && r.caps.input_delay == 2 && r.max_slots == 2 &&
           strstr(r.caps.game_json, "widescreen") != NULL,
       "the ruleset's caps parse like a host's blob");
    ck(rnet_lobby_automatch_queue(l, NULL, 0, NULL) < 0 &&
           rnet_lobby_automatch_state(l) == RNET_LOBBY_AUTOMATCH_FAILED,
       "no fingerprint, no queue -- and the reason is said");
    rnet_lobby_set_fp(l, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    tx_clear();
    l->c.join.ok = 1; /* a claimed error must not touch the join record */
    ck(rnet_lobby_automatch_queue(l, NULL, 0, "hud@1.0#aa;fonts@2.0#bb") == 0, "queued");
    q = tx_op("automatch_queue");
    ck(q && strstr(q, "\"ruleset_id\":\"standard\"") && strstr(q, "\"disc_fp\":\"0011"),
       "the ticket carries the ruleset and the fingerprint");
    ck(q && strstr(q, "\"mod_exempt\":[\"hud@1.0#aa\",\"fonts@2.0#bb\"]"),
       "the exemption evidence is an ARRAY");
    ck(q && strstr(q, "\"mods_enabled\":false"), "the assertion rides along");
    ck(rnet_lobby_automatch_queue(l, NULL, 0, NULL) < 0, "no second ticket while one is out");
    rnet_lobby__ingest(l, "{\"op\":\"error\",\"code\":\"need_account\",\"ok\":false}");
    ck(rnet_lobby_automatch_state(l) == RNET_LOBBY_AUTOMATCH_FAILED &&
           strstr(rnet_lobby_automatch_error(l), "Sign in") != NULL,
       "a refusal of the first attempt is claimed as automatch's");
    ck(rnet_lobby_join_info(l)->ok == 1,
       "a claimed automatch refusal leaves the join record alone (unclaimed, a "
       "not-seated error would clear ok)");
    rnet_lobby_automatch_queue(l, "standard", 0, NULL);
    rnet_lobby__ingest(l,
        "{\"op\":\"automatch_queued\",\"ok\":true,\"ticket_id\":\"T1\","
        "\"titles\":[{\"game_name\":\"Test Title\",\"ruleset_id\":\"standard\",\"pool\":3}]}");
    ck(rnet_lobby_automatch_state(l) == RNET_LOBBY_AUTOMATCH_QUEUED &&
           rnet_lobby_automatch_pool(l) == 3,
       "queued with the pool size");
    rnet_lobby__ingest(l,
        "{\"op\":\"automatch_status\",\"ok\":true,\"ticket_id\":\"T1\",\"queued_secs\":47,"
        "\"est_rtt_ms\":38,\"titles\":[{\"pool\":5}]}");
    ck(rnet_lobby_automatch_queued_secs(l) == 47 && rnet_lobby_automatch_pool(l) == 5,
       "status updates the wait");
    rnet_lobby__ingest(l,
        "{\"op\":\"automatch_found\",\"ok\":true,\"match_id\":\"M1\","
        "\"game_name\":\"Test Title\",\"game_version\":\"1.2.0\","
        "\"ruleset_id\":\"standard\",\"ruleset_label\":\"Standard\","
        "\"match_caps\":{\"v\":1,\"input_delay\":4},"
        "\"opponent\":{\"handle\":\"Marisa\",\"discord_username\":\"marisa\","
        "\"avatar\":\"x\",\"country\":\"JP\"},\"est_rtt_ms\":74,\"accept_secs\":15,"
        "\"input_delay\":4,\"input_prediction\":8,\"frames_needed\":5}");
    ck(rnet_lobby_automatch_found_get(l, &f) == 1, "found");
    ck(!strcmp(f.opponent, "Marisa") && !strcmp(f.opponent_username, "marisa") &&
           !strcmp(f.opponent_country, "JP"),
       "the opponent OBJECT is read (the snesrecomp copy read it as a string)");
    ck(!strcmp(f.ruleset_label, "Standard") && f.est_rtt_ms == 74, "label and estimate");
    ck(f.input_delay == 4 && f.input_prediction == 8 && f.frames_needed == 5,
       "the floored delay the player is agreeing to");
    ck(f.accept_secs >= 14 && f.accept_secs <= 15, "a live countdown");
    ck(rnet_lobby_automatch_found_caps(l)->valid &&
           rnet_lobby_automatch_found_caps(l)->input_delay == 4,
       "the floored caps");
    tx_clear();
    ck(rnet_lobby_automatch_accept(l, 1) == 0 && tx_op("automatch_accept") &&
           strstr(tx_op("automatch_accept"), "\"match_id\":\"M1\"") &&
           strstr(tx_op("automatch_accept"), "\"accept\":true"),
       "accept echoes the match id");
    ck(rnet_lobby_automatch_state(l) == RNET_LOBBY_AUTOMATCH_ACCEPTED, "accepted");
    rnet_lobby__ingest(l,
        "{\"op\":\"automatch_requeue\",\"ok\":true,\"reason\":\"peer_declined\","
        "\"queued\":true}");
    ck(rnet_lobby_automatch_state(l) == RNET_LOBBY_AUTOMATCH_QUEUED,
       "a peer's decline puts us back in the queue, not in FAILED");
    rnet_lobby__ingest(l,
        "{\"op\":\"automatch_requeue\",\"ok\":true,\"reason\":\"lobby_limit\","
        "\"queued\":false}");
    ck(rnet_lobby_automatch_state(l) == RNET_LOBBY_AUTOMATCH_FAILED,
       "lobby_limit ends the ticket");
    rnet_lobby__ingest(l,
        "{\"op\":\"automatch_cancelled\",\"ok\":true,\"reason\":\"declined\","
        "\"cooldown_secs\":30}");
    ck(strstr(rnet_lobby_automatch_error(l), "30 Second Cooldown") != NULL,
       "a decline's cooldown is shown");
    /* An accepted pairing that reaches a room marks it as automatch's. */
    rnet_lobby_automatch_queue(l, "standard", 0, NULL);
    rnet_lobby__ingest(l, "{\"op\":\"automatch_queued\",\"ok\":true,\"ticket_id\":\"T2\"}");
    rnet_lobby__ingest(l,
        "{\"op\":\"automatch_found\",\"ok\":true,\"match_id\":\"M2\",\"opponent\":\"Legacy\","
        "\"opponent_country\":\"FR\",\"label\":\"Old Label\",\"accept_secs\":15}");
    ck(rnet_lobby_automatch_found_get(l, &f) && !strcmp(f.opponent, "Legacy") &&
           !strcmp(f.ruleset_label, "Old Label"),
       "the legacy flat shape still reads");
    rnet_lobby_automatch_accept(l, 1);
    rnet_lobby__ingest(l,
        "{\"op\":\"joined\",\"ok\":true,\"lobby_id\":\"A1\",\"session_id\":7,"
        "\"local_slot\":1,\"spectator\":false,\"spectator_slot_base\":64,"
        "\"host_endpoint\":\"relay.example:8777\",\"guest_endpoint\":\"\","
        "\"automatch\":true,\"match_caps\":{\"v\":1,\"input_delay\":4}}");
    ck(rnet_lobby_automatch_room(l) == 1 &&
           rnet_lobby_automatch_state(l) == RNET_LOBBY_AUTOMATCH_IDLE,
       "the room is automatch's and the ticket is spent");
    rnet_lobby__ingest(l, "{\"op\":\"left\",\"ok\":true}");
    ck(rnet_lobby_automatch_room(l) == 0, "leaving ends it");
    /* A declined ack must not reopen the gate. */
    rnet_lobby_automatch_queue(l, "standard", 0, NULL);
    rnet_lobby__ingest(l, "{\"op\":\"automatch_queued\",\"ok\":true,\"ticket_id\":\"T3\"}");
    rnet_lobby__ingest(l, "{\"op\":\"automatch_found\",\"ok\":true,\"match_id\":\"M3\"}");
    rnet_lobby_automatch_accept(l, 0);
    rnet_lobby__ingest(l, "{\"op\":\"automatch_accept_ok\",\"ok\":true,\"accept\":false}");
    ck(rnet_lobby_automatch_state(l) == RNET_LOBBY_AUTOMATCH_IDLE,
       "a declined ack leaves the gate closed");
    /* A new connection re-asks. */
    rnet_lobby__ingest(l, "{\"op\":\"welcome\",\"player_id\":\"me2\",\"ok\":true}");
    ck(rnet_lobby_automatch_available(l) == 0, "a new connection forgets the rulesets");
    rnet_lobby_close(&l);
}

static void case_desync_report(void)
{
    RNetLobby *l = open_default("me");
    RNetLobbyDesyncReport r;
    const char *f;
    printf("  desync report\n");
    memset(&r, 0, sizeof(r));
    r.tick = 1234;
    r.partition = "wram";
    r.mine = 0xdeadbeefu;
    r.theirs = 0x1u;
    r.is_host = 1;
    r.mod_exempt = "hud@1#aa";
    ck(rnet_lobby_report_desync(l, &r) == 0, "sent");
    f = tx_op("desync_report");
    ck(f && strstr(f, "\"mine\":\"deadbeef\"") && strstr(f, "\"theirs\":\"00000001\""),
       "digests travel as hex strings");
    ck(f && strstr(f, "\"role\":\"host\"") && strstr(f, "\"tick\":1234"), "role and tick");
    rnet_lobby_close(&l);
}

int main(void)
{
    case_config_defaults();
    case_env_overrides();
    case_version_rule();
    case_fingerprint();
    case_welcome();
    case_utf8_names();
    case_session_hello();
    case_list();
    case_big_list_frames();
    case_create_frames();
    case_created_and_binds();
    case_joined_guest();
    case_relay_policy();
    case_errors();
    case_chat();
    case_seats();
    case_caps();
    case_auto_ready_gallery();
    case_ready_extras();
    case_signal_gate();
    case_ws_rtt();
    case_mod_signal_hold();
    case_turn();
    case_automatch();
    case_desync_report();
    tx_clear();
    printf(fails ? "\n%d failure(s)\n" : "\nlobby_client_test: all passed\n", fails);
    return fails != 0;
}
