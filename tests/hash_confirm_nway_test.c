/*
 * N-peer hash-confirm watermark.
 *
 * Pins:
 *   - a legacy (two-player) tracker and an N-way tracker with one expected
 *     seat produce identical watermarks, mismatches and heals for the same
 *     input sequence (N=2 behaviour unchanged);
 *   - at N=3/4 (and sparse rooms / observers) a tick resolves only when EVERY
 *     expected seat reported it equal to local;
 *   - one diverged seat stops the watermark and is named, even before the
 *     other seats report;
 *   - the unattributed single-cell tracker's 1-of-N masking (the defect this
 *     exists for) is reproduced on the legacy path and absent on the N-way one;
 *   - ordering: out-of-order, duplicate, same-tick replacement, ring wrap,
 *     prime, heal, membership change.
 */
#include "recomp_net/config.h"
#include "recomp_net/hash_confirm.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            printf("FAIL: %s (line %d)\n", msg, __LINE__);                     \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static uint32_t dig(uint32_t tick) { return 0x9E3779B9u * (tick + 1u); }

static void test_expected_mask(void)
{
    CHECK(rnet_expected_peer_mask(2, 0, 0) == 0x2u, "2P host waits on seat 1");
    CHECK(rnet_expected_peer_mask(2, 0, 1) == 0x1u, "2P guest waits on seat 0");
    CHECK(rnet_expected_peer_mask(4, 0, 1) == 0xDu, "4P seat 1 waits on 0,2,3");
    CHECK(rnet_expected_peer_mask(4, 0x5u, 0) == 0x4u, "sparse 0+2: host waits on 2");
    CHECK(rnet_expected_peer_mask(4, 0x5u, 2) == 0x1u, "sparse 0+2: seat 2 waits on 0");
    CHECK(rnet_expected_peer_mask(4, 0xBu, 4) == 0xBu, "observer waits on all occupied");
    CHECK(rnet_expected_peer_mask(3, 0xF0u, 0) == 0u, "occupied bits past slot_count dropped");
    CHECK(rnet_expected_peer_mask(8, 0, 7) == 0x7Fu, "8P seat 7");
    {
        RNetConfig cfg;
        rnet_config_init_defaults(&cfg);
        cfg.slot_count = 4;
        cfg.local_slot = 3;
        cfg.occupied_mask = 0x9u; /* seats 0 and 3 */
        CHECK(rnet_config_expected_peer_mask(&cfg) == 0x1u, "config helper");
    }
}

/* Drive one legacy and one N-way(1 seat) tracker with the same sequence and
 * require every observable to agree after every step. */
static void test_legacy_equivalence(void)
{
    RNetHashConfirm leg, nw;
    uint32_t rng = 12345u;
    int step;
    rnet_hc_reset(&leg);
    rnet_hc_init_n(&nw, 1u << 1);
    for (step = 0; step < 4000; ++step) {
        uint32_t r, tick, d;
        uint32_t t1 = 0, l1 = 0, p1 = 0, t2 = 0, l2 = 0, p2 = 0, sl = 99;
        uint8_t m1, m2;
        rng = rng * 1103515245u + 12345u;
        r = rng >> 8;
        tick = (uint32_t)(step / 3) + (r % 7u) - 2u;
        if ((int)tick < 0 || tick > 0x7fffffffu)
            tick = 0u;
        d = ((r >> 5) % 17u == 0u) ? 0xBADu : dig(tick);
        switch (r % 5u) {
        case 0:
        case 1:
            rnet_hc_note_local(&leg, tick, dig(tick));
            rnet_hc_note_local(&nw, tick, dig(tick));
            break;
        case 2:
        case 3:
            rnet_hc_note_peer(&leg, tick, d);
            CHECK(rnet_hc_note_peer_slot(&nw, 1u, tick, d), "n-way accepts seat 1");
            break;
        default:
            CHECK(rnet_hc_heal_stale_gap(&leg) == rnet_hc_heal_stale_gap(&nw),
                  "heal agrees");
            if ((r >> 3) % 50u == 0u) {
                uint32_t p = rnet_hc_resolved_through(&leg);
                rnet_hc_prime_after(&leg, p);
                rnet_hc_prime_after(&nw, p);
            }
            break;
        }
        CHECK(rnet_hc_resolved_through(&leg) == rnet_hc_resolved_through(&nw),
              "resolved agrees");
        CHECK(rnet_hc_confirm_through(&leg, tick) == rnet_hc_confirm_through(&nw, tick),
              "confirm agrees");
        m1 = rnet_hc_peek_mismatch(&leg, &t1, &l1, &p1);
        m2 = rnet_hc_peek_mismatch_slot(&nw, &t2, &sl, &l2, &p2);
        CHECK(m1 == m2, "peek agrees");
        if (m1 && m2)
            CHECK(t1 == t2 && l1 == l2 && p1 == p2 && sl == 1u, "peek values agree");
        {
            uint32_t a = 0, b = 0;
            uint8_t ha = rnet_hc_peer_digest(&leg, tick, &a);
            uint8_t hb = rnet_hc_peer_digest(&nw, tick, &b);
            CHECK(ha == hb && (!ha || a == b), "peer digest agrees");
        }
        if (failures > 20)
            return;
    }
}

static void test_mode_isolation(void)
{
    RNetHashConfirm hc;
    rnet_hc_reset(&hc);
    CHECK(!rnet_hc_is_nway(&hc), "reset is legacy");
    CHECK(!rnet_hc_note_peer_slot(&hc, 1u, 0u, 1u), "legacy refuses attributed feed");
    rnet_hc_init_n(&hc, 0x6u);
    CHECK(rnet_hc_is_nway(&hc) && rnet_hc_peer_mask(&hc) == 0x6u, "init_n");
    rnet_hc_note_local(&hc, 0u, 7u);
    rnet_hc_note_peer(&hc, 0u, 7u); /* unattributed: must be ignored */
    CHECK(rnet_hc_reported_mask(&hc, 0u) == 0u, "n-way ignores unattributed feed");
    CHECK(!rnet_hc_confirm_through(&hc, 0u), "unattributed never resolves");
    CHECK(!rnet_hc_note_peer_slot(&hc, 0u, 0u, 7u), "own seat refused");
    CHECK(!rnet_hc_note_peer_slot(&hc, 3u, 0u, 7u), "empty seat refused");
    CHECK(!rnet_hc_note_peer_slot(&hc, 64u, 0u, 7u), "spectator wire slot refused");
    CHECK(!rnet_hc_note_peer_slot(NULL, 1u, 0u, 7u), "NULL safe");

    rnet_hc_init_n(&hc, 0u);
    rnet_hc_note_local(&hc, 0u, 7u);
    CHECK(!rnet_hc_confirm_through(&hc, 0u), "empty peer mask fails closed");
    rnet_hc_init_n(&hc, 0xFFFFFF02u);
    CHECK(rnet_hc_peer_mask(&hc) == 0x02u, "bits past RNET_HC_MAX_PEERS dropped");
}

/* Local seat 0, peers 1 and 2. */
static void test_n3(void)
{
    RNetHashConfirm hc;
    uint32_t t;
    rnet_hc_init_n(&hc, rnet_expected_peer_mask(3, 0, 0));
    for (t = 0; t < 4; ++t)
        rnet_hc_note_local(&hc, t, dig(t));
    rnet_hc_note_peer_slot(&hc, 1u, 0u, dig(0));
    CHECK(!rnet_hc_confirm_through(&hc, 0u), "N=3: one of two peers is not enough");
    CHECK(rnet_hc_reported_mask(&hc, 0u) == 0x2u, "reported mask");
    {
        uint32_t d = 0;
        CHECK(!rnet_hc_peer_digest(&hc, 0u, &d), "consensus waits for all seats");
    }
    rnet_hc_note_peer_slot(&hc, 2u, 0u, dig(0));
    CHECK(rnet_hc_confirm_through(&hc, 0u) && rnet_hc_resolved_through(&hc) == 0u,
          "N=3: both peers matched");
    {
        uint32_t d = 0;
        CHECK(rnet_hc_peer_digest(&hc, 0u, &d) && d == dig(0), "consensus digest");
    }
    /* Peer 2 ahead of peer 1: nothing moves until peer 1 catches up. */
    rnet_hc_note_peer_slot(&hc, 2u, 1u, dig(1));
    rnet_hc_note_peer_slot(&hc, 2u, 2u, dig(2));
    rnet_hc_note_peer_slot(&hc, 2u, 3u, dig(3));
    CHECK(rnet_hc_resolved_through(&hc) == 0u, "N=3: lagging seat holds watermark");
    rnet_hc_note_peer_slot(&hc, 1u, 1u, dig(1));
    rnet_hc_note_peer_slot(&hc, 1u, 2u, dig(2));
    CHECK(rnet_hc_resolved_through(&hc) == 2u, "N=3: min over seats");
    rnet_hc_note_peer_slot(&hc, 1u, 3u, dig(3));
    CHECK(rnet_hc_resolved_through(&hc) == 3u, "N=3: caught up");
}

/* Local seat 1, peers 0, 2, 3; seat 2 diverges at tick 5. */
static void test_n4_diverged_seat(void)
{
    RNetHashConfirm hc;
    uint32_t t, mt = 0, ms = 0, ml = 0, mp = 0;
    rnet_hc_init_n(&hc, rnet_expected_peer_mask(4, 0, 1));
    CHECK(rnet_hc_peer_mask(&hc) == 0xDu, "N=4 mask");
    for (t = 0; t < 8; ++t) {
        rnet_hc_note_local(&hc, t, dig(t));
        rnet_hc_note_peer_slot(&hc, 0u, t, dig(t));
        if (t < 5)
            rnet_hc_note_peer_slot(&hc, 2u, t, dig(t));
        else if (t == 5)
            rnet_hc_note_peer_slot(&hc, 2u, t, 0xDEADu);
    }
    /* Seat 3 has not reported tick 5 yet; seat 2's divergence must already
     * be a stop. */
    for (t = 0; t < 5; ++t)
        rnet_hc_note_peer_slot(&hc, 3u, t, dig(t));
    CHECK(rnet_hc_resolved_through(&hc) == 4u, "N=4: watermark stops before diverged tick");
    CHECK(rnet_hc_peek_mismatch_slot(&hc, &mt, &ms, &ml, &mp), "N=4: mismatch reported early");
    CHECK(mt == 5u && ms == 2u && ml == dig(5) && mp == 0xDEADu, "N=4: names seat 2");
    {
        uint32_t d = 0;
        CHECK(rnet_hc_peer_digest(&hc, 5u, &d) && d == 0xDEADu,
              "consensus view exposes the diverged digest to an N=2-shaped consumer");
    }
    rnet_hc_note_peer_slot(&hc, 3u, 5u, dig(5));
    rnet_hc_note_peer_slot(&hc, 3u, 6u, dig(6));
    CHECK(rnet_hc_resolved_through(&hc) == 4u, "N=4: healthy seats cannot outvote a diverged one");
    CHECK(!rnet_hc_heal_stale_gap(&hc), "N=4: heal refuses a live mismatch");
    {
        uint32_t d = 0;
        CHECK(rnet_hc_peer_digest_slot(&hc, 2u, 5u, &d) && d == 0xDEADu, "per-seat digest");
        CHECK(!rnet_hc_peer_digest_slot(&hc, 1u, 5u, &d), "own seat has no peer digest");
    }
    /* Seat 2 resimulates and re-sends the corrected digest (latest wins). */
    rnet_hc_note_peer_slot(&hc, 2u, 5u, dig(5));
    rnet_hc_note_peer_slot(&hc, 2u, 6u, dig(6));
    CHECK(rnet_hc_resolved_through(&hc) == 6u, "N=4: corrected digest resolves");
}

/* The defect: an unattributed single cell agrees with whichever seat's
 * commit arrived last. */
static void test_one_of_n_masking(void)
{
    RNetHashConfirm leg, nw;
    rnet_hc_reset(&leg);
    rnet_hc_init_n(&nw, 0xEu); /* local seat 0, peers 1..3 */
    rnet_hc_note_local(&leg, 0u, 0xAAu);
    rnet_hc_note_local(&nw, 0u, 0xAAu);
    /* seat 1 diverged, seats 2/3 healthy and arrive later */
    rnet_hc_note_peer(&leg, 0u, 0xBBu);
    rnet_hc_note_peer(&leg, 0u, 0xAAu);
    rnet_hc_note_peer(&leg, 0u, 0xAAu);
    rnet_hc_note_peer_slot(&nw, 1u, 0u, 0xBBu);
    rnet_hc_note_peer_slot(&nw, 2u, 0u, 0xAAu);
    rnet_hc_note_peer_slot(&nw, 3u, 0u, 0xAAu);
    CHECK(rnet_hc_confirm_through(&leg, 0u),
          "legacy single cell masks the diverged seat (documented defect)");
    CHECK(!rnet_hc_confirm_through(&nw, 0u), "N-way does not");
    {
        uint32_t s = 99;
        CHECK(rnet_hc_peek_mismatch_slot(&nw, NULL, &s, NULL, NULL) && s == 1u,
              "N-way names seat 1");
    }
}

static void test_sparse_and_observer(void)
{
    RNetHashConfirm hc;
    /* 4-max room, seats 0 and 2 occupied, we are seat 0. */
    rnet_hc_init_n(&hc, rnet_expected_peer_mask(4, 0x5u, 0));
    rnet_hc_note_local(&hc, 0u, 1u);
    CHECK(!rnet_hc_note_peer_slot(&hc, 1u, 0u, 1u), "sparse: empty seat 1 refused");
    CHECK(!rnet_hc_note_peer_slot(&hc, 3u, 0u, 1u), "sparse: empty seat 3 refused");
    CHECK(!rnet_hc_confirm_through(&hc, 0u), "sparse: not yet");
    rnet_hc_note_peer_slot(&hc, 2u, 0u, 1u);
    CHECK(rnet_hc_confirm_through(&hc, 0u), "sparse: resolves on the only occupied peer");

    /* Observer of a 4-seat room with seats 0,1,3: waits on all three. */
    rnet_hc_init_n(&hc, rnet_expected_peer_mask(4, 0xBu, 4));
    rnet_hc_note_local(&hc, 0u, 5u);
    rnet_hc_note_peer_slot(&hc, 0u, 0u, 5u);
    rnet_hc_note_peer_slot(&hc, 1u, 0u, 5u);
    CHECK(!rnet_hc_confirm_through(&hc, 0u), "observer: seat 3 missing");
    rnet_hc_note_peer_slot(&hc, 3u, 0u, 5u);
    CHECK(rnet_hc_confirm_through(&hc, 0u), "observer: all seats");
}

static void test_ordering(void)
{
    RNetHashConfirm hc;
    uint32_t t;
    rnet_hc_init_n(&hc, 0x6u); /* peers 1, 2 */

    /* Out of order across ticks and seats. */
    rnet_hc_note_peer_slot(&hc, 2u, 2u, dig(2));
    rnet_hc_note_peer_slot(&hc, 1u, 1u, dig(1));
    rnet_hc_note_local(&hc, 2u, dig(2));
    rnet_hc_note_peer_slot(&hc, 1u, 2u, dig(2));
    rnet_hc_note_local(&hc, 1u, dig(1));
    rnet_hc_note_peer_slot(&hc, 2u, 0u, dig(0));
    rnet_hc_note_peer_slot(&hc, 2u, 1u, dig(1));
    CHECK(!rnet_hc_confirm_through(&hc, 0u), "tick 0 still missing local + seat 1");
    rnet_hc_note_local(&hc, 0u, dig(0));
    rnet_hc_note_peer_slot(&hc, 1u, 0u, dig(0));
    CHECK(rnet_hc_resolved_through(&hc) == 2u, "out-of-order resolves contiguously");

    /* Duplicate delivery is idempotent. */
    rnet_hc_note_peer_slot(&hc, 1u, 2u, dig(2));
    rnet_hc_note_peer_slot(&hc, 1u, 2u, dig(2));
    CHECK(rnet_hc_resolved_through(&hc) == 2u, "duplicate idempotent");

    /* Same tick: latest wins (wrong then right resolves; right then wrong
     * stops). */
    rnet_hc_note_local(&hc, 3u, dig(3));
    rnet_hc_note_peer_slot(&hc, 1u, 3u, 0x1u);
    rnet_hc_note_peer_slot(&hc, 1u, 3u, dig(3));
    rnet_hc_note_peer_slot(&hc, 2u, 3u, dig(3));
    CHECK(rnet_hc_resolved_through(&hc) == 3u, "same-tick replacement: latest wins");
    rnet_hc_note_local(&hc, 4u, dig(4));
    rnet_hc_note_peer_slot(&hc, 1u, 4u, dig(4));
    rnet_hc_note_peer_slot(&hc, 2u, 4u, 0x2u);
    CHECK(rnet_hc_resolved_through(&hc) == 3u, "replacement with a wrong digest stops");
    rnet_hc_note_peer_slot(&hc, 2u, 4u, dig(4));
    CHECK(rnet_hc_resolved_through(&hc) == 4u, "and re-correcting resolves");

    /* A stale digest for an already-resolved tick cannot move or break the
     * watermark. */
    rnet_hc_note_peer_slot(&hc, 1u, 1u, 0xBADu);
    CHECK(rnet_hc_resolved_through(&hc) == 4u, "stale resolved-tick digest is inert");
    CHECK(!rnet_hc_peek_mismatch_slot(&hc, NULL, NULL, NULL, NULL),
          "stale digest is not a live mismatch");

    /* Ring wrap: tick 5+RING evicts tick 5 in every ring. */
    rnet_hc_note_peer_slot(&hc, 2u, 5u, dig(5));
    CHECK(rnet_hc_reported_mask(&hc, 5u) == 0x4u, "seat 2 holds tick 5");
    rnet_hc_note_peer_slot(&hc, 2u, 5u + RNET_HC_RING, dig(5u + RNET_HC_RING));
    rnet_hc_note_local(&hc, 5u, dig(5));
    rnet_hc_note_peer_slot(&hc, 1u, 5u, dig(5));
    CHECK(rnet_hc_reported_mask(&hc, 5u) == 0x2u, "ring wrap evicts the older tick");
    CHECK(rnet_hc_resolved_through(&hc) == 4u, "evicted seat holds the watermark");

    /* prime keeps the mode and the mask. */
    rnet_hc_prime_after(&hc, 99u);
    CHECK(rnet_hc_is_nway(&hc) && rnet_hc_peer_mask(&hc) == 0x6u, "prime keeps mode/mask");
    rnet_hc_note_local(&hc, 100u, 1u);
    rnet_hc_note_peer_slot(&hc, 1u, 100u, 1u);
    CHECK(rnet_hc_resolved_through(&hc) == 99u, "prime: still needs seat 2");
    rnet_hc_note_peer_slot(&hc, 2u, 100u, 1u);
    CHECK(rnet_hc_resolved_through(&hc) == 100u, "prime: then resolves");

    /* Membership change: seat 2 leaves while it is the only one missing. */
    for (t = 101; t < 104; ++t) {
        rnet_hc_note_local(&hc, t, dig(t));
        rnet_hc_note_peer_slot(&hc, 1u, t, dig(t));
    }
    CHECK(rnet_hc_resolved_through(&hc) == 100u, "departed seat still expected");
    rnet_hc_set_peer_mask(&hc, 0x2u);
    CHECK(rnet_hc_resolved_through(&hc) == 103u, "set_peer_mask re-evaluates");
    CHECK(!rnet_hc_note_peer_slot(&hc, 2u, 104u, dig(104)), "departed seat now refused");
}

static void test_heal_nway(void)
{
    RNetHashConfirm hc;
    rnet_hc_init_n(&hc, 0x6u);
    rnet_hc_note_local(&hc, 0u, 1u);
    rnet_hc_note_peer_slot(&hc, 1u, 0u, 1u);
    rnet_hc_note_peer_slot(&hc, 2u, 0u, 1u);
    CHECK(rnet_hc_resolved_through(&hc) == 0u, "heal setup");
    /* Tick 1 aged out everywhere; 1+RING and 30+RING complete and equal;
     * 40+RING has only seat 1. */
    rnet_hc_note_local(&hc, 1u + RNET_HC_RING, 0xC1u);
    rnet_hc_note_peer_slot(&hc, 1u, 1u + RNET_HC_RING, 0xC1u);
    rnet_hc_note_peer_slot(&hc, 2u, 1u + RNET_HC_RING, 0xC1u);
    rnet_hc_note_local(&hc, 30u + RNET_HC_RING, 0xC2u);
    rnet_hc_note_peer_slot(&hc, 1u, 30u + RNET_HC_RING, 0xC2u);
    rnet_hc_note_peer_slot(&hc, 2u, 30u + RNET_HC_RING, 0xC2u);
    rnet_hc_note_local(&hc, 40u + RNET_HC_RING, 0xC3u);
    rnet_hc_note_peer_slot(&hc, 1u, 40u + RNET_HC_RING, 0xC3u);
    CHECK(rnet_hc_heal_stale_gap(&hc), "heal moves");
    CHECK(rnet_hc_resolved_through(&hc) == 30u + RNET_HC_RING,
          "heal stops at the highest tick EVERY seat reported");

    /* A later tick with one seat disagreeing blocks any heal. */
    rnet_hc_init_n(&hc, 0x6u);
    rnet_hc_note_local(&hc, 0u, 1u);
    rnet_hc_note_peer_slot(&hc, 1u, 0u, 1u);
    rnet_hc_note_peer_slot(&hc, 2u, 0u, 1u);
    rnet_hc_note_local(&hc, 10u + RNET_HC_RING, 0xD1u);
    rnet_hc_note_peer_slot(&hc, 1u, 10u + RNET_HC_RING, 0xD1u);
    rnet_hc_note_peer_slot(&hc, 2u, 10u + RNET_HC_RING, 0xD1u);
    rnet_hc_note_local(&hc, 20u + RNET_HC_RING, 0xD2u);
    rnet_hc_note_peer_slot(&hc, 2u, 20u + RNET_HC_RING, 0xEEu);
    CHECK(!rnet_hc_heal_stale_gap(&hc), "heal refuses when any later seat disagrees");
}

int main(void)
{
    test_expected_mask();
    test_legacy_equivalence();
    test_mode_isolation();
    test_n3();
    test_n4_diverged_seat();
    test_one_of_n_masking();
    test_sparse_and_observer();
    test_ordering();
    test_heal_nway();
    if (failures) {
        printf("hash_confirm_nway_test: %d/%d check(s) failed\n", failures, checks);
        return 1;
    }
    printf("hash_confirm_nway_test: ALL PASS (%d checks)\n", checks);
    return 0;
}
