#include "recomp_net/hash_confirm.h"

#include <string.h>

/*
 * Both modes run the same algorithms over an "effective" seat mask: an N-way
 * tracker uses its peer_mask, a legacy tracker uses {bit 0} = the implicit
 * peer in peer[0]. With one seat every loop below reduces exactly to the
 * original two-player code, which is what keeps N=2 behaviour unchanged.
 */

#define HC_ALL_SEATS ((1u << RNET_HC_MAX_PEERS) - 1u)

static uint32_t slot_of(uint32_t tick)
{
    return tick % RNET_HC_RING;
}

static uint32_t eff_mask(const RNetHashConfirm *hc)
{
    return hc->nway ? hc->peer_mask : 1u;
}

static int ring_at(const RNetHcRing *r, uint32_t tick, uint32_t *dig)
{
    uint32_t i = slot_of(tick);
    if (!r->valid[i] || r->tick[i] != tick)
        return 0;
    if (dig)
        *dig = r->digest[i];
    return 1;
}

static void ring_put(RNetHcRing *r, uint32_t tick, uint32_t digest)
{
    uint32_t i = slot_of(tick);
    r->tick[i] = tick;
    r->digest[i] = digest;
    r->valid[i] = 1u;
}

static int local_at(const RNetHashConfirm *hc, uint32_t tick, uint32_t *dig)
{
    return ring_at(&hc->local, tick, dig);
}

static int next_tick(const RNetHashConfirm *hc, uint32_t *next)
{
    if (!hc->resolved_valid) {
        *next = 0u;
        return 1;
    }
    if (hc->resolved_through == 0xffffffffu)
        return 0;
    *next = hc->resolved_through + 1u;
    return 1;
}

/* 1 when local and every expected seat have `tick` and all equal local. */
static int all_agree(const RNetHashConfirm *hc, uint32_t tick)
{
    uint32_t mask = eff_mask(hc);
    uint32_t ld = 0;
    uint32_t s;
    if (mask == 0u)
        return 0; /* nobody to agree with: fail closed */
    if (!local_at(hc, tick, &ld))
        return 0;
    for (s = 0; s < RNET_HC_MAX_PEERS; s++) {
        uint32_t pd = 0;
        if (!(mask & (1u << s)))
            continue;
        if (!ring_at(&hc->peer[s], tick, &pd) || pd != ld)
            return 0;
    }
    return 1;
}

static void try_advance(RNetHashConfirm *hc)
{
    for (;;) {
        uint32_t next;
        if (!next_tick(hc, &next))
            return;
        if (!all_agree(hc, next))
            return;
        hc->resolved_through = next;
        hc->resolved_valid = 1u;
    }
}

void rnet_hc_reset(RNetHashConfirm *hc)
{
    if (!hc)
        return;
    memset(hc, 0, sizeof(*hc));
}

void rnet_hc_init_n(RNetHashConfirm *hc, uint32_t peer_mask)
{
    if (!hc)
        return;
    memset(hc, 0, sizeof(*hc));
    hc->nway = 1u;
    hc->peer_mask = peer_mask & HC_ALL_SEATS;
}

void rnet_hc_set_peer_mask(RNetHashConfirm *hc, uint32_t peer_mask)
{
    if (!hc)
        return;
    if (!hc->nway) {
        /* The implicit peer has no seat; its digests cannot be attributed,
         * so they are dropped rather than credited to some seat. */
        memset(hc->peer, 0, sizeof(hc->peer));
        hc->nway = 1u;
    }
    hc->peer_mask = peer_mask & HC_ALL_SEATS;
    try_advance(hc);
}

uint8_t rnet_hc_is_nway(const RNetHashConfirm *hc)
{
    return (hc && hc->nway) ? 1u : 0u;
}

uint32_t rnet_hc_peer_mask(const RNetHashConfirm *hc)
{
    if (!hc)
        return 0u;
    return eff_mask(hc);
}

void rnet_hc_prime_after(RNetHashConfirm *hc, uint32_t last_ok)
{
    uint8_t nway;
    uint32_t mask;
    if (!hc)
        return;
    nway = hc->nway;
    mask = hc->peer_mask;
    memset(hc, 0, sizeof(*hc));
    hc->nway = nway;
    hc->peer_mask = mask;
    hc->resolved_through = last_ok;
    hc->resolved_valid = 1u;
}

void rnet_hc_note_local(RNetHashConfirm *hc, uint32_t tick, uint32_t digest)
{
    if (!hc)
        return;
    ring_put(&hc->local, tick, digest);
    try_advance(hc);
}

void rnet_hc_note_peer(RNetHashConfirm *hc, uint32_t tick, uint32_t digest)
{
    if (!hc || hc->nway)
        return;
    ring_put(&hc->peer[0], tick, digest);
    try_advance(hc);
}

uint8_t rnet_hc_note_peer_slot(RNetHashConfirm *hc, uint32_t slot, uint32_t tick,
                               uint32_t digest)
{
    if (!hc || !hc->nway || slot >= RNET_HC_MAX_PEERS)
        return 0u;
    if (!(hc->peer_mask & (1u << slot)))
        return 0u;
    ring_put(&hc->peer[slot], tick, digest);
    try_advance(hc);
    return 1u;
}

uint32_t rnet_hc_resolved_through(const RNetHashConfirm *hc)
{
    if (!hc || !hc->resolved_valid)
        return 0u;
    return hc->resolved_through;
}

uint8_t rnet_hc_confirm_through(const RNetHashConfirm *hc, uint32_t tick)
{
    if (!hc || !hc->resolved_valid)
        return 0u;
    return (tick <= hc->resolved_through) ? 1u : 0u;
}

uint8_t rnet_hc_local_digest(const RNetHashConfirm *hc, uint32_t tick,
                             uint32_t *digest_out)
{
    uint32_t d = 0;
    if (!hc || !local_at(hc, tick, &d))
        return 0u;
    if (digest_out)
        *digest_out = d;
    return 1u;
}

uint8_t rnet_hc_peer_digest_slot(const RNetHashConfirm *hc, uint32_t slot, uint32_t tick,
                                 uint32_t *digest_out)
{
    uint32_t d = 0;
    if (!hc || slot >= RNET_HC_MAX_PEERS || !(eff_mask(hc) & (1u << slot)))
        return 0u;
    if (!ring_at(&hc->peer[slot], tick, &d))
        return 0u;
    if (digest_out)
        *digest_out = d;
    return 1u;
}

uint32_t rnet_hc_reported_mask(const RNetHashConfirm *hc, uint32_t tick)
{
    uint32_t mask, s, out = 0u;
    if (!hc)
        return 0u;
    mask = eff_mask(hc);
    for (s = 0; s < RNET_HC_MAX_PEERS; s++) {
        if ((mask & (1u << s)) && ring_at(&hc->peer[s], tick, NULL))
            out |= (1u << s);
    }
    return out;
}

uint8_t rnet_hc_peer_digest(const RNetHashConfirm *hc, uint32_t tick,
                            uint32_t *digest_out)
{
    uint32_t mask, s;
    uint32_t ref = 0, first = 0;
    int have_ref, have_first = 0, all = 1;
    if (!hc)
        return 0u;
    if (!hc->nway)
        return rnet_hc_peer_digest_slot(hc, 0u, tick, digest_out);
    mask = hc->peer_mask;
    if (mask == 0u)
        return 0u;
    have_ref = local_at(hc, tick, &ref);
    for (s = 0; s < RNET_HC_MAX_PEERS; s++) {
        uint32_t pd = 0;
        if (!(mask & (1u << s)))
            continue;
        if (!ring_at(&hc->peer[s], tick, &pd)) {
            all = 0;
            continue;
        }
        if (!have_first) {
            first = pd;
            have_first = 1;
        }
        if (!have_ref) {
            ref = pd;
            have_ref = 1;
            continue;
        }
        if (pd != ref) {
            if (digest_out)
                *digest_out = pd;
            return 1u;
        }
    }
    if (!all || !have_first)
        return 0u;
    if (digest_out)
        *digest_out = first;
    return 1u;
}

uint8_t rnet_hc_peek_mismatch_slot(const RNetHashConfirm *hc, uint32_t *tick_out,
                                   uint32_t *slot_out, uint32_t *local_out,
                                   uint32_t *peer_out)
{
    uint32_t next, mask, s;
    uint32_t ld = 0;
    if (!hc)
        return 0u;
    if (!next_tick(hc, &next))
        return 0u;
    if (!local_at(hc, next, &ld))
        return 0u;
    mask = eff_mask(hc);
    for (s = 0; s < RNET_HC_MAX_PEERS; s++) {
        uint32_t pd = 0;
        if (!(mask & (1u << s)))
            continue;
        if (!ring_at(&hc->peer[s], next, &pd) || pd == ld)
            continue;
        if (tick_out)
            *tick_out = next;
        if (slot_out)
            *slot_out = s;
        if (local_out)
            *local_out = ld;
        if (peer_out)
            *peer_out = pd;
        return 1u;
    }
    return 0u;
}

uint8_t rnet_hc_peek_mismatch(const RNetHashConfirm *hc, uint32_t *tick_out,
                              uint32_t *local_out, uint32_t *peer_out)
{
    return rnet_hc_peek_mismatch_slot(hc, tick_out, NULL, local_out, peer_out);
}

uint8_t rnet_hc_heal_stale_gap(RNetHashConfirm *hc)
{
    uint32_t next, mask, s, i;
    uint32_t best = 0u;
    uint8_t have_best = 0u;
    if (!hc || !hc->resolved_valid)
        return 0u;
    if (hc->resolved_through == 0xffffffffu)
        return 0u;
    mask = eff_mask(hc);
    if (mask == 0u)
        return 0u;
    next = hc->resolved_through + 1u;
    /* Only a gap nobody holds any more is stale; anything present at next is
     * either pending or a live mismatch, and both must stay stuck. */
    if (local_at(hc, next, NULL))
        return 0u;
    for (s = 0; s < RNET_HC_MAX_PEERS; s++) {
        if ((mask & (1u << s)) && ring_at(&hc->peer[s], next, NULL))
            return 0u;
    }
    for (i = 0; i < RNET_HC_RING; i++) {
        uint32_t t, ld;
        int complete = 1;
        if (!hc->local.valid[i])
            continue;
        t = hc->local.tick[i];
        if (t <= hc->resolved_through)
            continue;
        ld = hc->local.digest[i];
        for (s = 0; s < RNET_HC_MAX_PEERS; s++) {
            uint32_t pd = 0;
            if (!(mask & (1u << s)))
                continue;
            if (!ring_at(&hc->peer[s], t, &pd)) {
                complete = 0;
                continue;
            }
            if (pd != ld)
                return 0u; /* any later disagreement: never heal over it */
        }
        if (!complete)
            continue;
        if (!have_best || t > best) {
            best = t;
            have_best = 1u;
        }
    }
    if (!have_best || best <= hc->resolved_through)
        return 0u;
    hc->resolved_through = best;
    return 1u;
}
