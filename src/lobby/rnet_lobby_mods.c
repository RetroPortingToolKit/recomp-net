/* rnet_lobby_mods.c -- mod plan / offer wire, the launch gate, and the
 * peer-to-peer package transfer. Lifted from snesrecomp's snes_lobby_client.c
 * (psxrecomp's copy has no mod support). */
#include "lobby/rnet_lobby_internal.h"

#include "recomp_net/ice_xfer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── wire encoding ───────────────────────────────────────────────────────── */

int rnet_lobby__parse_mod_pkgs(RNetJsonSpan obj, const char *key,
                               RNetLobbyModPkg *out, int max)
{
    RNetJsonSpan arr, row;
    int n = 0;
    if (!out || max <= 0)
        return 0;
    /* A string here is the superseded ';' encoding: nothing, on purpose. */
    if (!rnet_json_arr(obj, key, &arr))
        return 0;
    while (n < max && rnet_json_arr_next(&arr, &row)) {
        RNetLobbyModPkg r;
        if (rnet_json_kind(row) != '{')
            continue;
        memset(&r, 0, sizeof(r));
        /* The (id, ver) pair is the whole identity the gates match on: half
         * of it, or a truncated half, is a DIFFERENT package, not a lesser
         * row. */
        if (rnet_json_str(row, "id", r.id, sizeof(r.id)) != 1 || !r.id[0])
            continue;
        if (rnet_json_str(row, "ver", r.ver, sizeof(r.ver)) != 1 || !r.ver[0])
            continue;
        rnet_json_str(row, "n", r.name, sizeof(r.name));
        rnet_json_str(row, "f", r.feats, sizeof(r.feats));
        out[n++] = r;
    }
    return n;
}

int rnet_lobby__append_mod_pkgs(char *dst, size_t cap, const char *key,
                                const RNetLobbyModPkg *pkgs, int count)
{
    size_t used;
    int i, n, wrote = 0;
    if (!dst || cap < 8 || !key || (!pkgs && count > 0))
        return 0;
    n = snprintf(dst, cap, "\"%s\":[", key);
    if (n < 0 || (size_t)n >= cap)
        return 0;
    used = (size_t)n;
    for (i = 0; i < count; ++i) {
        char id_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_MOD_ID_LEN)];
        char ver_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_MOD_VER_LEN)];
        char name_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_MOD_NAME_LEN)];
        char feats_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_MOD_FEATS_LEN)];
        if (!pkgs[i].id[0] || !pkgs[i].ver[0])
            continue;
        /* id and ver come from package metadata, which a crafted package
         * chooses: escaped like the rest. */
        rnet_json_escape(pkgs[i].id, id_esc, sizeof(id_esc));
        rnet_json_escape(pkgs[i].ver, ver_esc, sizeof(ver_esc));
        rnet_json_escape(pkgs[i].name, name_esc, sizeof(name_esc));
        rnet_json_escape(pkgs[i].feats, feats_esc, sizeof(feats_esc));
        n = snprintf(dst + used, cap - used,
                     "%s{\"id\":\"%s\",\"ver\":\"%s\",\"n\":\"%s\",\"f\":\"%s\"}",
                     wrote ? "," : "", id_esc, ver_esc, name_esc, feats_esc);
        if (n < 0 || (size_t)n >= cap - used)
            return 0; /* a SHORT array names fewer requirements: none at all */
        used += (size_t)n;
        wrote++;
    }
    n = snprintf(dst + used, cap - used, "]");
    if (n < 0 || (size_t)n >= cap - used)
        return 0;
    return (int)(used + (size_t)n);
}

int rnet_lobby__append_mod_offer(RNetLobby *l, char *dst, size_t cap)
{
    RNetLobbyModPkg rows[RNET_LOBBY_MAX_MODS];
    int n, used, m;
    const char *obj;
    if (!dst || cap < 8)
        return 0;
    dst[0] = '\0';
    if (!l->offer_fn)
        return 0;
    memset(rows, 0, sizeof(rows));
    n = l->offer_fn(rows, RNET_LOBBY_MAX_MODS, l->offer_ctx);
    if (n < 0)
        n = 0;
    if (n > RNET_LOBBY_MAX_MODS)
        n = RNET_LOBBY_MAX_MODS;
    used = snprintf(dst, cap, ",\"mod_offer\":{\"v\":1,");
    if (used < 0 || (size_t)used >= cap)
        return 0;
    m = rnet_lobby__append_mod_pkgs(dst + used, cap - (size_t)used, "pkgs", rows, n);
    if (m <= 0) {
        dst[0] = '\0';
        return 0;
    }
    used += m;
    if ((size_t)used + 2 >= cap) {
        dst[0] = '\0';
        return 0;
    }
    dst[used++] = '}';
    dst[used] = '\0';
    /* The server discards a mod_offer over 2048 bytes, and a discarded offer
     * is indistinguishable from "has nothing". Refuse here, audibly. The
     * object is what is measured, not the `,"mod_offer":` prefix. */
    obj = strchr(dst, '{');
    if (obj && strlen(obj) > 2000) {
        LOBBY_WARN(l, "the installed mod set is %u bytes, over the lobby "
                      "server's 2048-byte limit; it was not announced",
                   (unsigned)strlen(obj));
        dst[0] = '\0';
        return 0;
    }
    return used;
}

void rnet_lobby_set_mod_offer_supplier(RNetLobby *l, RNetLobbyModOfferFn fn,
                                       void *ctx)
{
    if (!l)
        return;
    l->offer_fn = fn;
    l->offer_ctx = ctx;
}

int rnet_lobby_need_mods_count(RNetLobby *l)
{
    return l ? l->c.need_mods_count : 0;
}

const RNetLobbyModPkg *rnet_lobby_need_mods_get(RNetLobby *l, int index)
{
    if (!l || index < 0 || index >= l->c.need_mods_count)
        return NULL;
    return &l->c.need_mods[index];
}

int rnet_lobby_need_mods_can_transfer(RNetLobby *l)
{
    return l ? l->c.need_mods_can_transfer : 0;
}

/* ── the launch gate ─────────────────────────────────────────────────────── */

/* A peer that announced nothing counts as missing everything -- not a guess:
 * this build always announces, so silence is an older build or a set too
 * large to state, and either way we do not know it can play. Matched on id
 * ALONE: whether two builds resolve to the same simulation is settled later
 * and more precisely by the engine's session-start mod-set exchange. */
int rnet_lobby__member_missing_count(RNetLobby *l, int mi)
{
    const RNetLobbyMatchCaps *caps = &l->c.match_caps;
    int missing = 0, i, j;
    if (mi < 0 || mi >= RNET_LOBBY_MAX_MEMBERS)
        return 0;
    for (i = 0; i < caps->mod_count; ++i) {
        int have = 0;
        for (j = 0; j < l->c.member_offer_count[mi]; ++j) {
            if (!strcmp(l->c.member_offer[mi][j].id, caps->mods[i].id)) {
                have = 1;
                break;
            }
        }
        if (!have)
            missing++;
    }
    return missing;
}

int rnet_lobby_match_blocked_by_mods(RNetLobby *l, char *who, size_t who_cap,
                                     char *what, size_t what_cap)
{
    int n, blocked = 0;
    if (who && who_cap)
        who[0] = '\0';
    if (what && what_cap)
        what[0] = '\0';
    if (!l || !l->c.in_lobby || l->c.match_caps.mod_count <= 0)
        return 0;
    for (n = 0; n < l->c.member_count; ++n) {
        const RNetLobbyMatchCaps *caps = &l->c.match_caps;
        int missing, i, j;
        if (!strcmp(l->c.members[n].player_id, l->c.player_id))
            continue; /* the host runs its own plan by definition */
        /* The gallery runs the match too, so it needs the mods as much as a
         * player does. */
        missing = rnet_lobby__member_missing_count(l, n);
        if (missing <= 0)
            continue;
        blocked += missing;
        if (who && who_cap && !who[0])
            snprintf(who, who_cap, "%s", l->c.members[n].display_name);
        if (what && what_cap && !what[0]) {
            for (i = 0; i < caps->mod_count; ++i) {
                int have = 0;
                for (j = 0; j < l->c.member_offer_count[n]; ++j)
                    if (!strcmp(l->c.member_offer[n][j].id, caps->mods[i].id)) {
                        have = 1;
                        break;
                    }
                if (!have) {
                    snprintf(what, what_cap, "%s@%s", caps->mods[i].id,
                             caps->mods[i].ver);
                    break;
                }
            }
        }
    }
    return blocked;
}

int rnet_lobby_local_missing_mods(RNetLobby *l)
{
    int n;
    if (!l)
        return 0;
    for (n = 0; n < l->c.member_count; ++n)
        if (!strcmp(l->c.members[n].player_id, l->c.player_id))
            return rnet_lobby__member_missing_count(l, n);
    return 0;
}

/* ── transfer ────────────────────────────────────────────────────────────── */

/* What an agent EMITS is not what its peer must be PUSHED: LOCAL_* describes
 * the sender, and the far side must receive it as REMOTE_*. Forwarded
 * unchanged, both agents gather and sit in "connecting" forever. */
int rnet_lobby__mod_ice_type_for_push(int emitted_type)
{
    if (emitted_type == (int)RNET_SIGNAL_LOCAL_SDP)
        return (int)RNET_SIGNAL_REMOTE_SDP;
    if (emitted_type == (int)RNET_SIGNAL_LOCAL_CANDIDATE)
        return (int)RNET_SIGNAL_REMOTE_CANDIDATE;
    return emitted_type;
}

static int path_is_relay(const char *path)
{
    return path && !strcmp(path, "relay");
}

static int path_is_known(const char *path)
{
    if (!path || !path[0])
        return 0;
    return !strcmp(path, "relay") || !strcmp(path, "host") ||
           !strcmp(path, "srflx") || !strcmp(path, "prflx");
}

int rnet_lobby_mod_relay_size_allows(const char *path, uint64_t bytes,
                                     const char *package_id, char *reason,
                                     size_t reason_cap)
{
    if (reason && reason_cap)
        reason[0] = '\0';
    if (!path_is_relay(path))
        return 1;
    if (bytes <= (uint64_t)RNET_LOBBY_MOD_RELAY_MAX_BYTES)
        return 1;
    if (reason && reason_cap) {
        /* Which mod, how big, the cap, and what to do instead. */
        snprintf(reason, reason_cap,
                 "%s is %.1f MB. This connection could not find a direct route "
                 "between the two of you, so the file would have to go through "
                 "the relay server, which is capped at %u MB. Download the mod "
                 "from its original source instead.",
                 (package_id && package_id[0]) ? package_id : "that mod",
                 (double)bytes / (1024.0 * 1024.0),
                 (unsigned)(RNET_LOBBY_MOD_RELAY_MAX_BYTES / (1024u * 1024u)));
    }
    return 0;
}

static void xfer_emit(const RNetSignal *msg, void *user)
{
    RNetLobby *l = (RNetLobby *)user;
    if (!msg || !l)
        return;
    (void)rnet_lobby_send_signal_to(l, l->c.xfer_peer,
                                    RNET_LOBBY_SIG_MOD_ICE_BASE + (int)msg->type,
                                    (int)msg->flag, msg->text);
}

void rnet_lobby__mod_reset(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    if (c->xfer)
        rnet_ice_xfer_close(&c->xfer);
    c->xfer = NULL;
    c->xfer_busy = 0;
    c->xfer_sending = 0;
    c->xfer_peer[0] = '\0';
    c->xfer_id[0] = '\0';
    c->xfer_ver[0] = '\0';
    c->xfer_sha[0] = '\0';
    c->xfer_expect = 0;
    /* Held bytes never queued are ours, freed with the exporter's own hook;
     * a blob that reached rnet_ice_xfer_queue_blob belongs to the transfer. */
    if (c->xfer_hold) {
        if (l->free_fn)
            l->free_fn(c->xfer_hold);
        else
            free(c->xfer_hold);
        c->xfer_hold = NULL;
    }
    c->xfer_hold_len = 0;
    c->xfer_hold_hdr[0] = '\0';
    c->xfer_path_priced = 0;
    c->sig_hold_n = 0;
    c->sig_hold_from[0] = '\0';
}

static void xfer_fail(RNetLobby *l, const char *why)
{
    snprintf(l->c.xfer_err, sizeof(l->c.xfer_err), "%s",
             (why && why[0]) ? why : "transfer failed");
    LOBBY_WARN(l, "mod transfer failed - %s", l->c.xfer_err);
    l->c.xfer_progress = -2;
    rnet_lobby__mod_reset(l);
}

/* The ICE config from the lobby's TURN mint, copied into the connection
 * because RNetIceConfig borrows its strings for the agent's life. */
static void xfer_ice_config(RNetLobby *l, RNetIceConfig *ice, int controlling)
{
    const RNetLobbyTurnCredentials *tc = rnet_lobby_turn_credentials(l);
    RNetLobbyConn *c = &l->c;
    rnet_ice_config_init_defaults(ice);
    /* The role is decided HERE, before the agent exists: the defaults made
     * both peers offerers ("ICE role conflict (both controlling)"). */
    ice->controlling = (rnet_u8)(controlling ? 1 : 0);
    if (!tc || !tc->valid)
        return; /* host candidates only; fine on a LAN */
    if (tc->stun_host[0]) {
        snprintf(c->ice_stun, sizeof(c->ice_stun), "%s", tc->stun_host);
        ice->stun_host = c->ice_stun;
        ice->stun_port = (rnet_u16)(tc->stun_port > 0 ? tc->stun_port : 3478);
    }
    if (tc->turn_host[0]) {
        snprintf(c->ice_turn, sizeof(c->ice_turn), "%s", tc->turn_host);
        snprintf(c->ice_user, sizeof(c->ice_user), "%s", tc->username);
        snprintf(c->ice_pass, sizeof(c->ice_pass), "%s", tc->password);
        ice->turn_host = c->ice_turn;
        ice->turn_port = (rnet_u16)(tc->turn_port > 0 ? tc->turn_port : 3478);
        ice->turn_user = c->ice_user;
        ice->turn_pass = c->ice_pass;
    }
}

static int xfer_open(RNetLobby *l, const char *peer, int controlling)
{
    RNetLobbyConn *c = &l->c;
    RNetIceConfig ice;
    if (c->xfer)
        rnet_ice_xfer_close(&c->xfer);
    xfer_ice_config(l, &ice, controlling);
    if (rnet_ice_xfer_open(&c->xfer, &ice, xfer_emit, l) != 0 || !c->xfer) {
        xfer_fail(l, "could not open a direct connection");
        return -1;
    }
    snprintf(c->xfer_peer, sizeof(c->xfer_peer), "%s", peer ? peer : "");
    c->xfer_busy = 1;
    c->xfer_progress = 0;
    c->xfer_err[0] = '\0';
    c->xfer_last_state = -1;
    c->xfer_started_ms = rnet_lobby__now_ms();
    c->xfer_connected_ms = 0;
    /* Replay what arrived before the agent existed -- only from this peer: a
     * hold from an abandoned exchange would poison the new one. */
    if (c->sig_hold_n > 0) {
        if (c->xfer_peer[0] && !strcmp(c->sig_hold_from, c->xfer_peer)) {
            int i;
            LOBBY_INFO(l, "replaying %d held ICE signal(s)", c->sig_hold_n);
            for (i = 0; i < c->sig_hold_n; ++i)
                rnet_ice_xfer_push_signal(c->xfer, &c->sig_hold[i]);
        }
        c->sig_hold_n = 0;
        c->sig_hold_from[0] = '\0';
    }
    return 0;
}

/* HOST: a seated peer asked for a package. Every refusal is sent to the peer
 * AND logged here -- the machine that knows why must say so. */
static void xfer_on_request(RNetLobby *l, const char *from, const char *text)
{
    RNetLobbyConn *c = &l->c;
    char id[RNET_LOBBY_MOD_ID_LEN];
    char ver[RNET_LOBBY_MOD_VER_LEN];
    const char *at;
    size_t idlen;
    uint8_t *blob = NULL;
    uint32_t len = 0;
    char sha[65];
    char err[192];
    char id_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_MOD_ID_LEN)];
    char ver_esc[RNET_JSON_ESC_CAP(RNET_LOBBY_MOD_VER_LEN)];
    int i, in_plan = 0;

    err[0] = '\0';
    sha[0] = '\0';
    if (!from || !from[0] || !text)
        return;
    LOBBY_INFO(l, "%s asked for \"%s\"", from, text);
    if (c->xfer_busy) {
        LOBBY_INFO(l, "refusing - already transferring");
        (void)rnet_lobby_send_signal_to(l, from, RNET_LOBBY_SIG_MOD_NAK, 0,
                                        "the host is already sending a mod; "
                                        "try again in a moment");
        return;
    }
    at = strchr(text, '@');
    if (!at)
        return;
    idlen = (size_t)(at - text);
    if (idlen == 0 || idlen >= sizeof(id))
        return;
    memcpy(id, text, idlen);
    id[idlen] = '\0';
    snprintf(ver, sizeof(ver), "%s", at + 1);
    /* Only ever send what this host runs: the plan is what the guest was
     * shown, and anything else would let a peer pull arbitrary packages off
     * this machine by name. */
    for (i = 0; i < c->match_caps.mod_count; ++i)
        if (!strcmp(c->match_caps.mods[i].id, id)) {
            in_plan = 1;
            snprintf(ver, sizeof(ver), "%s", c->match_caps.mods[i].ver);
            break;
        }
    if (!in_plan) {
        LOBBY_WARN(l, "refusing - \"%s\" is not in this host's published plan "
                      "(%d package(s))", id, c->match_caps.mod_count);
        (void)rnet_lobby_send_signal_to(l, from, RNET_LOBBY_SIG_MOD_NAK, 0,
                                        "that mod is not part of this lobby's plan");
        return;
    }
    if (!l->export_fn) {
        LOBBY_WARN(l, "refusing - no mod export hook is installed in this build");
        (void)rnet_lobby_send_signal_to(l, from, RNET_LOBBY_SIG_MOD_NAK, 0,
                                        "the host's build cannot send mods");
        return;
    }
    if (l->export_fn(id, ver, &blob, &len, sha, sizeof(sha), err, sizeof(err),
                     l->hook_ctx) != 1) {
        LOBBY_WARN(l, "refusing - packing %s@%s failed: %s", id, ver,
                   err[0] ? err : "(the exporter gave no reason)");
        (void)rnet_lobby_send_signal_to(l, from, RNET_LOBBY_SIG_MOD_NAK, 0,
                                        err[0] ? err : "could not pack the mod");
        return;
    }
    if (xfer_open(l, from, /*controlling=*/1) != 0) {
        if (blob) {
            if (l->free_fn) l->free_fn(blob);
            else free(blob);
        }
        return;
    }
    c->xfer_sending = 1;
    snprintf(c->xfer_id, sizeof(c->xfer_id), "%s", id);
    snprintf(c->xfer_ver, sizeof(c->xfer_ver), "%s", ver);
    /* Header then payload; the digest travels in the header so the payload
     * is checked against a value that did not come from it. Both are HELD
     * until ICE connects: the size cap depends on whether the pair is
     * relayed, which is not known before. */
    rnet_json_escape(id, id_esc, sizeof(id_esc));
    rnet_json_escape(ver, ver_esc, sizeof(ver_esc));
    snprintf(c->xfer_hold_hdr, sizeof(c->xfer_hold_hdr),
             "{\"id\":\"%s\",\"ver\":\"%s\",\"len\":%u,\"sha256\":\"%s\"}",
             id_esc, ver_esc, (unsigned)len, sha);
    c->xfer_hold = blob;
    c->xfer_hold_len = (size_t)len;
    c->xfer_path_priced = 0;
    LOBBY_INFO(l, "packed %s@%s (%u bytes) for %s - waiting for the path before "
                  "sending", id, ver, (unsigned)len, from);
}

/* HOST: the agent is up, the pair type knowable. Price the path, then queue
 * or refuse. */
static void xfer_release_held(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    char path[32];
    char reason[256];
    uint8_t *hdr_copy, *blob;
    size_t hdr_len, blob_len;
    if (!c->xfer || !c->xfer_hold || c->xfer_path_priced)
        return;
    rnet_ice_xfer_path(c->xfer, path, sizeof(path));
    if (!path_is_known(path)) {
        /* Connected but unnamed: wait briefly, then take it at its word --
         * refusing would cap direct transfers we merely failed to name. */
        if (c->xfer_connected_ms &&
            rnet_lobby__now_ms() - c->xfer_connected_ms < 2000u)
            return;
        LOBBY_WARN(l, "could not tell whether this link is relayed (path=\"%s\") "
                      "- sending %s@%s uncapped", path, c->xfer_id, c->xfer_ver);
    }
    if (!rnet_lobby_mod_relay_size_allows(path, (uint64_t)c->xfer_hold_len,
                                          c->xfer_id, reason, sizeof(reason))) {
        LOBBY_WARN(l, "refusing to relay %s@%s - %s", c->xfer_id, c->xfer_ver,
                   reason);
        (void)rnet_lobby_send_signal_to(l, c->xfer_peer, RNET_LOBBY_SIG_MOD_NAK,
                                        0, reason);
        /* Not a failure on THIS machine: the host's panel stays calm. */
        c->xfer_progress = -1;
        rnet_lobby__mod_reset(l);
        return;
    }
    hdr_len = strlen(c->xfer_hold_hdr);
    hdr_copy = (uint8_t *)malloc(hdr_len + 1);
    if (!hdr_copy) {
        xfer_fail(l, "out of memory");
        return;
    }
    memcpy(hdr_copy, c->xfer_hold_hdr, hdr_len + 1);
    /* Ownership moves on a successful queue: drop our pointer FIRST so a
     * failure below cannot free bytes the transfer already owns. */
    blob = c->xfer_hold;
    blob_len = c->xfer_hold_len;
    c->xfer_hold = NULL;
    c->xfer_hold_len = 0;
    c->xfer_path_priced = 1;
    if (rnet_ice_xfer_queue_blob(c->xfer, hdr_copy, hdr_len) != 0) {
        if (l->free_fn) l->free_fn(blob);
        else free(blob);
        xfer_fail(l, "could not queue the mod for sending");
        return;
    }
    if (rnet_ice_xfer_queue_blob(c->xfer, blob, blob_len) != 0) {
        xfer_fail(l, "could not queue the mod for sending");
        return;
    }
    LOBBY_INFO(l, "sending %s@%s (%u bytes) over the %s path", c->xfer_id,
               c->xfer_ver, (unsigned)blob_len, path);
}

/* GUEST: a completed blob arrived. */
static void xfer_on_blob(RNetLobby *l, uint8_t *data, size_t len)
{
    RNetLobbyConn *c = &l->c;
    char err[192];
    char id[RNET_LOBBY_MOD_ID_LEN];
    char ver[RNET_LOBBY_MOD_VER_LEN];
    err[0] = '\0';
    id[0] = '\0';
    ver[0] = '\0';
    LOBBY_INFO(l, "received %u byte(s) over the direct link", (unsigned)len);
    if (!c->xfer_sha[0]) {
        /* First blob: the header. */
        RNetJsonSpan h = rnet_json_span_n((const char *)data, len);
        char got_id[RNET_LOBBY_MOD_ID_LEN];
        char path[32];
        char reason[256];
        long long expect;
        got_id[0] = '\0';
        rnet_json_str(h, "sha256", c->xfer_sha, sizeof(c->xfer_sha));
        rnet_json_str(h, "id", got_id, sizeof(got_id));
        /* The host is answering a SPECIFIC request; a different package is
         * something nobody asked for. */
        if (c->xfer_id[0] && got_id[0] && strcmp(got_id, c->xfer_id) != 0) {
            free(data);
            xfer_fail(l, "the host offered a different mod than the one requested");
            return;
        }
        if (got_id[0])
            snprintf(c->xfer_id, sizeof(c->xfer_id), "%s", got_id);
        rnet_json_str(h, "ver", c->xfer_ver, sizeof(c->xfer_ver));
        expect = rnet_json_i64(h, "len", 0);
        c->xfer_expect = (expect > 0 && expect <= 0xFFFFFFFFll) ? (uint32_t)expect : 0;
        free(data);
        if (!c->xfer_sha[0] || !c->xfer_expect) {
            xfer_fail(l, "the host described the mod in a way we cannot read");
            return;
        }
        /* The same cap on the receiving side: the cap protects the relay
         * operator, so it cannot depend on the other end honouring it. */
        rnet_ice_xfer_path(c->xfer, path, sizeof(path));
        if (!rnet_lobby_mod_relay_size_allows(path, (uint64_t)c->xfer_expect,
                                              c->xfer_id, reason, sizeof(reason))) {
            xfer_fail(l, reason);
            return;
        }
        return;
    }
    if ((uint32_t)len != c->xfer_expect) {
        free(data);
        xfer_fail(l, "the mod arrived a different size than the host said");
        return;
    }
    if (!l->install_fn) {
        free(data);
        xfer_fail(l, "this build cannot install mods");
        return;
    }
    /* The digest is checked inside the install hook, before unpacking. */
    if (l->install_fn(data, (uint32_t)len, c->xfer_sha, id, sizeof(id), ver,
                      sizeof(ver), err, sizeof(err), l->hook_ctx) != 1) {
        free(data);
        xfer_fail(l, err[0] ? err : "the mod could not be installed");
        return;
    }
    free(data);
    LOBBY_INFO(l, "installed %s@%s from the host", id, ver);
    c->xfer_progress = 100;
    rnet_lobby__mod_reset(l);
    /* Re-announce: the host's launch gate reads the announcement. */
    if (c->in_lobby)
        rnet_lobby__send_set_ready(l, c->local_ready ? 1 : 0);
}

void rnet_lobby__mod_pump(RNetLobby *l)
{
    RNetLobbyConn *c = &l->c;
    uint8_t *data = NULL;
    size_t len = 0;
    char err[160];
    if (!c->xfer)
        return;
    rnet_ice_xfer_pump(c->xfer);
    {
        /* Narrate the handshake: a transfer that never connects and one that
         * stalls otherwise look identical in the log. */
        const RNetIceState st = rnet_ice_xfer_state(c->xfer);
        if ((int)st != c->xfer_last_state) {
            c->xfer_last_state = (int)st;
            LOBBY_INFO(l, "mod transfer link is %s", rnet_ice_state_name(st));
            if ((st == RNET_ICE_STATE_CONNECTED || st == RNET_ICE_STATE_COMPLETED) &&
                !c->xfer_connected_ms)
                c->xfer_connected_ms = rnet_lobby__now_ms();
        }
    }
    if (rnet_ice_xfer_failed(c->xfer, err, sizeof(err))) {
        xfer_fail(l, err);
        return;
    }
    {
        /* Stall watchdog: a transfer that never ends blocks every later one
         * (one at a time). */
        const uint64_t now = rnet_lobby__now_ms();
        if (!c->xfer_connected_ms && now - c->xfer_started_ms > 45000u) {
            xfer_fail(l, "could not open a direct connection to the other player "
                         "(no route found)");
            return;
        }
        if (c->xfer_connected_ms && now - c->xfer_connected_ms > 180000u) {
            xfer_fail(l, "the transfer stopped making progress");
            return;
        }
    }
    if (c->xfer_hold && c->xfer_connected_ms) {
        xfer_release_held(l);
        if (!c->xfer)
            return;
    }
    {
        const int p = rnet_ice_xfer_progress(c->xfer);
        if (p >= 0)
            c->xfer_progress = p;
    }
    while (rnet_ice_xfer_take_blob(c->xfer, &data, &len)) {
        xfer_on_blob(l, data, len);
        if (!c->xfer)
            return;
        data = NULL;
        len = 0;
    }
    /* Done when everything queued has left -- and not before anything was
     * queued (an archive still held while ICE connects is not "finished"). */
    if (c->xfer_sending && c->xfer_path_priced && rnet_ice_xfer_send_idle(c->xfer)) {
        LOBBY_INFO(l, "%s@%s sent", c->xfer_id, c->xfer_ver);
        c->xfer_progress = -1;
        rnet_lobby__mod_reset(l);
    }
}

int rnet_lobby__mod_on_signal(RNetLobby *l, int type, int flag, const char *text,
                              const char *from)
{
    RNetLobbyConn *c = &l->c;
    if (type == RNET_LOBBY_SIG_MOD_REQ) {
        xfer_on_request(l, from, text);
        return 1;
    }
    if (type == RNET_LOBBY_SIG_MOD_NAK) {
        /* Only the peer we asked may refuse us. */
        if (c->xfer_busy && (!from || !from[0] || !strcmp(from, c->xfer_peer)))
            xfer_fail(l, (text && text[0]) ? text : "the host refused");
        return 1;
    }
    if (type > RNET_LOBBY_SIG_MOD_ICE_BASE &&
        type <= RNET_LOBBY_SIG_MOD_ICE_BASE + (int)RNET_SIGNAL_SET_CONTROLLING) {
        RNetSignal sig;
        memset(&sig, 0, sizeof(sig));
        sig.type = (RNetSignalType)rnet_lobby__mod_ice_type_for_push(
            type - RNET_LOBBY_SIG_MOD_ICE_BASE);
        sig.flag = (rnet_u8)(flag & 0xFF);
        snprintf(sig.text, sizeof(sig.text), "%s", text ? text : "");
        /* Only from the peer we are transferring with; anyone else's SDP
         * breaks the live negotiation. */
        if (c->xfer && from && from[0] && !strcmp(from, c->xfer_peer)) {
            rnet_ice_xfer_push_signal(c->xfer, &sig);
        } else if (from && from[0]) {
            /* No agent yet: the sender is still packing. Hold it -- libjuice
             * will not re-send, and a dropped offer stalls the handshake. */
            if (c->sig_hold_n == 0 || strcmp(c->sig_hold_from, from) != 0) {
                c->sig_hold_n = 0;
                snprintf(c->sig_hold_from, sizeof(c->sig_hold_from), "%s", from);
            }
            if (c->sig_hold_n < RNET_LOBBY_SIG_HOLD)
                c->sig_hold[c->sig_hold_n++] = sig;
            else
                LOBBY_WARN(l, "ICE hold buffer full; dropping a candidate");
        }
        return 1;
    }
    return 0;
}

int rnet_lobby_mod_request(RNetLobby *l, const char *package_id,
                           const char *version)
{
    RNetLobbyConn *c;
    char text[RNET_LOBBY_MOD_ID_LEN + RNET_LOBBY_MOD_VER_LEN + 2];
    if (!l || !rnet_lobby_connected(l) || !l->c.in_lobby)
        return -1;
    c = &l->c;
    if (c->is_host)
        return -1; /* the host IS the source */
    if (!c->host_player_id[0])
        return -1;
    if (c->xfer_busy) {
        LOBBY_INFO(l, "not asking for %s - already transferring %s",
                   package_id ? package_id : "?", c->xfer_id);
        return -2; /* busy, not broken */
    }
    if (!package_id || !package_id[0])
        return -1;
    if (xfer_open(l, c->host_player_id, /*controlling=*/0) != 0)
        return -1;
    c->xfer_sending = 0;
    c->xfer_sha[0] = '\0';
    c->xfer_expect = 0;
    /* Named NOW: mod_in_flight attributes progress to a row through the
     * whole connect phase. */
    snprintf(c->xfer_id, sizeof(c->xfer_id), "%s", package_id);
    snprintf(c->xfer_ver, sizeof(c->xfer_ver), "%s", version ? version : "");
    snprintf(text, sizeof(text), "%s@%s", package_id, version ? version : "");
    if (rnet_lobby_send_signal_to(l, c->host_player_id, RNET_LOBBY_SIG_MOD_REQ, 0,
                                  text) != 0) {
        xfer_fail(l, "could not reach the host");
        return -1;
    }
    LOBBY_INFO(l, "asked the host for %s", text);
    return 0;
}

void rnet_lobby_mod_cancel(RNetLobby *l)
{
    if (!l || !l->c.xfer_busy)
        return;
    LOBBY_INFO(l, "mod transfer cancelled");
    l->c.xfer_progress = -1;
    rnet_lobby__mod_reset(l);
}

int rnet_lobby_mod_progress(RNetLobby *l)
{
    if (!l)
        return -1;
    return l->c.xfer_busy ? l->c.xfer_progress
                          : (l->c.xfer_progress == -2 ? -2 : -1);
}

int rnet_lobby_mod_failed(RNetLobby *l, char *err, size_t err_cap)
{
    if (!l || !l->c.xfer_err[0])
        return 0;
    if (err && err_cap)
        snprintf(err, err_cap, "%s", l->c.xfer_err);
    return 1;
}

const char *rnet_lobby_mod_in_flight(RNetLobby *l)
{
    return (l && l->c.xfer_busy) ? l->c.xfer_id : "";
}

void rnet_lobby_set_mod_transfer_hooks(RNetLobby *l,
                                       RNetLobbyModExportFn export_fn,
                                       RNetLobbyModFreeFn free_fn,
                                       RNetLobbyModInstallFn install_fn,
                                       void *ctx)
{
    if (!l)
        return;
    l->export_fn = export_fn;
    l->free_fn = free_fn;
    l->install_fn = install_fn;
    l->hook_ctx = ctx;
}
