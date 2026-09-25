/*
 * N-peer rollback episode coordination.
 *
 * Pins:
 *   - peer-seal completion waits on every OCCUPIED seat (N=3/4, sparse rooms,
 *     observers) and never on an empty one;
 *   - lowest-slot BEGIN arbitration: the rule table, and convergence of a
 *     four-seat room under every delivery order of two concurrent BEGINs;
 *   - the N-way resolved frontier is the minimum over seats, never demotes,
 *     and equals set_peer_convergence with one peer;
 *   - BASELINE/POST agreement: all seats required, any seat's mismatch is a
 *     stop that names it, stale/foreign reports rejected.
 */
#include "recomp_net/rollback.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);            \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

/* Host stub: local seat confirmed, every remote seat PREDICTED, so remote
 * rows only become authoritative through apply_peer_seal_rows. */
typedef struct Host
{
    uint32_t local_slot;
} Host;

static int h_save(void *c, uint32_t t) { (void)c; (void)t; return 0; }
static int h_load(void *c, uint32_t t) { (void)c; (void)t; return 0; }
static int h_adv(void *c, uint32_t t) { (void)c; (void)t; return 0; }
static uint32_t h_dig(void *c, uint32_t t, uint32_t p) { (void)c; (void)p; return t; }
static uint8_t h_hc(void *c, uint32_t t) { (void)c; (void)t; return 0u; }
static uint8_t h_row(void *ctx, int32_t slot, uint32_t tick, RNetRbFrame *out)
{
    Host *h = (Host *)ctx;
    memset(out, 0, sizeof(*out));
    out->tick = tick;
    out->buttons = (uint16_t)(0x10u + (uint32_t)slot);
    out->is_valid = 1u;
    out->is_predicted = ((uint32_t)slot == h->local_slot) ? 0u : 1u;
    return 1u;
}

static RNetRbSession *make(Host *h, uint32_t local, uint32_t slots, uint32_t occupied)
{
    RNetRbConfig cfg;
    RNetRollbackVTable vt;
    memset(&cfg, 0, sizeof(cfg));
    cfg.local_slot = local;
    cfg.slot_count = slots;
    cfg.delay = 2;
    cfg.occupied_mask = occupied;
    memset(&vt, 0, sizeof(vt));
    h->local_slot = local;
    vt.ctx = h;
    vt.save_state = h_save;
    vt.load_state = h_load;
    vt.advance_sim = h_adv;
    vt.state_digest = h_dig;
    vt.hash_confirm_through = h_hc;
    vt.get_input_row = h_row;
    return rnet_rb_create(&cfg, &vt);
}

static void seal(RNetRbSession *s, uint32_t epoch, uint8_t initiator)
{
    RNetRbCorrection corr;
    memset(&corr, 0, sizeof(corr));
    corr.epoch_id = epoch;
    corr.mismatch_tick = 10u;
    corr.load_tick = 10u;
    corr.target_tick = 13u;
    corr.slot = -1;
    corr.initiator = initiator;
    corr.from_peer_notify = initiator ? 0u : 1u;
    rnet_rb_begin_episode(s, &corr);
    rnet_rb_seal_inputs(s, 10u, 13u, -1);
}

static void peer_rows(RNetRbSession *s, uint32_t epoch, int32_t slot)
{
    RNetRbFrame rows[4];
    uint32_t i;
    for (i = 0; i < 4; ++i) {
        memset(&rows[i], 0, sizeof(rows[i]));
        rows[i].tick = 10u + i;
        rows[i].buttons = (uint16_t)(0x10 + slot);
        rows[i].is_valid = 1u;
    }
    CHECK(rnet_rb_apply_peer_seal_rows(s, epoch, 10u, 13u, slot, 0u, rows, 4u),
          "apply peer seal rows");
}

static void test_seal_completion(void)
{
    Host h;
    RNetRbSession *s;

    /* N=4 full room, local seat 1: needs 0, 2, 3. */
    s = make(&h, 1u, 4u, 0u);
    CHECK(s != NULL, "create N=4");
    CHECK(rnet_rb_expected_peer_mask(s) == 0xDu, "N=4 expected mask");
    seal(s, 7u, 1u);
    peer_rows(s, 7u, 0);
    peer_rows(s, 7u, 2);
    CHECK(!rnet_rb_all_peer_seal_rows_complete(s), "N=4: seat 3 missing");
    peer_rows(s, 7u, 3);
    CHECK(rnet_rb_all_peer_seal_rows_complete(s), "N=4: all seats sealed");
    rnet_rb_destroy(s);

    /* N=3. */
    s = make(&h, 0u, 3u, 0u);
    seal(s, 8u, 1u);
    peer_rows(s, 8u, 1);
    CHECK(!rnet_rb_all_peer_seal_rows_complete(s), "N=3: seat 2 missing");
    peer_rows(s, 8u, 2);
    CHECK(rnet_rb_all_peer_seal_rows_complete(s), "N=3: complete");
    rnet_rb_destroy(s);

    /* Sparse: 4-max room, seats 0 and 2 occupied. The pre-fix FSM waited on
     * seats 1 and 3 forever. */
    s = make(&h, 0u, 4u, 0x5u);
    CHECK(rnet_rb_expected_peer_mask(s) == 0x4u, "sparse expected mask");
    seal(s, 9u, 1u);
    CHECK(!rnet_rb_all_peer_seal_rows_complete(s), "sparse: seat 2 missing");
    peer_rows(s, 9u, 2);
    CHECK(rnet_rb_all_peer_seal_rows_complete(s),
          "sparse: completes without the empty seats");
    rnet_rb_destroy(s);

    /* Same room without occupied_mask (legacy config) documents the old
     * behaviour: empty seats are waited on. */
    s = make(&h, 0u, 4u, 0u);
    seal(s, 9u, 1u);
    peer_rows(s, 9u, 2);
    CHECK(!rnet_rb_all_peer_seal_rows_complete(s),
          "legacy (mask 0) still waits on every seat in range");
    rnet_rb_set_occupied_mask(s, 0x5u);
    CHECK(rnet_rb_all_peer_seal_rows_complete(s), "set_occupied_mask applies live");
    rnet_rb_destroy(s);

    /* Observer of seats 0, 1, 3 in a 4-seat room. */
    s = make(&h, 4u, 4u, 0xBu);
    CHECK(s != NULL && rnet_rb_is_observer(s), "observer create");
    CHECK(rnet_rb_expected_peer_mask(s) == 0xBu, "observer waits on all occupied");
    seal(s, 10u, 0u);
    peer_rows(s, 10u, 0);
    peer_rows(s, 10u, 1);
    CHECK(!rnet_rb_all_peer_seal_rows_complete(s), "observer: seat 3 missing");
    peer_rows(s, 10u, 3);
    CHECK(rnet_rb_all_peer_seal_rows_complete(s), "observer: complete");
    rnet_rb_destroy(s);

    /* N=2 unchanged. */
    s = make(&h, 0u, 2u, 0u);
    seal(s, 11u, 1u);
    CHECK(!rnet_rb_all_peer_seal_rows_complete(s), "N=2: seat 1 missing");
    peer_rows(s, 11u, 1);
    CHECK(rnet_rb_all_peer_seal_rows_complete(s), "N=2: complete");
    rnet_rb_destroy(s);
}

static void begin_from(RNetRbSession *s, uint32_t epoch, uint32_t initiator_slot,
                       uint32_t local_slot)
{
    RNetRbCorrection corr;
    memset(&corr, 0, sizeof(corr));
    corr.epoch_id = epoch;
    corr.mismatch_tick = 20u;
    corr.load_tick = 20u;
    corr.target_tick = 22u;
    corr.slot = -1;
    corr.initiator = (initiator_slot == local_slot) ? 1u : 0u;
    corr.from_peer_notify = corr.initiator ? 0u : 1u;
    rnet_rb_begin_episode_from(s, &corr, initiator_slot);
}

static void test_arbitration_table(void)
{
    Host h;
    RNetRbSession *s = make(&h, 2u, 4u, 0u);
    RNetRbCorrection corr;

    CHECK(rnet_rb_get_initiator_slot(s) == RNET_RB_SLOT_NONE, "idle has no initiator");
    CHECK(rnet_rb_arbitrate_begin(s, 3u, 0x13u) == nRNetRbBeginFollow, "idle follows");
    CHECK(rnet_rb_arbitrate_begin(s, 2u, 0x12u) == nRNetRbBeginRefuse,
          "own reflected BEGIN refused");

    /* We initiate. */
    begin_from(s, 0x12u, 2u, 2u);
    CHECK(rnet_rb_get_initiator_slot(s) == 2u, "initiator recorded");
    CHECK(rnet_rb_arbitrate_begin(s, 3u, 0x13u) == nRNetRbBeginRefuse,
          "higher seat refused while we initiate");
    CHECK(rnet_rb_arbitrate_begin(s, 1u, 0x11u) == nRNetRbBeginYield,
          "lower seat wins over our episode");
    CHECK(rnet_rb_arbitrate_begin(s, 0u, 0x10u) == nRNetRbBeginYield, "seat 0 wins");

    /* We follow seat 1. */
    rnet_rb_session_reset(s);
    begin_from(s, 0x21u, 1u, 2u);
    CHECK(rnet_rb_get_initiator_slot(s) == 1u, "follow records sender seat");
    CHECK(rnet_rb_arbitrate_begin(s, 1u, 0x21u) == nRNetRbBeginSameEpisode,
          "same initiator + epoch is the same episode");
    CHECK(rnet_rb_arbitrate_begin(s, 1u, 0x29u) == nRNetRbBeginRefuse,
          "same initiator, new epoch while busy: refuse");
    CHECK(rnet_rb_arbitrate_begin(s, 3u, 0x21u) == nRNetRbBeginRefuse,
          "a colliding epoch from another seat is not our episode");
    CHECK(rnet_rb_arbitrate_begin(s, 0u, 0x30u) == nRNetRbBeginYield,
          "seat 0 outranks the seat we follow");

    /* Terminal phases are idle for arbitration. */
    rnet_rb_set_phase(s, nRNetRbPhaseCommit);
    CHECK(rnet_rb_arbitrate_begin(s, 3u, 0x33u) == nRNetRbBeginFollow, "after Commit: follow");
    rnet_rb_set_phase(s, nRNetRbPhaseAbort);
    CHECK(rnet_rb_arbitrate_begin(s, 3u, 0x33u) == nRNetRbBeginFollow, "after Abort: follow");
    rnet_rb_session_reset(s);
    CHECK(rnet_rb_get_initiator_slot(s) == RNET_RB_SLOT_NONE, "reset clears initiator");

    /* Legacy begin_episode: initiator = local; follower = unknown. */
    memset(&corr, 0, sizeof(corr));
    corr.epoch_id = 5u;
    corr.initiator = 1u;
    corr.slot = -1;
    rnet_rb_begin_episode(s, &corr);
    CHECK(rnet_rb_get_initiator_slot(s) == 2u, "legacy initiator = local seat");
    rnet_rb_session_reset(s);
    corr.initiator = 0u;
    corr.from_peer_notify = 1u;
    rnet_rb_begin_episode(s, &corr);
    CHECK(rnet_rb_get_initiator_slot(s) == RNET_RB_SLOT_NONE, "legacy follower: unknown");
    CHECK(rnet_rb_arbitrate_begin(s, 0u, 5u) == nRNetRbBeginSameEpisode,
          "legacy follower: same epoch is the same episode");
    CHECK(rnet_rb_arbitrate_begin(s, 0u, 6u) == nRNetRbBeginRefuse,
          "legacy follower: unknown initiator never yields");
    rnet_rb_destroy(s);
}

/* ---- four-seat convergence under arbitrary delivery order ---- */

enum { kSeats = 4, kMaxMsgs = 64 };

typedef struct Msg
{
    uint8_t nack;      /* 0 = BEGIN, 1 = NACK */
    uint32_t from, to;
    uint32_t epoch;
} Msg;

typedef struct Room
{
    Host host[kSeats];
    RNetRbSession *s[kSeats];
    Msg q[kMaxMsgs];
    int n;
} Room;

static void room_push(Room *r, uint8_t nack, uint32_t from, uint32_t to, uint32_t epoch)
{
    if (r->n < kMaxMsgs) {
        r->q[r->n].nack = nack;
        r->q[r->n].from = from;
        r->q[r->n].to = to;
        r->q[r->n].epoch = epoch;
        r->n++;
    }
}

static void room_initiate(Room *r, uint32_t seat, uint32_t epoch)
{
    uint32_t to;
    begin_from(r->s[seat], epoch, seat, seat);
    for (to = 0; to < kSeats; ++to)
        if (to != seat)
            room_push(r, 0u, seat, to, epoch);
}

static void room_deliver(Room *r, const Msg *m)
{
    RNetRbSession *s = r->s[m->to];
    if (m->nack) {
        /* Initiator aborts only the episode the NACK names. */
        if (rnet_rb_is_active(s) && rnet_rb_get_epoch_id(s) == m->epoch &&
            rnet_rb_get_initiator_slot(s) == m->to)
            rnet_rb_session_reset(s);
        return;
    }
    switch (rnet_rb_arbitrate_begin(s, m->from, m->epoch)) {
    case nRNetRbBeginFollow:
        begin_from(s, m->epoch, m->from, m->to);
        break;
    case nRNetRbBeginYield:
        rnet_rb_session_reset(s);
        begin_from(s, m->epoch, m->from, m->to);
        break;
    case nRNetRbBeginRefuse:
        room_push(r, 1u, m->to, m->from, m->epoch);
        break;
    case nRNetRbBeginSameEpisode:
        break;
    }
}

static void test_convergence(void)
{
    uint32_t seed;
    int trials_ok = 0;
    for (seed = 1; seed <= 2000u; ++seed) {
        Room r;
        uint32_t i, rng = seed * 2654435761u;
        int converged = 1;
        memset(&r, 0, sizeof(r));
        for (i = 0; i < kSeats; ++i)
            r.s[i] = make(&r.host[i], i, kSeats, 0u);
        /* Seats 2 and 1 detect a mismatch concurrently (either order). */
        if (seed & 1u) {
            room_initiate(&r, 2u, (1u << 3) | 2u);
            room_initiate(&r, 1u, (1u << 3) | 1u);
        } else {
            room_initiate(&r, 1u, (1u << 3) | 1u);
            room_initiate(&r, 2u, (1u << 3) | 2u);
        }
        /* Deliver in a random order, including messages generated on the
         * way (NACKs). */
        while (r.n > 0) {
            uint32_t pick;
            Msg m;
            rng = rng * 1103515245u + 12345u;
            pick = (rng >> 8) % (uint32_t)r.n;
            m = r.q[pick];
            r.q[pick] = r.q[r.n - 1];
            r.n--;
            room_deliver(&r, &m);
        }
        for (i = 0; i < kSeats; ++i) {
            if (!rnet_rb_is_active(r.s[i]) || rnet_rb_get_initiator_slot(r.s[i]) != 1u ||
                rnet_rb_get_epoch_id(r.s[i]) != ((1u << 3) | 1u))
                converged = 0;
        }
        if (converged)
            trials_ok++;
        else if (g_failures < 5)
            fprintf(stderr, "  seed %u did not converge\n", (unsigned)seed);
        for (i = 0; i < kSeats; ++i)
            rnet_rb_destroy(r.s[i]);
    }
    CHECK(trials_ok == 2000, "all 2000 delivery orders converge on seat 1's episode");
}

static void test_peer_resolved(void)
{
    Host h;
    RNetRbSession *s = make(&h, 0u, 4u, 0u);
    uint32_t t = 0;
    CHECK(!rnet_rb_note_peer_resolved(s, 1u, 10u), "one of three: no move");
    CHECK(!rnet_rb_note_peer_resolved(s, 2u, 20u), "two of three: no move");
    CHECK(rnet_rb_resolved_through(s) == 0u, "frontier waits on every seat");
    CHECK(rnet_rb_note_peer_resolved(s, 3u, 15u), "all three: moves");
    CHECK(rnet_rb_resolved_through(s) == 10u, "frontier = min over seats");
    CHECK(rnet_rb_note_peer_resolved(s, 1u, 30u), "lagging seat advances");
    CHECK(rnet_rb_resolved_through(s) == 15u, "new min");
    CHECK(!rnet_rb_note_peer_resolved(s, 1u, 5u), "a seat's demote never demotes us");
    CHECK(rnet_rb_resolved_through(s) == 15u, "watermark held");
    CHECK(rnet_rb_peer_resolved(s, 1u, &t) && t == 5u, "per-seat latest kept");
    CHECK(!rnet_rb_note_peer_resolved(s, 0u, 99u), "own seat refused");
    CHECK(!rnet_rb_note_peer_resolved(s, 64u, 99u), "spectator refused");
    rnet_rb_session_reset(s);
    CHECK(rnet_rb_resolved_through(s) == 15u, "frontier survives reset");
    CHECK(rnet_rb_peer_resolved(s, 2u, &t) && t == 20u, "per-seat survives reset");
    rnet_rb_destroy(s);

    /* Sparse room, and a seat leaving. */
    s = make(&h, 0u, 4u, 0xDu); /* seats 0, 2, 3 */
    CHECK(!rnet_rb_note_peer_resolved(s, 1u, 50u), "empty seat refused");
    rnet_rb_note_peer_resolved(s, 2u, 40u);
    CHECK(rnet_rb_resolved_through(s) == 0u, "seat 3 missing");
    rnet_rb_set_occupied_mask(s, 0x5u); /* seat 3 leaves */
    CHECK(rnet_rb_resolved_through(s) == 40u, "departure re-evaluates frontier");
    rnet_rb_destroy(s);

    /* N=2: identical to set_peer_convergence. */
    {
        Host h2;
        RNetRbSession *a = make(&h, 0u, 2u, 0u);
        RNetRbSession *b = make(&h2, 0u, 2u, 0u);
        static const uint32_t seq[] = {5u, 3u, 9u, 9u, 2u, 40u, 1u};
        size_t i;
        for (i = 0; i < sizeof(seq) / sizeof(seq[0]); ++i) {
            rnet_rb_set_peer_convergence(a, seq[i]);
            rnet_rb_note_peer_resolved(b, 1u, seq[i]);
            CHECK(rnet_rb_resolved_through(a) == rnet_rb_resolved_through(b),
                  "N=2 note_peer_resolved == set_peer_convergence");
        }
        rnet_rb_destroy(a);
        rnet_rb_destroy(b);
    }
}

static void test_agree(void)
{
    RNetRbPeerAgree a;
    uint32_t loc[4] = {1u, 2u, 3u, 4u};
    uint32_t bad[4] = {1u, 2u, 0xEEu, 4u};
    uint32_t slot = 0, word = 0;

    rnet_rb_agree_begin(&a, 0xEu, 0x21u, 100u, 4u); /* peers 1, 2, 3 */
    CHECK(rnet_rb_agree_status(&a, &slot, &word) == nRNetRbAgreePending, "empty: pending");
    CHECK(rnet_rb_agree_note(&a, 1u, 0x21u, 100u, loc), "seat 1 before local");
    CHECK(rnet_rb_agree_status(&a, NULL, NULL) == nRNetRbAgreePending,
          "no local yet: pending");
    rnet_rb_agree_set_local(&a, loc);
    CHECK(rnet_rb_agree_status(&a, NULL, NULL) == nRNetRbAgreePending, "seats 2,3 missing");
    CHECK(rnet_rb_agree_missing_mask(&a) == 0xCu, "missing mask");
    CHECK(rnet_rb_agree_note(&a, 3u, 0x21u, 100u, bad), "seat 3 diverged");
    CHECK(rnet_rb_agree_status(&a, &slot, &word) == nRNetRbAgreeMismatch,
          "mismatch before seat 2 reports");
    CHECK(slot == 3u && word == 2u, "names seat 3, word 2");
    CHECK(rnet_rb_agree_note(&a, 3u, 0x21u, 100u, loc), "seat 3 corrected (latest wins)");
    CHECK(rnet_rb_agree_note(&a, 2u, 0x21u, 100u, loc), "seat 2");
    CHECK(rnet_rb_agree_status(&a, &slot, NULL) == nRNetRbAgreeMatch &&
              slot == RNET_RB_SLOT_NONE,
          "match once every seat agrees");

    CHECK(!rnet_rb_agree_note(&a, 1u, 0x20u, 100u, loc), "stale epoch rejected");
    CHECK(!rnet_rb_agree_note(&a, 1u, 0x21u, 101u, loc), "other tick rejected");
    CHECK(!rnet_rb_agree_note(&a, 0u, 0x21u, 100u, loc), "own seat rejected");
    CHECK(!rnet_rb_agree_note(&a, 64u, 0x21u, 100u, loc), "spectator rejected");

    /* Seat leaves mid-checkpoint. */
    rnet_rb_agree_begin(&a, 0x6u, 1u, 7u, 2u);
    rnet_rb_agree_set_local(&a, loc);
    rnet_rb_agree_note(&a, 1u, 1u, 7u, loc);
    CHECK(rnet_rb_agree_status(&a, NULL, NULL) == nRNetRbAgreePending, "seat 2 missing");
    rnet_rb_agree_set_peer_mask(&a, 0x2u);
    CHECK(rnet_rb_agree_status(&a, NULL, NULL) == nRNetRbAgreeMatch, "seat 2 dropped");

    /* Only word_count words compared. */
    rnet_rb_agree_begin(&a, 0x2u, 1u, 7u, 2u);
    rnet_rb_agree_set_local(&a, loc);
    rnet_rb_agree_note(&a, 1u, 1u, 7u, bad); /* differs only in word 2 */
    CHECK(rnet_rb_agree_status(&a, NULL, NULL) == nRNetRbAgreeMatch,
          "words past word_count ignored");

    rnet_rb_agree_begin(&a, 0u, 1u, 7u, 1u);
    rnet_rb_agree_set_local(&a, loc);
    CHECK(rnet_rb_agree_status(&a, NULL, NULL) == nRNetRbAgreePending,
          "empty peer mask never matches");
}

int main(void)
{
    test_seal_completion();
    test_arbitration_table();
    test_convergence();
    test_peer_resolved();
    test_agree();
    if (g_failures) {
        fprintf(stderr, "rollback_nway_test: %d/%d check(s) failed\n", g_failures, g_checks);
        return 1;
    }
    printf("rollback_nway_test: ALL PASS (%d checks)\n", g_checks);
    return 0;
}
