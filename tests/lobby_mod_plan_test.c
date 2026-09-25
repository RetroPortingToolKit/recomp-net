/* lobby_mod_plan_test -- ported from snesrecomp tests/netplay/
 * lobby_mod_plan_test.c (origin/main 284afca) onto the shared client.
 *
 * The plan is what decides whether a match may start, and the offer is what
 * each seat claims to hold. Both must survive the wire exactly: a row that
 * parses to the wrong package tells a player to install the wrong thing, and
 * a count one short hides a requirement -- silently, in the direction that
 * lets a broken match start. These cases drive the REAL encoders / parsers
 * (through the private header), not a mirror of them. No case opens a socket.
 *
 * Differences from the SNES original, all deliberate:
 *   - encode returns the caps OBJECT; the `,"match_caps":` wrapper is the
 *     caller's (caps_member in the client);
 *   - the old-string case checks the key the plan actually uses (mod_plan) as
 *     well as the server's `mods` (the original only checked `mods`);
 *   - the offer carries "v":1 like the server's documented shape.
 */
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

static RNetLobby *open_quiet(void)
{
    RNetLobbyConfig cfg;
    RNetLobby *l = NULL;
    rnet_lobby_config_init(&cfg);
    cfg.game_name = "Test Title";
    cfg.game_version = "1.0.0";
    cfg.platform = "test";
    cfg.max_players = 4;
    cfg.max_spectators = 4;
    cfg.waiting_room_rtt = RNET_LOBBY_RTT_OFF;
    cfg.list_latency = 0;
    cfg.lan_beacon = 0;
    cfg.host_advertise = 0;
    cfg.log_min_level = RNET_LOBBY_LOG_ERROR + 1; /* silence */
    if (rnet_lobby_open(&l, &cfg) != 0 || !l) {
        printf("could not open a lobby handle\n");
        exit(2);
    }
    return l;
}

static void sink(void *user, const char *frame)
{
    (void)user;
    (void)frame;
}

static int two_pkg_offer(RNetLobbyModPkg *out, int max, void *ctx)
{
    (void)ctx;
    if (max < 2)
        return 0;
    memset(out, 0, sizeof(*out) * 2);
    snprintf(out[0].id, sizeof(out[0].id), "gwed.localization");
    snprintf(out[0].ver, sizeof(out[0].ver), "1.0.0");
    snprintf(out[1].id, sizeof(out[1].id), "gwed.enhancement.widescreen");
    snprintf(out[1].ver, sizeof(out[1].ver), "1.0.0");
    return 2;
}

/* The server insists on an ARRAY under mod_plan, and the seat-gate key
 * `mods` must stay unpublished: this title lets a peer without the mods in. */
static void ck_is_json_array(const char *json)
{
    const char *m = strstr(json, "\"mod_plan\":");
    ck(m != NULL, "match_caps carries a mod_plan field");
    if (!m)
        return;
    ck(m[11] == '[', "mod_plan is a JSON array, not a string");
    ck(strstr(json, "\"mods\":") == NULL, "the server's seat-gate key is NOT published");
}

static void case_rows(RNetLobby *l)
{
    RNetLobbyMatchCaps caps, back;
    char json[RNET_LOBBY_CAPS_JSON_LEN];
    printf("  plan rows\n");
    rnet_lobby_match_caps_init(l, &caps);
    caps.valid = 1;
    caps.input_delay = 2;
    caps.mod_count = 2;
    snprintf(caps.mods[0].id, sizeof(caps.mods[0].id), "gwed.localization");
    snprintf(caps.mods[0].ver, sizeof(caps.mods[0].ver), "1.0.0");
    snprintf(caps.mods[0].name, sizeof(caps.mods[0].name), "Localization");
    snprintf(caps.mods[0].feats, sizeof(caps.mods[0].feats), "localization");
    snprintf(caps.mods[1].id, sizeof(caps.mods[1].id), "gwed.enhancement.widescreen");
    snprintf(caps.mods[1].ver, sizeof(caps.mods[1].ver), "1.0.0");
    snprintf(caps.mods[1].name, sizeof(caps.mods[1].name), "Widescreen");
    snprintf(caps.mods[1].feats, sizeof(caps.mods[1].feats), "widescreen");

    ck(rnet_lobby_match_caps_encode(l, &caps, json, sizeof(json)) > 0, "caps encoded");
    ck_is_json_array(json);
    ck(strstr(json, "[,") == NULL, "no leading comma in the array");
    ck(strstr(json, "\"id\":\"gwed.localization\"") != NULL, "row 0 id present");
    ck(strstr(json, "\"ver\":\"1.0.0\"") != NULL, "row 0 ver present");

    ck(rnet_lobby_match_caps_decode(l, json, &back) == 1, "caps decode");
    ck(back.mod_count == 2, "two rows round-tripped");
    if (back.mod_count == 2) {
        ck(!strcmp(back.mods[0].id, "gwed.localization"), "row 0 id");
        ck(!strcmp(back.mods[0].ver, "1.0.0"), "row 0 ver");
        ck(!strcmp(back.mods[0].feats, "localization"), "row 0 feats");
        ck(!strcmp(back.mods[1].id, "gwed.enhancement.widescreen"), "row 1 id");
    }
}

static void case_empty(RNetLobby *l)
{
    RNetLobbyMatchCaps caps;
    RNetLobbyModPkg back[RNET_LOBBY_MAX_MODS];
    char json[512];
    printf("  empty plan\n");
    rnet_lobby_match_caps_init(l, &caps);
    caps.valid = 1;
    ck(rnet_lobby_match_caps_encode(l, &caps, json, sizeof(json)) > 0, "empty encoded");
    ck(strstr(json, "\"mod_plan\":[]") != NULL, "empty plan is an empty array");
    ck(rnet_lobby__parse_mod_pkgs(rnet_json_span(json), "mod_plan", back,
                                  RNET_LOBBY_MAX_MODS) == 0,
       "empty array parses to no rows");
}

static void case_old_string_encoding_is_not_revived(void)
{
    RNetLobbyModPkg back[RNET_LOBBY_MAX_MODS];
    const char *old = "{\"v\":1,\"mod_plan\":\"gwed.localization@1.0.0/localization\","
                      "\"mods\":\"gwed.localization@1.0.0\"}";
    printf("  superseded string plan\n");
    ck(rnet_lobby__parse_mod_pkgs(rnet_json_span(old), "mod_plan", back,
                                  RNET_LOBBY_MAX_MODS) == 0,
       "a string-encoded mod_plan yields no rows");
    ck(rnet_lobby__parse_mod_pkgs(rnet_json_span(old), "mods", back,
                                  RNET_LOBBY_MAX_MODS) == 0,
       "a string-encoded mods yields no rows");
}

static void case_partial_rows_dropped(void)
{
    RNetLobbyModPkg back[RNET_LOBBY_MAX_MODS];
    const char *j = "{\"mod_plan\":[{\"id\":\"a.one\"},{\"ver\":\"2.0\"},"
                    "{\"id\":\"b.two\",\"ver\":\"2.0\"}]}";
    int n;
    char big[512];
    printf("  partial rows\n");
    n = rnet_lobby__parse_mod_pkgs(rnet_json_span(j), "mod_plan", back,
                                   RNET_LOBBY_MAX_MODS);
    ck(n == 1, "rows missing id or ver are dropped");
    if (n == 1)
        ck(!strcmp(back[0].id, "b.two"), "the complete row survives");
    /* An id too long to hold would be TRUNCATED into a different package. */
    memset(big, 'x', 200);
    big[200] = '\0';
    {
        char jj[768];
        snprintf(jj, sizeof(jj), "{\"mod_plan\":[{\"id\":\"%s\",\"ver\":\"1\"},"
                                 "{\"id\":\"ok\",\"ver\":\"1\"}]}", big);
        n = rnet_lobby__parse_mod_pkgs(rnet_json_span(jj), "mod_plan", back,
                                       RNET_LOBBY_MAX_MODS);
        ck(n == 1 && !strcmp(back[0].id, "ok"),
           "an id that would truncate is skipped, not shortened");
    }
}

static void case_overflow_refuses(RNetLobby *l)
{
    RNetLobbyMatchCaps caps;
    char small[64];
    int i;
    printf("  overflow\n");
    rnet_lobby_match_caps_init(l, &caps);
    caps.valid = 1;
    caps.mod_count = RNET_LOBBY_MAX_MODS;
    for (i = 0; i < RNET_LOBBY_MAX_MODS; ++i) {
        snprintf(caps.mods[i].id, sizeof(caps.mods[i].id), "pkg.number.%02d", i);
        snprintf(caps.mods[i].ver, sizeof(caps.mods[i].ver), "1.0.0");
    }
    ck(rnet_lobby_match_caps_encode(l, &caps, small, sizeof(small)) == 0,
       "a plan that does not fit publishes nothing at all");
    ck(small[0] == '\0', "and leaves no fragment behind");
}

static int gate_with(RNetLobby *l, const RNetLobbyModPkg *plan, int plan_n,
                     const RNetLobbyModPkg *peer, int peer_n, char *who,
                     size_t who_cap, char *what, size_t what_cap)
{
    RNetLobbyConn *c = &l->c;
    rnet_lobby_match_caps_init(l, &c->match_caps);
    c->match_caps.valid = 1;
    c->match_caps.mod_count = plan_n;
    memcpy(c->match_caps.mods, plan, sizeof(RNetLobbyModPkg) * (size_t)plan_n);
    c->in_lobby = 1;
    snprintf(c->player_id, sizeof(c->player_id), "me");
    c->member_count = 2;
    snprintf(c->members[0].player_id, sizeof(c->members[0].player_id), "me");
    snprintf(c->members[1].player_id, sizeof(c->members[1].player_id), "them");
    snprintf(c->members[1].display_name, sizeof(c->members[1].display_name), "Bob");
    c->member_offer_count[0] = 0;
    c->member_offer_count[1] = peer_n;
    if (peer_n)
        memcpy(c->member_offer[1], peer, sizeof(RNetLobbyModPkg) * (size_t)peer_n);
    return rnet_lobby_match_blocked_by_mods(l, who, who_cap, what, what_cap);
}

static RNetLobbyModPkg row(const char *id, const char *ver)
{
    RNetLobbyModPkg r;
    memset(&r, 0, sizeof(r));
    snprintf(r.id, sizeof(r.id), "%s", id);
    snprintf(r.ver, sizeof(r.ver), "%s", ver);
    return r;
}

static void case_gate_matches_on_id_only(RNetLobby *l)
{
    RNetLobbyModPkg plan[2], peer[2];
    char who[64], what[160];
    printf("  launch gate\n");
    plan[0] = row("gwed.localization", "1.0.0");
    peer[0] = row("gwed.localization", "2.5.1");
    ck(gate_with(l, plan, 1, peer, 1, who, sizeof(who), what, sizeof(what)) == 0,
       "a different version of the same package does NOT block the match");
    peer[0] = row("something.else", "1.0.0");
    ck(gate_with(l, plan, 1, peer, 1, who, sizeof(who), what, sizeof(what)) == 1,
       "a genuinely absent package blocks the match");
    ck(!strcmp(who, "Bob"), "the blocked player is named");
    ck(!strcmp(what, "gwed.localization@1.0.0"), "the missing package is named");
    plan[1] = row("gwed.enhancement.widescreen", "1.0.0");
    peer[0] = row("gwed.localization", "0.9");
    peer[1] = row("gwed.enhancement.widescreen", "9.9");
    ck(gate_with(l, plan, 2, peer, 2, who, sizeof(who), what, sizeof(what)) == 0,
       "every package present at any version starts the match");
    ck(gate_with(l, plan, 2, peer, 0, who, sizeof(who), what, sizeof(what)) == 2,
       "a peer announcing nothing is missing everything");
    ck(rnet_lobby_local_missing_mods(l) == 2,
       "our own seat, announcing nothing, is missing the plan too");
}

static int fake_export(const char *id, const char *ver, uint8_t **out,
                       uint32_t *out_len, char *sha, uint32_t sha_cap, char *err,
                       uint32_t err_cap, void *ctx)
{
    (void)id; (void)ver; (void)out; (void)out_len; (void)sha; (void)sha_cap;
    (void)err; (void)err_cap; (void)ctx;
    return 0;
}

static void fake_free(uint8_t *b)
{
    (void)b;
}

static int fake_install(const uint8_t *d, uint32_t n, const char *sha, char *id,
                        uint32_t ic, char *ver, uint32_t vc, char *err,
                        uint32_t ec, void *ctx)
{
    (void)d; (void)n; (void)sha; (void)id; (void)ic; (void)ver; (void)vc;
    (void)err; (void)ec; (void)ctx;
    return 0;
}

/* Build properties, not connection properties: they used to live in the
 * per-connection struct that every connect() wipes. */
static void case_hooks_survive_disconnect(RNetLobby *l)
{
    printf("  hooks survive reconnect\n");
    rnet_lobby_set_mod_transfer_hooks(l, fake_export, fake_free, fake_install, NULL);
    rnet_lobby_set_mod_offer_supplier(l, two_pkg_offer, NULL);
    rnet_lobby__test_attach(l, "me", sink, NULL);
    rnet_lobby_disconnect(l);
    ck(l->export_fn == fake_export, "export hook survives the disconnect");
    ck(l->install_fn == fake_install, "install hook survives too");
    ck(l->offer_fn == two_pkg_offer, "offer supplier survives too");
    rnet_lobby_set_mod_transfer_hooks(l, NULL, NULL, NULL, NULL);
    rnet_lobby_set_mod_offer_supplier(l, NULL, NULL);
}

static void case_ice_local_becomes_remote(void)
{
    printf("  ICE type translation\n");
    ck(rnet_lobby__mod_ice_type_for_push((int)RNET_SIGNAL_LOCAL_SDP) ==
           (int)RNET_SIGNAL_REMOTE_SDP,
       "a peer's LOCAL_SDP is pushed as REMOTE_SDP");
    ck(rnet_lobby__mod_ice_type_for_push((int)RNET_SIGNAL_LOCAL_CANDIDATE) ==
           (int)RNET_SIGNAL_REMOTE_CANDIDATE,
       "a peer's LOCAL_CANDIDATE is pushed as REMOTE_CANDIDATE");
    ck(rnet_lobby__mod_ice_type_for_push((int)RNET_SIGNAL_GATHERING_DONE) ==
           (int)RNET_SIGNAL_GATHERING_DONE, "GATHERING_DONE passes through");
    ck(rnet_lobby__mod_ice_type_for_push((int)RNET_SIGNAL_SET_CONTROLLING) ==
           (int)RNET_SIGNAL_SET_CONTROLLING, "SET_CONTROLLING passes through");
    ck(rnet_lobby__mod_ice_type_for_push((int)RNET_SIGNAL_REMOTE_SDP) ==
           (int)RNET_SIGNAL_REMOTE_SDP, "REMOTE_SDP is not translated twice");
}

/* The offer this peer SENDS must be readable by the code that RECEIVES one:
 * the two ends once disagreed ({"pkgs":[...]} vs a bare array) and every peer
 * looked empty-handed. Asserted end to end through a real lobby_update. */
static void case_offer_round_trips_through_a_slot_row(RNetLobby *l)
{
    char offer[RNET_LOBBY_MAX_MODS * RNET_LOBBY_MOD_ROW_JSON + 64];
    char upd[sizeof(offer) + 512];
    RNetLobbyMember m;
    printf("  offer round trip\n");
    rnet_lobby__test_attach(l, "me", sink, NULL);
    rnet_lobby_set_mod_offer_supplier(l, two_pkg_offer, NULL);
    ck(rnet_lobby__append_mod_offer(l, offer, sizeof(offer)) > 0, "offer encodes");
    ck(strncmp(offer, ",\"mod_offer\":{", 14) == 0,
       "the offer travels as an object, which is what the server accepts");
    snprintf(upd, sizeof(upd),
             "{\"op\":\"lobby_update\",\"player_count\":2,\"max_slots\":2,"
             "\"host_player_id\":\"me\",\"slots\":["
             "{\"slot\":0,\"player_id\":\"me\",\"display_name\":\"Me\",\"ready\":true},"
             "{\"slot\":1,\"player_id\":\"them\",\"display_name\":\"Bob\","
             "\"ready\":true%s}]}",
             offer);
    rnet_lobby__ingest(l, upd);
    ck(rnet_lobby_member_count(l) == 2, "both seats parse");
    ck(rnet_lobby_member_get(l, 1, &m) && m.mod_offer_count == 2,
       "both offered packages are read back from the echo");
    ck(!strcmp(l->c.member_offer[1][0].id, "gwed.localization"), "offer row 0 id");
    ck(!strcmp(l->c.member_offer[1][1].id, "gwed.enhancement.widescreen"),
       "offer row 1 id");
    ck(rnet_lobby_member_json(l, 1) &&
           strstr(rnet_lobby_member_json(l, 1), "\"mod_offer\"") != NULL,
       "the raw row keeps the offer for the title to read");
    rnet_lobby_set_mod_offer_supplier(l, NULL, NULL);
    rnet_lobby_disconnect(l);
}

static void case_mod_set_round_trips(RNetLobby *l)
{
    RNetLobbyMatchCaps caps, back;
    char json[RNET_LOBBY_CAPS_JSON_LEN];
    printf("  mod_set round trip\n");
    rnet_lobby_match_caps_init(l, &caps);
    caps.valid = 1;
    snprintf(caps.mod_set, sizeof(caps.mod_set),
             "gwed.enhancement.widescreen@1.0.0/widescreen;"
             "gwed.localization@1.0.0/localization language=en");
    snprintf(caps.mod_cosmetic_allow, sizeof(caps.mod_cosmetic_allow),
             "gwed.hud@1.0.0#%s",
             "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    ck(rnet_lobby_match_caps_encode(l, &caps, json, sizeof(json)) > 0, "caps encode");
    ck(rnet_lobby_match_caps_decode(l, json, &back) == 1, "caps decode");
    ck(!strcmp(back.mod_set, caps.mod_set),
       "the host's effective set survives the round trip verbatim");
    ck(strstr(back.mod_set, "language=en") != NULL, "resolved option values survive");
    ck(!strcmp(back.mod_cosmetic_allow, caps.mod_cosmetic_allow),
       "the exemption grant survives verbatim");
    ck(rnet_lobby_match_caps_decode(l, "{\"v\":1}", &back) == 1 &&
           back.mod_cosmetic_allow[0] == '\0',
       "an absent grant is empty: nothing is exempt");
}

#define ONE_MB (1024u * 1024u)

static void case_relay_cap(void)
{
    char why[256];
    const char *direct[] = { "host", "srflx", "prflx" };
    size_t i;
    printf("  relay cap\n");
    ck(RNET_LOBBY_MOD_RELAY_MAX_BYTES == 5u * ONE_MB, "the cap is 5 MiB");
    ck(rnet_lobby_mod_relay_size_allows("relay", 5u * ONE_MB, "m", why, sizeof(why)) == 1,
       "5 MB exactly still goes over the relay");
    ck(why[0] == '\0', "an allowed transfer writes no reason");
    ck(rnet_lobby_mod_relay_size_allows("relay", 5u * ONE_MB + 1u, "m", why,
                                        sizeof(why)) == 0,
       "one byte over the cap is refused on the relay");
    for (i = 0; i < sizeof(direct) / sizeof(direct[0]); ++i) {
        why[0] = 'x';
        ck(rnet_lobby_mod_relay_size_allows(direct[i], 400u * ONE_MB, "big", why,
                                            sizeof(why)) == 1, direct[i]);
        ck(why[0] == '\0', "no reason on an allowed direct transfer");
    }
    ck(rnet_lobby_mod_relay_size_allows("unknown", 400u * ONE_MB, "big", why,
                                        sizeof(why)) == 1,
       "an unnamed path does not refuse");
    ck(rnet_lobby_mod_relay_size_allows(NULL, 400u * ONE_MB, "big", why,
                                        sizeof(why)) == 1,
       "a NULL path does not refuse");
    ck(rnet_lobby_mod_relay_size_allows("none", 400u * ONE_MB, "big", why,
                                        sizeof(why)) == 1,
       "a build without ICE does not refuse");
    why[0] = '\0';
    (void)rnet_lobby_mod_relay_size_allows("relay", 12u * ONE_MB + 512u * 1024u,
                                           "gwed.enormous", why, sizeof(why));
    ck(strstr(why, "gwed.enormous") != NULL, "the reason names the package");
    ck(strstr(why, "12.5 MB") != NULL, "the reason states the actual size");
    ck(strstr(why, "5 MB") != NULL, "the reason states the cap");
    ck(strstr(why, "original source") != NULL, "the reason says where to get it");
    ck(strlen(why) < 256, "the reason fits the transfer error buffer");
    why[0] = '\0';
    (void)rnet_lobby_mod_relay_size_allows("relay", 9u * ONE_MB, NULL, why, sizeof(why));
    ck(strstr(why, "that mod") != NULL, "an unnamed package still reads");
    {
        char s[16];
        ck(rnet_lobby_mod_relay_size_allows("relay", 90u * ONE_MB, "m", s, sizeof(s)) == 0,
           "a truncated reason is still a refusal");
        ck(s[sizeof(s) - 1] == '\0', "the short reason stays terminated");
        ck(rnet_lobby_mod_relay_size_allows("relay", 90u * ONE_MB, "m", NULL, 0) == 0,
           "no reason buffer at all is still a refusal");
    }
}

static void attach_as(RNetLobby *l, const char *pid)
{
    rnet_lobby__test_attach(l, pid, sink, NULL);
}

static void case_spectator_rows_are_tagged(RNetLobby *l)
{
    const char *json =
        "{\"op\":\"lobby_update\",\"player_count\":2,\"max_slots\":2,"
        "\"allow_spectators\":true,\"max_spectators\":4,"
        "\"spectator_count\":2,\"spectator_slot_base\":64,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"Host\",\"ready\":true},"
        "{\"slot\":1,\"player_id\":\"g\",\"display_name\":\"Guest\",\"ready\":false}],"
        "\"spectators\":[{\"slot\":64,\"player_id\":\"s0\",\"display_name\":\"Watcher\",\"ready\":false},"
        "{\"slot\":66,\"player_id\":\"s2\",\"display_name\":\"Other\",\"ready\":false}]}";
    RNetLobbyConn *c = &l->c;
    printf("  spectator rows\n");
    attach_as(l, "s2");
    rnet_lobby__parse_slots(l, rnet_json_span(json));
    ck(c->member_count == 4, "both tables land in one membership list");
    ck(c->members[0].is_spectator == 0, "row 0 is a player");
    ck(c->members[1].is_spectator == 0, "row 1 is a player");
    ck(c->members[2].is_spectator == 1, "row 2 is a spectator");
    ck(c->members[3].is_spectator == 1, "row 3 is a spectator");
    ck(c->members[2].slot == 64, "spectator seat index is preserved");
    ck(c->members[3].slot == 66, "a sparse gallery keeps its indices");
    ck(!strcmp(c->members[3].display_name, "Other"), "names still parse");
    ck(rnet_lobby_local_is_spectator(l) == 1, "this client knows it is watching");
    ck(c->join.local_slot == 66, "and which seat it holds");
    ck(rnet_lobby_allow_spectators(l) == 1, "allow_spectators round-trips");
    ck(rnet_lobby_max_spectators(l) == 4, "max_spectators round-trips");
    ck(rnet_lobby_spectator_slot_base(l) == 64, "the base comes from the server");
}

static void case_a_player_is_not_a_spectator(RNetLobby *l)
{
    const char *json =
        "{\"allow_spectators\":true,\"max_spectators\":4,\"spectator_slot_base\":64,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"me\",\"display_name\":\"Me\",\"ready\":true}],"
        "\"spectators\":[{\"slot\":64,\"player_id\":\"s\",\"display_name\":\"S\",\"ready\":false}]}";
    printf("  player role\n");
    attach_as(l, "me");
    rnet_lobby__parse_slots(l, rnet_json_span(json));
    ck(rnet_lobby_local_is_spectator(l) == 0, "a seated player is not watching");
    ck(l->c.local_ready == 1, "and its own ready still tracks");
}

static void case_promotion_flips_the_role(RNetLobby *l)
{
    const char *watching =
        "{\"allow_spectators\":true,\"spectator_slot_base\":64,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"H\",\"ready\":false}],"
        "\"spectators\":[{\"slot\":64,\"player_id\":\"me\",\"display_name\":\"Me\",\"ready\":false}]}";
    const char *playing =
        "{\"allow_spectators\":true,\"spectator_slot_base\":64,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"H\",\"ready\":false},"
        "{\"slot\":1,\"player_id\":\"me\",\"display_name\":\"Me\",\"ready\":false}],"
        "\"spectators\":[]}";
    printf("  promotion\n");
    attach_as(l, "me");
    rnet_lobby__parse_slots(l, rnet_json_span(watching));
    ck(rnet_lobby_local_is_spectator(l) == 1, "watching first");
    rnet_lobby__parse_slots(l, rnet_json_span(playing));
    ck(rnet_lobby_local_is_spectator(l) == 0, "playing after the host moved us");
    ck(l->c.join.local_slot == 1, "and holding the seat we were moved to");
    ck(l->c.member_count == 2, "the empty gallery contributes no rows");
}

static void case_a_server_without_spectators_reads_as_before(RNetLobby *l)
{
    const char *json =
        "{\"player_count\":2,\"max_slots\":2,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"H\",\"ready\":true},"
        "{\"slot\":1,\"player_id\":\"me\",\"display_name\":\"Me\",\"ready\":true}]}";
    printf("  legacy server\n");
    attach_as(l, "me");
    rnet_lobby__parse_slots(l, rnet_json_span(json));
    ck(l->c.member_count == 2, "both players parse");
    ck(l->c.members[0].is_spectator == 0 && l->c.members[1].is_spectator == 0,
       "nobody is tagged as a spectator");
    ck(rnet_lobby_allow_spectators(l) == 0, "and the gallery is reported closed");
    ck(rnet_lobby_local_is_spectator(l) == 0, "so this client is a player");
}

static void case_launch_does_not_erase_the_gallery(RNetLobby *l)
{
    const char *update =
        "{\"allow_spectators\":true,\"max_spectators\":4,\"spectator_slot_base\":64,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"H\",\"ready\":true}],"
        "\"spectators\":[{\"slot\":64,\"player_id\":\"me\",\"display_name\":\"Me\",\"ready\":false}]}";
    const char *launch =
        "{\"op\":\"launch\",\"player_count\":1,\"spectator_count\":1,"
        "\"spectator_slot_base\":64,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"H\",\"ready\":true}],"
        "\"spectators\":[{\"slot\":64,\"player_id\":\"me\",\"display_name\":\"Me\",\"ready\":false}]}";
    printf("  launch keeps the role\n");
    attach_as(l, "me");
    rnet_lobby__parse_slots(l, rnet_json_span(update));
    ck(rnet_lobby_allow_spectators(l) == 1, "gallery open in the lobby");
    rnet_lobby__parse_slots(l, rnet_json_span(launch));
    ck(rnet_lobby_allow_spectators(l) == 1, "still open through launch");
    ck(rnet_lobby_max_spectators(l) == 4, "and the size is remembered");
    ck(rnet_lobby_local_is_spectator(l) == 1, "we launch as a spectator");
}

static void case_gallery_seat_addressing(RNetLobby *l)
{
    printf("  seat addressing\n");
    attach_as(l, "me");
    l->c.join.spectator_slot_base = 64;
    ck(rnet_lobby_spectator_slot(l, 0) == 64, "gallery 0 addresses seat 64");
    ck(rnet_lobby_spectator_slot(l, 3) == 67, "gallery 3 addresses seat 67");
    ck(rnet_lobby_spectator_slot(l, 4) == -1, "past the gallery is refused");
    ck(rnet_lobby_spectator_slot(l, -1) == -1, "and so is a negative index");
    ck(RNET_LOBBY_MAX_PLAYERS < RNET_LOBBY_DEFAULT_SPECTATOR_SLOT_BASE,
       "the two halves of the namespace cannot collide");
    ck(rnet_lobby_seat_valid(l, 3) == 1 && rnet_lobby_seat_valid(l, 4) == 0,
       "player seats end at the title's max_players");
    ck(rnet_lobby_seat_valid(l, 67) == 1 && rnet_lobby_seat_valid(l, 68) == 0,
       "gallery seats end at the title's max_spectators");
}

static void case_the_gallery_does_not_negotiate(RNetLobby *l)
{
    const char *json =
        "{\"op\":\"lobby_update\",\"player_count\":2,\"max_slots\":2,"
        "\"allow_spectators\":true,\"max_spectators\":4,"
        "\"spectator_count\":1,\"spectator_slot_base\":64,"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"Host\",\"ready\":true},"
        "{\"slot\":1,\"player_id\":\"g\",\"display_name\":\"Guest\",\"ready\":false}],"
        "\"spectators\":[{\"slot\":64,\"player_id\":\"s0\",\"display_name\":\"Watcher\",\"ready\":false}]}";
    printf("  gallery does not negotiate\n");
    attach_as(l, "h");
    rnet_lobby__parse_slots(l, rnet_json_span(json));
    ck(rnet_lobby__ice_signal_is_for_us(l, (int)RNET_SIGNAL_LOCAL_SDP, "g") == 1,
       "the other player's SDP is the one we negotiate with");
    ck(rnet_lobby__ice_signal_is_for_us(l, (int)RNET_SIGNAL_LOCAL_CANDIDATE, "g") == 1,
       "and so are its candidates");
    ck(rnet_lobby__ice_signal_is_for_us(l, (int)RNET_SIGNAL_LOCAL_SDP, "s0") == 0,
       "a spectator's SDP is refused");
    ck(rnet_lobby__ice_signal_is_for_us(l, (int)RNET_SIGNAL_LOCAL_CANDIDATE, "s0") == 0,
       "and its candidates with it");
    ck(rnet_lobby__ice_signal_is_for_us(l, (int)RNET_SIGNAL_GATHERING_DONE, "s0") == 0,
       "every ICE type is filtered, not just the SDP");
    ck(rnet_lobby__ice_signal_is_for_us(l, (int)RNET_SIGNAL_LOCAL_SDP, "") == 1,
       "an unattributable sender is handled as before");
    ck(rnet_lobby__ice_signal_is_for_us(l, (int)RNET_SIGNAL_LOCAL_SDP, NULL) == 1,
       "and so is a missing one");
    ck(rnet_lobby__ice_signal_is_for_us(l, RNET_LOBBY_SIG_RTT_PING, "s0") == 1,
       "a non-ICE signal is not judged by an ICE rule");
    attach_as(l, "s0");
    rnet_lobby__parse_slots(l, rnet_json_span(json));
    ck(rnet_lobby_local_is_spectator(l) == 1, "this client is watching");
    ck(rnet_lobby__ice_signal_is_for_us(l, (int)RNET_SIGNAL_LOCAL_SDP, "h") == 0,
       "a spectator ingests nothing, not even the host's offer");
    ck(rnet_lobby__ice_signal_is_for_us(l, (int)RNET_SIGNAL_LOCAL_SDP, "g") == 0,
       "nor the other player's");
}

static void case_launch_transport_survives_a_lobby_update(RNetLobby *l)
{
    const char *launch =
        "{\"op\":\"launch\",\"ok\":true,\"lobby_id\":\"L\",\"session_id\":8,"
        "\"host_endpoint\":\"relay.example:8777\","
        "\"guest_endpoint\":\"relay.example:8777\","
        "\"relay_endpoint\":\"relay.example:8777\",\"transport\":\"sfu\","
        "\"player_count\":2,\"max_slots\":2,\"spectator_relay_base\":2,"
        "\"match_caps\":{\"v\":1,\"input_delay\":9,\"force_input_relay\":false},"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"Host\"},"
        "{\"slot\":1,\"player_id\":\"g\",\"display_name\":\"Guest\"}]}";
    const char *update =
        "{\"op\":\"lobby_update\",\"lobby_id\":\"L\",\"player_count\":2,"
        "\"max_slots\":2,\"host_endpoint\":\"relay.example:8777\","
        "\"guest_endpoint\":\"relay.example:8777\","
        "\"match_caps\":{\"v\":1,\"input_delay\":9,\"force_input_relay\":false},"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"Host\"},"
        "{\"slot\":1,\"player_id\":\"g\",\"display_name\":\"Guest\"}]}";
    RNetLobbyJoinInfo out;
    printf("  launch transport survives lobby traffic\n");
    attach_as(l, "g");
    rnet_lobby__ingest(l, launch);
    ck(l->c.join.force_input_relay == 1,
       "a launch carrying relay_endpoint is a relayed match");
    ck(!strcmp(l->c.join.peer_hostport, "relay.example:8777"),
       "and everyone dials the relay");
    ck(!strcmp(l->c.join.bind_hostport, "0.0.0.0:0"), "from an ephemeral local bind");
    rnet_lobby__ingest(l, update);
    ck(l->c.match_caps.force_input_relay == 0,
       "the host's published caps still say what the host's toggle says");
    ck(l->c.join.force_input_relay == 1,
       "but the match is still relayed -- the launch decided that, not the caps");
    ck(rnet_lobby_try_fill_launch(l, &out) == 1, "the launch still fills");
    ck(out.force_input_relay == 1, "so the session starts on the relay, not p2p");
    rnet_lobby__ingest(l,
        "{\"op\":\"launch\",\"ok\":true,\"lobby_id\":\"L\",\"session_id\":9,"
        "\"host_endpoint\":\"1.2.3.4:5000\",\"guest_endpoint\":\"5.6.7.8:6000\","
        "\"transport\":\"ice_p2p\",\"player_count\":2,\"max_slots\":2,"
        "\"match_caps\":{\"v\":1,\"input_delay\":9,\"force_input_relay\":false},"
        "\"slots\":[{\"slot\":0,\"player_id\":\"h\",\"display_name\":\"Host\"},"
        "{\"slot\":1,\"player_id\":\"g\",\"display_name\":\"Guest\"}]}");
    ck(l->c.join.force_input_relay == 0,
       "a p2p launch does not inherit the previous match's relay");
}

static void case_chat_ring(RNetLobby *l)
{
    RNetLobbyChatMsg got;
    uint32_t a = 0, b = 0, before;
    char buf[32];
    int i;
    printf("  chat ring\n");
    attach_as(l, "me");
    rnet_lobby__chat_push(l, &l->c.chat, "them", "", "Them", "", "first", "", 0);
    rnet_lobby__chat_push(l, &l->c.chat, "me", "", "Me", "", "second", "", 0);
    rnet_lobby__chat_push(l, &l->c.chat, "", "", "", "", "third", "", 1);
    ck(rnet_lobby_chat_count(l) == 3, "three lines land");
    ck(rnet_lobby_chat_get(l, 0, &got) && !strcmp(got.text, "first"),
       "index 0 is the oldest");
    ck(rnet_lobby_chat_get(l, 2, &got) && !strcmp(got.text, "third"),
       "index 2 is the newest");
    ck(rnet_lobby_chat_get(l, 0, &got) && got.is_local == 0, "a peer's line is not local");
    ck(rnet_lobby_chat_get(l, 1, &got) && got.is_local == 1, "our own echoed line is local");
    ck(rnet_lobby_chat_get(l, 2, &got) && got.is_system == 1 && got.is_local == 0,
       "a system line is neither ours nor a player's");
    (void)rnet_lobby_chat_get(l, 0, &got);
    a = got.seq;
    (void)rnet_lobby_chat_get(l, 2, &got);
    b = got.seq;
    ck(b > a, "seq increases with arrival order");
    ck(!rnet_lobby_chat_get(l, 3, &got), "reading past the end is refused");
    ck(!rnet_lobby_chat_get(l, -1, &got), "so is a negative index");

    attach_as(l, "me");
    for (i = 0; i < RNET_LOBBY_CHAT_RING + 10; ++i) {
        snprintf(buf, sizeof(buf), "line%d", i);
        rnet_lobby__chat_push(l, &l->c.chat, "them", "", "Them", "", buf, "", 0);
    }
    ck(rnet_lobby_chat_count(l) == RNET_LOBBY_CHAT_RING, "the ring stops at its capacity");
    ck(rnet_lobby_chat_get(l, 0, &got) && !strcmp(got.text, "line10"),
       "the oldest surviving line is the 11th sent");
    snprintf(buf, sizeof(buf), "line%d", RNET_LOBBY_CHAT_RING + 9);
    ck(rnet_lobby_chat_get(l, RNET_LOBBY_CHAT_RING - 1, &got) && !strcmp(got.text, buf),
       "the newest line is the last one sent");

    attach_as(l, "me");
    rnet_lobby__chat_push(l, &l->c.chat, "them", "", "Them", "", "", "", 0);
    rnet_lobby__chat_push(l, &l->c.chat, "them", "", "Them", "", NULL, "", 0);
    ck(rnet_lobby_chat_count(l) == 0, "an empty line is not a line");
    rnet_lobby__chat_push(l, &l->c.chat, "them", "", "Them", "", "hello", "", 0);
    (void)rnet_lobby_chat_get(l, 0, &got);
    before = got.seq;
    rnet_lobby_chat_clear(l);
    ck(rnet_lobby_chat_count(l) == 0, "clear empties the room log");
    rnet_lobby__chat_push(l, &l->c.chat, "them", "", "Them", "", "new room", "", 0);
    (void)rnet_lobby_chat_get(l, 0, &got);
    ck(got.seq > before, "seq keeps counting across a clear");
    rnet_lobby_disconnect(l);
}

int main(void)
{
    RNetLobby *l = open_quiet();
    case_rows(l);
    case_empty(l);
    case_old_string_encoding_is_not_revived();
    case_partial_rows_dropped();
    case_overflow_refuses(l);
    case_gate_matches_on_id_only(l);
    case_hooks_survive_disconnect(l);
    case_ice_local_becomes_remote();
    case_offer_round_trips_through_a_slot_row(l);
    case_mod_set_round_trips(l);
    case_relay_cap();
    case_spectator_rows_are_tagged(l);
    case_a_player_is_not_a_spectator(l);
    case_promotion_flips_the_role(l);
    case_a_server_without_spectators_reads_as_before(l);
    case_launch_does_not_erase_the_gallery(l);
    case_gallery_seat_addressing(l);
    case_the_gallery_does_not_negotiate(l);
    case_launch_transport_survives_a_lobby_update(l);
    case_chat_ring(l);
    rnet_lobby_close(&l);
    ck(l == NULL, "close clears the handle");
    printf(fails ? "\n%d failure(s)\n" : "\nall mod-plan cases passed\n", fails);
    return fails != 0;
}
