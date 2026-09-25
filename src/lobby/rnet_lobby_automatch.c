/* rnet_lobby_automatch.c -- server-run pairing (recomp-net-server
 * docs/AUTOMATCH.md): the ticket, the accept gate, and the relay latency
 * probe. The room a pairing produces arrives as an ordinary `joined`.
 *
 * Merged from both engines. The psxrecomp copy parsed `automatch_found` as
 * the server sends it (match_id, ruleset_label, opponent as an OBJECT
 * {handle, discord_username, country}); the snesrecomp copy read a flat
 * "opponent" string and "label", both of which came back empty against the
 * current server. Both shapes are read here, the documented one first.
 */
#include "platform/rnet_platform.h"
#include "lobby/rnet_lobby_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#define PROBE_LEN 14

void rnet_lobby__am_reset_queue(RNetLobby *l)
{
    RNetLobbyAutomatch *a = &l->c.am;
    a->state = RNET_LOBBY_AUTOMATCH_IDLE;
    a->queue_in_flight = 0;
    a->ticket_id[0] = '\0';
    a->match_id[0] = '\0';
    a->queued_secs = 0;
    a->pool = 0;
    a->rtt_reported = 0;
    a->found_deadline_ms = 0;
    memset(&a->found, 0, sizeof(a->found));
    a->found_caps.valid = 0;
}

static void am_fail(RNetLobby *l, const char *why)
{
    RNetLobbyAutomatch *a = &l->c.am;
    a->state = RNET_LOBBY_AUTOMATCH_FAILED;
    a->queue_in_flight = 0;
    snprintf(a->error, sizeof(a->error), "%s", why ? why : "automatch failed");
    LOBBY_WARN(l, "automatch failed: %s", a->error);
}

static void probe_close(RNetLobby *l)
{
    RNetLobbyAutomatch *a = &l->c.am;
    if (a->probe_socket >= 0) {
        rnet_lobby__socket_close(a->probe_socket);
        a->probe_socket = -1;
    }
    a->probe_sent_ms = 0;
}

void rnet_lobby__am_close(RNetLobby *l)
{
    probe_close(l);
}

/* The server holds the ticket and the ruleset answer per socket: a new
 * connection re-asks rather than answering from a stale yes. */
void rnet_lobby__am_on_connection_reset(RNetLobby *l)
{
    RNetLobbyAutomatch *a = &l->c.am;
    rnet_lobby__am_reset_queue(l);
    probe_close(l);
    a->have_rulesets = 0;
    a->rulesets_in_flight = 0;
    a->ruleset_count = 0;
    a->rtt_ms = -1;
    a->in_automatch_room = 0;
    a->error[0] = '\0';
}

/* A ticket that reached a room is spent. Recorded before the reset, which
 * erases the evidence: ACCEPTED (or FOUND, when the pair resolved in the same
 * breath) is the only sign this seat came from a queue. */
void rnet_lobby__am_on_joined(RNetLobby *l)
{
    RNetLobbyAutomatch *a = &l->c.am;
    a->in_automatch_room = (a->state == RNET_LOBBY_AUTOMATCH_ACCEPTED ||
                            a->state == RNET_LOBBY_AUTOMATCH_FOUND);
    rnet_lobby__am_reset_queue(l);
}

void rnet_lobby__am_on_left(RNetLobby *l)
{
    /* Leaving a room must never leave a ticket looking live, and no seat
     * means no automatch room. */
    rnet_lobby__am_reset_queue(l);
    l->c.am.in_automatch_room = 0;
}

/* The 14-byte probe: magic, type, nonce -- LITTLE-endian like the relay's
 * header (input_relay.rs read_u32_le). The nonce sits where a session id
 * would; the relay echoes it untouched and rewrites only the type. */
static void probe_pack(const RNetLobbyAutomatch *a, unsigned char *out,
                       uint32_t nonce)
{
    const unsigned magic = a->probe_magic;
    const int type = a->probe_type ? a->probe_type : 200;
    memset(out, 0, PROBE_LEN);
    out[0] = (unsigned char)(magic & 0xFF);
    out[1] = (unsigned char)((magic >> 8) & 0xFF);
    out[2] = (unsigned char)((magic >> 16) & 0xFF);
    out[3] = (unsigned char)((magic >> 24) & 0xFF);
    out[4] = (unsigned char)(type & 0xFF);
    out[5] = (unsigned char)((type >> 8) & 0xFF);
    out[6] = (unsigned char)(nonce & 0xFF);
    out[7] = (unsigned char)((nonce >> 8) & 0xFF);
    out[8] = (unsigned char)((nonce >> 16) & 0xFF);
    out[9] = (unsigned char)((nonce >> 24) & 0xFF);
}

static int probe_send(RNetLobby *l)
{
    RNetLobbyAutomatch *a = &l->c.am;
    struct addrinfo hints, *res = NULL;
    unsigned char pkt[PROBE_LEN];
    char portstr[16];
    int fd;
    if (!a->probe_host[0] || a->probe_port <= 0)
        return -1;
    if (a->probe_sent_ms)
        return 0; /* one outstanding */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(portstr, sizeof(portstr), "%d", a->probe_port);
    rnet_os_startup();
    if (getaddrinfo(a->probe_host, portstr, &hints, &res) != 0 || !res)
        return -1;
#if defined(_WIN32)
    {
        SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == INVALID_SOCKET) {
            freeaddrinfo(res);
            return -1;
        }
        fd = (int)s;
    }
#else
    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
#endif
    rnet_lobby__set_nonblock(fd);
    /* A fresh nonce per attempt: a late reply to an older probe must not be
     * timed against this clock. */
    {
        uint32_t r = 0;
        if (rnet_os_random_bytes(&r, sizeof(r)) != 0)
            r = (uint32_t)(rnet_lobby__now_ms() * 2654435761u) ^ 0x9E3779B9u;
        a->probe_nonce = r;
    }
    probe_pack(a, pkt, a->probe_nonce);
#if defined(_WIN32)
    if (sendto((SOCKET)fd, (const char *)pkt, (int)sizeof(pkt), 0, res->ai_addr,
               (int)res->ai_addrlen) < 0) {
#else
    if (sendto(fd, pkt, sizeof(pkt), 0, res->ai_addr, res->ai_addrlen) < 0) {
#endif
        rnet_lobby__socket_close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    probe_close(l);
    a->probe_socket = fd;
    a->probe_sent_ms = rnet_lobby__now_ms();
    if (!a->probe_sent_ms)
        a->probe_sent_ms = 1;
    return 0;
}

static void send_rtt(RNetLobby *l)
{
    RNetLobbyAutomatch *a = &l->c.am;
    char msg[96];
    if (a->rtt_ms < 0 || a->rtt_reported)
        return;
    snprintf(msg, sizeof(msg), "{\"op\":\"automatch_rtt\",\"rtt_ms\":%d}",
             a->rtt_ms);
    if (rnet_lobby__send(l, msg) == 0)
        a->rtt_reported = 1;
}

/* Every pump: time the 201 reply, or give up after 2 s (a ticket without a
 * measurement is held out of pairing for the server's grace, then matches
 * anyway -- a silent relay delays a match, never prevents one). */
void rnet_lobby__am_poll(RNetLobby *l)
{
    RNetLobbyAutomatch *a = &l->c.am;
    unsigned char buf[64];
    uint64_t now;
    if (a->probe_socket < 0 || !a->probe_sent_ms)
        return;
    now = rnet_lobby__now_ms();
    for (;;) {
#if defined(_WIN32)
        int n = recv((SOCKET)a->probe_socket, (char *)buf, (int)sizeof(buf), 0);
#else
        int n = (int)recv(a->probe_socket, buf, sizeof(buf), 0);
#endif
        uint32_t echo;
        if (n < 0)
            break;
        if (n < 10)
            continue;
        /* Unconnected socket: an unrelated packet timed as our reply would be
         * a wrong number reported as fact. */
        echo = (uint32_t)buf[6] | (uint32_t)buf[7] << 8 |
               (uint32_t)buf[8] << 16 | (uint32_t)buf[9] << 24;
        if (echo != a->probe_nonce)
            continue;
        a->rtt_ms = (int)(now - a->probe_sent_ms);
        if (a->rtt_ms < 0) a->rtt_ms = 0;
        if (a->rtt_ms > 2000) a->rtt_ms = 2000; /* the server clamps too */
        LOBBY_INFO(l, "automatch probe %s:%d rtt=%d ms", a->probe_host,
                   a->probe_port, a->rtt_ms);
        probe_close(l);
        /* Already queued on an unknown latency: tell the server now rather
         * than letting its probe grace lapse. */
        if (a->state == RNET_LOBBY_AUTOMATCH_QUEUED)
            send_rtt(l);
        return;
    }
    if (now - a->probe_sent_ms > 2000) {
        LOBBY_INFO(l, "automatch probe timed out (%s:%d) -- queueing without a "
                      "latency estimate", a->probe_host, a->probe_port);
        probe_close(l);
    }
}

/* `probe: { endpoint, magic, type }`, from rulesets_ok and queued; the last
 * one wins. */
static void ingest_probe(RNetLobby *l, RNetJsonSpan msg)
{
    RNetLobbyAutomatch *a = &l->c.am;
    RNetJsonSpan obj;
    char endpoint[160];
    char *colon;
    if (!rnet_json_obj(msg, "probe", &obj))
        return;
    rnet_json_str(obj, "endpoint", endpoint, sizeof(endpoint));
    colon = strrchr(endpoint, ':'); /* IPv6 has several; the port is last */
    if (!colon || !colon[1])
        return;
    *colon = '\0';
    if (endpoint[0] == '[' && colon > endpoint + 1 && colon[-1] == ']') {
        colon[-1] = '\0';
        memmove(endpoint, endpoint + 1, strlen(endpoint + 1) + 1);
    }
    snprintf(a->probe_host, sizeof(a->probe_host), "%s", endpoint);
    a->probe_port = atoi(colon + 1);
    a->probe_magic = (unsigned)rnet_json_i64(obj, "magic", 0);
    a->probe_type = rnet_json_int(obj, "type", 200);
}

/* `titles: [ { ..., pool } ]` -- one title queued, so the first row. */
static int first_pool(RNetLobby *l, RNetJsonSpan msg)
{
    RNetJsonSpan arr, row;
    if (!rnet_json_arr(msg, "titles", &arr))
        return l->c.am.pool;
    if (!rnet_json_arr_next(&arr, &row) || rnet_json_kind(row) != '{')
        return 0;
    return rnet_json_int(row, "pool", 0);
}

static void decode_caps_member(RNetLobby *l, RNetJsonSpan obj, const char *key,
                               RNetLobbyMatchCaps *out)
{
    RNetJsonSpan caps;
    char buf[RNET_LOBBY_CAPS_JSON_LEN * 2];
    rnet_lobby_match_caps_init(l, out);
    if (!rnet_json_obj(obj, key, &caps) || !rnet_json_copy(caps, buf, sizeof(buf)))
        return;
    rnet_lobby_match_caps_decode(l, buf, out);
}

int rnet_lobby__am_handle_op(RNetLobby *l, const char *op, RNetJsonSpan m)
{
    RNetLobbyAutomatch *a = &l->c.am;
    if (!strcmp(op, "automatch_rulesets_ok")) {
        RNetJsonSpan arr, row;
        int n = 0;
        a->have_rulesets = 1;
        a->rulesets_in_flight = 0;
        a->ruleset_count = 0;
        ingest_probe(l, m);
        if (rnet_json_arr(m, "rulesets", &arr)) {
            while (n < RNET_LOBBY_MAX_RULESETS && rnet_json_arr_next(&arr, &row)) {
                RNetLobbyRuleset *r = &a->rulesets[n];
                if (rnet_json_kind(row) != '{')
                    continue;
                memset(r, 0, sizeof(*r));
                rnet_json_str(row, "id", r->id, sizeof(r->id));
                rnet_json_str(row, "label", r->label, sizeof(r->label));
                rnet_json_str(row, "caps_summary", r->caps_summary,
                              sizeof(r->caps_summary));
                rnet_json_str(row, "game_version", r->game_version,
                              sizeof(r->game_version));
                r->max_slots = rnet_json_int(row, "max_slots", 2);
                /* The server is the host of an automatch room: its caps are a
                 * host's blob. */
                decode_caps_member(l, row, "match_caps", &r->caps);
                if (r->id[0])
                    ++n;
            }
        }
        a->ruleset_count = n;
        LOBBY_INFO(l, "automatch %d ruleset(s) for \"%s\"", n, l->game_name);
        /* Measure now: a client that probed before queueing never waits out
         * the server's probe grace. */
        if (n > 0 && a->rtt_ms < 0)
            (void)probe_send(l);
        return 1;
    }
    if (!strcmp(op, "automatch_queued")) {
        a->state = RNET_LOBBY_AUTOMATCH_QUEUED;
        a->error[0] = '\0';
        a->queue_in_flight = 0;
        rnet_json_str(m, "ticket_id", a->ticket_id, sizeof(a->ticket_id));
        a->queued_secs = 0;
        a->pool = first_pool(l, m);
        ingest_probe(l, m);
        if (a->rtt_ms < 0)
            (void)probe_send(l);
        else
            send_rtt(l);
        LOBBY_INFO(l, "automatch queued (pool=%d)", a->pool);
        return 1;
    }
    if (!strcmp(op, "automatch_status")) {
        /* At most 1 Hz; never resurrects a ticket we gave up on. */
        if (a->state != RNET_LOBBY_AUTOMATCH_QUEUED)
            return 1;
        a->queued_secs = rnet_json_int(m, "queued_secs", a->queued_secs);
        a->pool = first_pool(l, m);
        return 1;
    }
    if (!strcmp(op, "automatch_found")) {
        RNetLobbyAutomatchFound *f = &a->found;
        RNetJsonSpan opp;
        memset(f, 0, sizeof(*f));
        rnet_json_str(m, "match_id", a->match_id, sizeof(a->match_id));
        if (!a->match_id[0])
            rnet_json_str(m, "ticket_id", a->match_id, sizeof(a->match_id));
        if (rnet_json_obj(m, "opponent", &opp)) {
            /* Read from the opponent object: its "country" is theirs, and a
             * top-level lookup must never answer for it. */
            rnet_json_str(opp, "handle", f->opponent, sizeof(f->opponent));
            rnet_json_str(opp, "discord_username", f->opponent_username,
                          sizeof(f->opponent_username));
            rnet_json_str(opp, "country", f->opponent_country,
                          sizeof(f->opponent_country));
        } else {
            rnet_json_str(m, "opponent", f->opponent, sizeof(f->opponent));
            rnet_json_str(m, "opponent_country", f->opponent_country,
                          sizeof(f->opponent_country));
        }
        rnet_json_str(m, "ruleset_id", f->ruleset_id, sizeof(f->ruleset_id));
        if (rnet_json_str(m, "ruleset_label", f->ruleset_label,
                          sizeof(f->ruleset_label)) == 0)
            rnet_json_str(m, "label", f->ruleset_label, sizeof(f->ruleset_label));
        rnet_json_str(m, "game_version", f->game_version, sizeof(f->game_version));
        f->est_rtt_ms = rnet_json_int(m, "est_rtt_ms", -1);
        f->accept_secs = rnet_json_int(m, "accept_secs", 15);
        /* The floored delay the player is agreeing to, not the ruleset's
         * advertised one. */
        f->input_delay = rnet_json_int(m, "input_delay", -1);
        f->input_prediction = rnet_json_int(m, "input_prediction", -1);
        f->frames_needed = rnet_json_int(m, "frames_needed", -1);
        decode_caps_member(l, m, "match_caps", &a->found_caps);
        /* A deadline, not a number: a stored "15" would read as a frozen
         * dialog for fifteen seconds. */
        a->found_deadline_ms =
            rnet_lobby__now_ms() +
            (uint64_t)(f->accept_secs > 0 ? f->accept_secs : 0) * 1000ull;
        a->state = RNET_LOBBY_AUTOMATCH_FOUND;
        LOBBY_INFO(l, "automatch found opponent=\"%s\" est_rtt=%d ms, %d s to "
                      "answer", f->opponent, f->est_rtt_ms, f->accept_secs);
        return 1;
    }
    if (!strcmp(op, "automatch_accept_ok")) {
        /* Honour WHICH answer was acknowledged: a player who declined must
         * not have the gate reopen on the ack. */
        if (rnet_json_bool(m, "accept", 1)) {
            if (a->state == RNET_LOBBY_AUTOMATCH_FOUND ||
                a->state == RNET_LOBBY_AUTOMATCH_ACCEPTED)
                a->state = RNET_LOBBY_AUTOMATCH_ACCEPTED;
        } else {
            rnet_lobby__am_reset_queue(l);
        }
        return 1;
    }
    if (!strcmp(op, "automatch_requeue")) {
        char reason[32];
        rnet_json_str(m, "reason", reason, sizeof(reason));
        /* The other side declined, lapsed or left: back to the FRONT with the
         * ticket intact. Not a failure. `queued:false` (lobby_limit) is the
         * one case the ticket is gone. */
        if (rnet_json_bool(m, "queued", strcmp(reason, "lobby_limit") != 0)) {
            a->state = RNET_LOBBY_AUTOMATCH_QUEUED;
            memset(&a->found, 0, sizeof(a->found));
            a->found_caps.valid = 0;
            a->found_deadline_ms = 0;
            a->pool = first_pool(l, m);
            LOBBY_INFO(l, "automatch re-queued (%s)",
                       reason[0] ? reason : "the offer lapsed");
        } else {
            am_fail(l, "The server is out of rooms -- try again shortly");
        }
        return 1;
    }
    if (!strcmp(op, "automatch_cancelled")) {
        char reason[32];
        int cooldown;
        rnet_json_str(m, "reason", reason, sizeof(reason));
        cooldown = rnet_json_int(m, "cooldown_secs", 0);
        rnet_lobby__am_reset_queue(l);
        /* A lapsed or declined offer is worth a line: the gate otherwise
         * vanishes and the cooldown it cost is invisible. */
        if (!strcmp(reason, "timeout") || !strcmp(reason, "declined")) {
            if (cooldown > 0) {
                char line[96];
                snprintf(line, sizeof(line), "%d Second Cooldown For Declining",
                         cooldown);
                am_fail(l, line);
            } else if (!strcmp(reason, "timeout")) {
                am_fail(l, "The offer lapsed before you answered");
            }
        }
        return 1;
    }
    if (!strcmp(op, "automatch_rtt_ok"))
        return 1;
    return 0;
}

/* A refusal arrives as a plain `error`; claimed only while an attempt is in
 * flight, because `error` is everyone's. */
int rnet_lobby__am_claim_error(RNetLobby *l, const char *code, RNetJsonSpan m)
{
    RNetLobbyAutomatch *a = &l->c.am;
    const char *why = NULL;
    char line[160];
    if (!(a->queue_in_flight || a->state == RNET_LOBBY_AUTOMATCH_QUEUED ||
          a->state == RNET_LOBBY_AUTOMATCH_FOUND ||
          a->state == RNET_LOBBY_AUTOMATCH_ACCEPTED))
        return 0;
    if (!strcmp(code, "need_account")) why = "Sign in to use automatch";
    else if (!strcmp(code, "automatch_off")) why = "This server has no automatch queues";
    else if (!strcmp(code, "already_queued")) why = "This account is already in a queue";
    else if (!strcmp(code, "already_in_lobby")) why = "Leave the room first";
    else if (!strcmp(code, "unknown_ruleset")) why = "That queue type is gone -- refresh";
    else if (!strcmp(code, "need_disc_fp"))
        why = "The server needs a content fingerprint this build did not send";
    else if (!strcmp(code, "version_not_pooled"))
        why = "This release is not the one this queue pools";
    else if (!strcmp(code, "mods_not_pooled")) why = "Turn off sim-affecting mods to queue";
    else if (!strcmp(code, "mod_not_approved"))
        why = "A mod exemption is not on this queue's approved list";
    else if (!strcmp(code, "slots_not_pooled")) why = "Automatch is two-player only";
    else if (!strcmp(code, "queue_full")) why = "The queue is full -- try again shortly";
    else if (!strcmp(code, "cooldown")) {
        int retry = rnet_json_int(m, "retry_secs", 0);
        if (retry > 0)
            snprintf(line, sizeof(line), "%d Second Cooldown For Declining", retry);
        else
            snprintf(line, sizeof(line), "Cooldown For Declining");
        why = line;
    }
    if (!why)
        return 0;
    am_fail(l, why);
    return 1;
}

/* ── public ──────────────────────────────────────────────────────────────── */

int rnet_lobby_automatch_request_rulesets(RNetLobby *l)
{
    char msg[256];
    char gn_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_NAME_LEN)];
    if (!l || !rnet_lobby_connected(l) || !l->game_name[0])
        return -1;
    if (l->c.am.rulesets_in_flight)
        return 0; /* the launcher polls every frame */
    rnet_json_escape(l->game_name, gn_esc, sizeof(gn_esc));
    snprintf(msg, sizeof(msg), "{\"op\":\"automatch_rulesets\",\"game_name\":\"%s\"}",
             gn_esc);
    if (rnet_lobby__send(l, msg) != 0)
        return -1;
    l->c.am.rulesets_in_flight = 1;
    return 0;
}

int rnet_lobby_automatch_available(RNetLobby *l)
{
    /* Not having asked is "no": the button is never offered on an
     * assumption. Zero rulesets is also "no". */
    return l && l->c.am.have_rulesets && l->c.am.ruleset_count > 0;
}

int rnet_lobby_automatch_ruleset_count(RNetLobby *l)
{
    return l ? l->c.am.ruleset_count : 0;
}

int rnet_lobby_automatch_ruleset_get(RNetLobby *l, int index, RNetLobbyRuleset *out)
{
    if (!l || !out || index < 0 || index >= l->c.am.ruleset_count)
        return 0;
    *out = l->c.am.rulesets[index];
    return 1;
}

int rnet_lobby_automatch_queue(RNetLobby *l, const char *ruleset_id,
                               int mods_enabled, const char *mod_exempt)
{
    RNetLobbyAutomatch *a;
    char msg[2560];
    char exempt_json[1024];
    char gn_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_NAME_LEN)];
    char gv_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_VERSION_LEN)];
    char rid_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_RULESET_ID_LEN)];
    char rtt[48];
    const char *rid;
    int max_slots = 2, i, n;
    if (!l || !rnet_lobby_connected(l))
        return -1;
    a = &l->c.am;
    rid = (ruleset_id && ruleset_id[0]) ? ruleset_id
          : (a->ruleset_count > 0 ? a->rulesets[0].id : "");
    if (!rid[0])
        return -1;
    if (a->state == RNET_LOBBY_AUTOMATCH_QUEUED ||
        a->state == RNET_LOBBY_AUTOMATCH_FOUND ||
        a->state == RNET_LOBBY_AUTOMATCH_ACCEPTED || a->queue_in_flight)
        return -1;
    /* The queue key REQUIRES a fingerprint: an empty one is a wildcard that
     * silently pairs a different dump. Refuse before the round trip. */
    if (strlen(l->fp) != 64) {
        am_fail(l, "this build cannot fingerprint its game image, so it cannot "
                   "queue");
        return -1;
    }
    for (i = 0; i < a->ruleset_count; ++i)
        if (!strcmp(a->rulesets[i].id, rid) && a->rulesets[i].max_slots >= 2)
            max_slots = a->rulesets[i].max_slots;
    rnet_json_escape(l->game_name, gn_esc, sizeof(gn_esc));
    rnet_json_escape(l->game_version, gv_esc, sizeof(gv_esc));
    rnet_json_escape(rid, rid_esc, sizeof(rid_esc));
    /* The exemption evidence as a JSON ARRAY: a ';'-joined string is valid
     * JSON that every as_array() reader sees as empty -- failing open. */
    {
        size_t o = 0;
        const char *p = mod_exempt;
        int first = 1;
        exempt_json[o++] = '[';
        while (p && *p) {
            const char *end = p;
            char entry[160];
            char esc[RNET_JSON_ESC_CAP(160)];
            size_t len;
            while (*end && *end != ';' && *end != '\n')
                ++end;
            len = (size_t)(end - p);
            if (len && len < sizeof(entry)) {
                size_t el;
                memcpy(entry, p, len);
                entry[len] = '\0';
                el = rnet_json_escape(entry, esc, sizeof(esc));
                if (o + el + 4 < sizeof(exempt_json)) {
                    if (!first)
                        exempt_json[o++] = ',';
                    exempt_json[o++] = '"';
                    memcpy(exempt_json + o, esc, el);
                    o += el;
                    exempt_json[o++] = '"';
                    first = 0;
                } else {
                    /* A SHORT list is approved in full while the client relies
                     * on more than it declared: refuse instead. */
                    am_fail(l, "too many mod exemptions to declare");
                    return -1;
                }
            }
            p = *end ? end + 1 : end;
        }
        exempt_json[o++] = ']';
        exempt_json[o] = '\0';
    }
    if (a->rtt_ms < 0)
        (void)probe_send(l);
    rtt[0] = '\0';
    if (a->rtt_ms >= 0)
        snprintf(rtt, sizeof(rtt), ",\"rtt_ms\":%d", a->rtt_ms);
    /* One title: this build runs one game. The wire allows several, in
     * preference order. */
    n = snprintf(msg, sizeof(msg),
                 "{\"op\":\"automatch_queue\",\"titles\":[{"
                 "\"game_name\":\"%s\",\"game_version\":\"%s\","
                 "\"disc_fp\":\"%s\",\"ruleset_id\":\"%s\",\"max_slots\":%d}],"
                 "\"mods_enabled\":%s,\"mod_exempt\":%s%s}",
                 gn_esc, gv_esc, l->fp, rid_esc, max_slots,
                 mods_enabled ? "true" : "false", exempt_json, rtt);
    if (n < 0 || (size_t)n >= sizeof(msg))
        return -1;
    if (rnet_lobby__send(l, msg) != 0)
        return -1;
    a->error[0] = '\0';
    a->queue_in_flight = 1;
    a->rtt_reported = (a->rtt_ms >= 0);
    return 0;
}

int rnet_lobby_automatch_cancel(RNetLobby *l)
{
    if (!l || !rnet_lobby_connected(l))
        return -1;
    return rnet_lobby__send(l, "{\"op\":\"automatch_cancel\"}");
}

int rnet_lobby_automatch_state(RNetLobby *l)
{
    return l ? l->c.am.state : RNET_LOBBY_AUTOMATCH_IDLE;
}

int rnet_lobby_automatch_queued_secs(RNetLobby *l)
{
    return l ? l->c.am.queued_secs : 0;
}

int rnet_lobby_automatch_pool(RNetLobby *l)
{
    return l ? l->c.am.pool : 0;
}

int rnet_lobby_automatch_found_get(RNetLobby *l, RNetLobbyAutomatchFound *out)
{
    RNetLobbyAutomatch *a;
    if (!l || !out || l->c.am.state != RNET_LOBBY_AUTOMATCH_FOUND)
        return 0;
    a = &l->c.am;
    *out = a->found;
    /* Recomputed per read: a live count, never below zero. */
    if (a->found_deadline_ms) {
        uint64_t now = rnet_lobby__now_ms();
        out->accept_secs = now >= a->found_deadline_ms
                               ? 0
                               : (int)((a->found_deadline_ms - now + 999ull) / 1000ull);
    }
    return 1;
}

const RNetLobbyMatchCaps *rnet_lobby_automatch_found_caps(RNetLobby *l)
{
    static RNetLobbyMatchCaps none;
    if (!l || l->c.am.state != RNET_LOBBY_AUTOMATCH_FOUND)
        return &none;
    return &l->c.am.found_caps;
}

int rnet_lobby_automatch_accept(RNetLobby *l, int accept)
{
    RNetLobbyAutomatch *a;
    char msg[256];
    char mid_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_ID_LEN)];
    char tid_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_ID_LEN)];
    if (!l || !rnet_lobby_connected(l) || l->c.am.state != RNET_LOBBY_AUTOMATCH_FOUND)
        return -1;
    a = &l->c.am;
    /* match_id is the documented echo (a late answer to a lapsed offer is
     * then discarded); ticket_id rides along for servers that keyed on it. */
    rnet_json_escape(a->match_id, mid_esc, sizeof(mid_esc));
    rnet_json_escape(a->ticket_id, tid_esc, sizeof(tid_esc));
    snprintf(msg, sizeof(msg),
             "{\"op\":\"automatch_accept\",\"match_id\":\"%s\",\"ticket_id\":\"%s\","
             "\"accept\":%s}",
             mid_esc, tid_esc, accept ? "true" : "false");
    if (rnet_lobby__send(l, msg) != 0)
        return -1;
    if (accept)
        a->state = RNET_LOBBY_AUTOMATCH_ACCEPTED;
    else
        rnet_lobby__am_reset_queue(l); /* the strike is the server's to record */
    return 0;
}

int rnet_lobby_automatch_room(RNetLobby *l)
{
    return l ? l->c.am.in_automatch_room : 0;
}

void rnet_lobby_automatch_refuse_local(RNetLobby *l, const char *why)
{
    if (l)
        am_fail(l, why);
}

const char *rnet_lobby_automatch_error(RNetLobby *l)
{
    return l ? l->c.am.error : "";
}

int rnet_lobby_automatch_rtt_ms(RNetLobby *l)
{
    return l ? l->c.am.rtt_ms : -1;
}
