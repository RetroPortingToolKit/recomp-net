/*
 * Four real UDP peers over loopback (LAN hub star, the shape the lobby relay
 * produces) exchanging every rollback control packet.
 *
 * Pins:
 *   1. every queued FRAME_COMMIT / SYNC / SEAL_ROWS / BASELINE / POST /
 *      RESOLVED carries its sender's seat, taken from the header local_slot
 *      (no wire change), and each receiver hears each other seat exactly once;
 *   2. the legacy (unattributed) takes drain the same queues unchanged;
 *   3. rnet_session_set_rb_peer_mask scopes episode packets to a seat group
 *      while FRAME_COMMIT / SEAL_ROWS / RESOLVED stay session-wide;
 *   4. end to end: FRAME_COMMITs taken with _from and fed to an N-way
 *      RNetHashConfirm stop on the one diverged seat and name it.
 */
#include "recomp_net/recomp_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
static unsigned test_pid(void) { return (unsigned)_getpid(); }
#else
#include <unistd.h>
static unsigned test_pid(void) { return (unsigned)getpid(); }
#endif

enum { kSeats = 4 };

static int g_failures;
static rnet_u64 g_now_ms = 1000;

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    g_failures++;
}

static void sample_local(rnet_u32 tick, RNetInputSample *out, void *opaque)
{
    (void)tick;
    (void)opaque;
    memset(out, 0, sizeof(*out));
    out->size = 2;
    out->valid = 1;
}

static void publish(rnet_u32 tick, const RNetInputSample *by_slot, int slots, void *opaque)
{
    (void)tick;
    (void)by_slot;
    (void)slots;
    (void)opaque;
}

static rnet_u64 now_ms(void *opaque)
{
    (void)opaque;
    return g_now_ms;
}

static void pump_all(RNetSession **s, int rounds)
{
    int r, i;
    for (r = 0; r < rounds; ++r) {
        g_now_ms++;
        for (i = 0; i < kSeats; ++i)
            rnet_session_pump(s[i]);
    }
}

/* Per-seat distinctive values so a receiver can check attribution. */
static rnet_u32 v(int seat, int k) { return 0x1000u * (rnet_u32)(seat + 1) + (rnet_u32)k; }

int main(void)
{
    RNetConfig cfg[kSeats];
    RNetHostVTable vt;
    RNetSession *s[kSeats] = {NULL, NULL, NULL, NULL};
    char bind[kSeats][64];
    const unsigned base_port = 43300u + (test_pid() % 900u) * 4u;
    int i, seat, rx;

    memset(&vt, 0, sizeof(vt));
    vt.sample_local = sample_local;
    vt.publish = publish;
    vt.now_ms = now_ms;

    for (i = 0; i < kSeats; ++i) {
        rnet_config_init_defaults(&cfg[i]);
        cfg[i].slot_count = kSeats;
        cfg[i].local_slot = (rnet_u8)i;
        cfg[i].input_delay = 2;
        cfg[i].session_id = 0x4e574159u ^ test_pid();
        snprintf(bind[i], sizeof(bind[i]), "127.0.0.1:%u", base_port + (unsigned)i);
        s[i] = rnet_session_create(&cfg[i], &vt);
        if (s[i] == NULL) {
            fail("session create");
            goto done;
        }
    }
    if (rnet_session_start_lan_hub(s[0], bind[0]) != 0) {
        fail("hub start");
        goto done;
    }
    for (i = 1; i < kSeats; ++i) {
        if (rnet_session_start_lan(s[i], bind[i], bind[0]) != 0) {
            fail("guest start");
            goto done;
        }
    }
    for (i = 0; i < 20000; ++i) {
        int all = 1;
        pump_all(s, 1);
        for (seat = 0; seat < kSeats; ++seat)
            all &= rnet_session_is_running(s[seat]);
        if (all)
            break;
    }
    for (seat = 0; seat < kSeats; ++seat)
        if (!rnet_session_is_running(s[seat])) {
            fail("a seat did not reach RUNNING");
            goto done;
        }

    /* ---- 1. every packet type, one from each seat ---- */
    for (seat = 0; seat < kSeats; ++seat) {
        RNetRbFrame rows[3];
        int k;
        for (k = 0; k < 3; ++k) {
            memset(&rows[k], 0, sizeof(rows[k]));
            rows[k].buttons = (uint16_t)v(seat, 40 + k);
            rows[k].is_valid = 1u;
        }
        rnet_session_send_rb_frame_commit(s[seat], v(seat, 1), v(seat, 2));
        rnet_session_send_rb_sync(s[seat], v(seat, 3), v(seat, 4), v(seat, 5), v(seat, 6),
                                  (rnet_u8)seat, RNET_RB_SYNC_OP_BEGIN, 0u);
        rnet_session_send_rb_seal_rows(s[seat], v(seat, 7), v(seat, 8), v(seat, 9),
                                       (rnet_u8)seat, 0u, rows, 3u);
        rnet_session_send_rb_baseline(s[seat], v(seat, 10), v(seat, 11), v(seat, 12),
                                      v(seat, 13), v(seat, 14), v(seat, 15));
        rnet_session_send_rb_post(s[seat], v(seat, 16), v(seat, 17), v(seat, 18),
                                  v(seat, 19), 1u);
        rnet_session_send_rb_resolved(s[seat], v(seat, 20));
    }
    pump_all(s, 200);

    for (rx = 0; rx < kSeats; ++rx) {
        unsigned seen[6] = {0, 0, 0, 0, 0, 0};
        rnet_u8 from;
        rnet_u32 a, b, c, d, e, f;
        rnet_u8 cs, op, fl, slot;
        RNetRbFrame rows[RNET_RB_MAX_SLOTS * 8];
        rnet_u16 count;
        rnet_u32 row_begin;

        while (rnet_session_take_rb_frame_commit_from(s[rx], &from, &a, &b)) {
            if (from >= kSeats || from == rx || a != v(from, 1) || b != v(from, 2))
                fail("FRAME_COMMIT attribution");
            else
                seen[0] |= 1u << from;
        }
        while (rnet_session_take_rb_sync_from(s[rx], &from, &a, &b, &c, &d, &cs, &op, &fl)) {
            if (from >= kSeats || from == rx || a != v(from, 3) || d != v(from, 6) ||
                cs != from || op != RNET_RB_SYNC_OP_BEGIN)
                fail("SYNC attribution");
            else
                seen[1] |= 1u << from;
        }
        while (rnet_session_take_rb_seal_rows_from(s[rx], &from, &a, &b, &c, &slot,
                                                   &row_begin, rows, &count)) {
            if (from >= kSeats || from == rx || a != v(from, 7) || slot != from ||
                count != 3u || rows[2].buttons != (uint16_t)v(from, 42))
                fail("SEAL_ROWS attribution");
            else
                seen[2] |= 1u << from;
        }
        while (rnet_session_take_rb_baseline_from(s[rx], &from, &a, &b, &c, &d, &e, &f)) {
            if (from >= kSeats || from == rx || a != v(from, 10) || f != v(from, 15))
                fail("BASELINE attribution");
            else
                seen[3] |= 1u << from;
        }
        while (rnet_session_take_rb_post_from(s[rx], &from, &a, &b, &c, &d, &op)) {
            if (from >= kSeats || from == rx || a != v(from, 16) || d != v(from, 19))
                fail("POST attribution");
            else
                seen[4] |= 1u << from;
        }
        while (rnet_session_take_rb_resolved_from(s[rx], &from, &a)) {
            if (from >= kSeats || from == rx || a != v(from, 20))
                fail("RESOLVED attribution");
            else
                seen[5] |= 1u << from;
        }
        for (i = 0; i < 6; ++i) {
            unsigned want = 0xFu & ~(1u << rx);
            if (seen[i] != want) {
                fprintf(stderr, "  seat %d type %d heard mask %x want %x\n", rx, i,
                        seen[i], want);
                fail("a seat did not hear every other seat exactly");
            }
        }
    }

    /* ---- 2. legacy takes still drain the same queue ---- */
    rnet_session_send_rb_baseline(s[2], 7u, 8u, 9u, 10u, 11u, 12u);
    rnet_session_send_rb_frame_commit(s[2], 55u, 66u);
    pump_all(s, 200);
    {
        rnet_u32 a = 0, b = 0, c, d, e, f;
        if (!rnet_session_take_rb_baseline(s[1], &a, &b, &c, &d, &e, &f) || a != 7u ||
            f != 12u)
            fail("legacy baseline take");
        if (rnet_session_take_rb_baseline_from(s[1], NULL, &a, &b, &c, &d, &e, &f))
            fail("legacy take did not consume the entry");
        if (!rnet_session_take_rb_frame_commit(s[3], &a, &b) || a != 55u || b != 66u)
            fail("legacy frame-commit take");
        while (rnet_session_take_rb_baseline(s[0], NULL, NULL, NULL, NULL, NULL, NULL)) {}
        while (rnet_session_take_rb_baseline(s[3], NULL, NULL, NULL, NULL, NULL, NULL)) {}
        while (rnet_session_take_rb_frame_commit(s[0], NULL, NULL)) {}
        while (rnet_session_take_rb_frame_commit(s[1], NULL, NULL)) {}
    }

    /* ---- 3. group scoping ---- */
    rnet_session_set_rb_peer_mask(s[3], 0x1u); /* episode packets only from seat 0 */
    for (seat = 0; seat < 3; ++seat) {
        rnet_session_send_rb_post(s[seat], 100u + (rnet_u32)seat, 0u, 0u, 0u, 0u);
        rnet_session_send_rb_frame_commit(s[seat], 200u + (rnet_u32)seat, 0u);
    }
    pump_all(s, 200);
    {
        rnet_u8 from;
        rnet_u32 a;
        unsigned posts = 0, commits = 0;
        while (rnet_session_take_rb_post_from(s[3], &from, &a, NULL, NULL, NULL, NULL))
            posts |= 1u << from;
        while (rnet_session_take_rb_frame_commit_from(s[3], &from, &a, NULL))
            commits |= 1u << from;
        if (posts != 0x1u)
            fail("peer mask: POST accepted from outside the group");
        if (commits != 0x7u)
            fail("peer mask: FRAME_COMMIT must stay session-wide");
        rnet_session_set_rb_peer_slot(s[3], 1);
        rnet_session_send_rb_post(s[0], 1u, 0u, 0u, 0u, 0u);
        rnet_session_send_rb_post(s[1], 2u, 0u, 0u, 0u, 0u);
        pump_all(s, 200);
        posts = 0;
        while (rnet_session_take_rb_post_from(s[3], &from, &a, NULL, NULL, NULL, NULL))
            posts |= 1u << from;
        if (posts != 0x2u)
            fail("set_rb_peer_slot must replace the mask");
        rnet_session_set_rb_peer_slot(s[3], -1);
        for (i = 0; i < kSeats; ++i)
            while (rnet_session_take_rb_post(s[i], NULL, NULL, NULL, NULL, NULL)) {}
        for (i = 0; i < kSeats; ++i)
            while (rnet_session_take_rb_frame_commit(s[i], NULL, NULL)) {}
    }

    /* ---- 4. end to end into an N-way watermark ---- */
    {
        RNetHashConfirm hc;
        rnet_u32 t;
        rnet_u8 from;
        rnet_u32 tick, hash, mt = 0, ms = 0;
        rnet_hc_init_n(&hc, rnet_config_expected_peer_mask(&cfg[0]));
        for (t = 0; t < 10; ++t) {
            rnet_hc_note_local(&hc, t, 0xA000u + t);
            for (seat = 1; seat < kSeats; ++seat) {
                rnet_u32 h = (seat == 2 && t >= 6) ? 0xBAD0u + t : 0xA000u + t;
                rnet_session_send_rb_frame_commit(s[seat], t, h);
            }
        }
        pump_all(s, 400);
        while (rnet_session_take_rb_frame_commit_from(s[0], &from, &tick, &hash))
            rnet_hc_note_peer_slot(&hc, from, tick, hash);
        if (rnet_hc_resolved_through(&hc) != 5u) {
            fprintf(stderr, "  resolved=%u\n", (unsigned)rnet_hc_resolved_through(&hc));
            fail("N-way watermark must stop before the diverged seat's first bad tick");
        }
        if (!rnet_hc_peek_mismatch_slot(&hc, &mt, &ms, NULL, NULL) || mt != 6u || ms != 2u)
            fail("mismatch must name seat 2 at tick 6");
    }

done:
    for (i = 0; i < kSeats; ++i)
        rnet_session_destroy(s[i]);
    if (g_failures == 0) {
        printf("session_nway_test: ok (4 seats, 6 packet types attributed)\n");
        return 0;
    }
    fprintf(stderr, "session_nway_test: %d failure(s)\n", g_failures);
    return 1;
}
