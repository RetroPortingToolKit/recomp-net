#include "recomp_net/local_rollback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TICKS 360u
typedef struct Fixture {
    rnet_u32 wall, state, row_tick, consumed, replayed, loads, begins, ends;
    rnet_u32 release[TICKS], oracle[TICKS];
    struct { rnet_u32 tick, state; int valid; } snapshots[RNET_LOCAL_HISTORY];
    RNetInputSample truth[2][TICKS], published[2];
    RNetLocalDriver *driver;
    int unread;
} Fixture;
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); ++failures; } } while (0)
static rnet_u32 step(rnet_u32 state, rnet_u32 tick, const RNetInputSample *rows, int unread) {
    unsigned s, b;
    for (s = 0; s < 2; ++s) {
        if (s && unread && (tick & 1u)) continue;
        for (b = 0; b < 32; ++b) state = (state ^ rows[s].bytes[b]) * 16777619u;
    }
    return (state ^ tick) * 16777619u;
}
static rnet_u64 now(void *ctx) { return ((Fixture *)ctx)->wall * 16667ull + 1u; }
static int save(void *ctx, rnet_u32 t) {
    Fixture *f = ctx; unsigned i = t % RNET_LOCAL_HISTORY;
    f->snapshots[i].tick = t; f->snapshots[i].state = f->state; f->snapshots[i].valid = 1;
    return 1;
}
static int load(void *ctx, rnet_u32 t) {
    Fixture *f = ctx; unsigned i = t % RNET_LOCAL_HISTORY, before;
    CHECK(f->snapshots[i].valid && f->snapshots[i].tick == t);
    CHECK(t == rnet_local_confirmed(f->driver));
    for (before = 0; before < t; ++before) CHECK(f->release[before] <= f->wall);
    f->state = f->snapshots[i].state; ++f->loads; return 1;
}
static void drop(void *ctx, rnet_u32 t) {
    Fixture *f = ctx; unsigned i;
    for (i = 0; i < RNET_LOCAL_HISTORY; ++i)
        if (f->snapshots[i].valid && f->snapshots[i].tick > t) f->snapshots[i].valid = 0;
}
static void publish(void *ctx, rnet_u32 t, const RNetInputSample *rows, int seats, int replay) {
    Fixture *f = ctx; CHECK(seats == 2); f->row_tick = t;
    memcpy(f->published, rows, sizeof(f->published));
    f->consumed = f->unread && (t & 1u) ? 1u : 3u;
    if (replay) ++f->replayed;
}
static int run(void *ctx, rnet_u32 t) {
    Fixture *f = ctx; CHECK(t == f->row_tick);
    f->state = step(f->state, t, f->published, f->unread); return 1;
}
static void begin(void *ctx) { ++((Fixture *)ctx)->begins; }
static void end(void *ctx) { ++((Fixture *)ctx)->ends; }
static rnet_u32 digest(void *ctx) { return ((Fixture *)ctx)->state; }
static rnet_u32 consumed(void *ctx, rnet_u32 t) { (void)t; return ((Fixture *)ctx)->consumed; }
static int input(void *ctx, int seat, rnet_u32 t, RNetInputSample *row) {
    Fixture *f = ctx;
    if (t >= TICKS || (seat && f->release[t] > f->wall)) return 0;
    *row = f->truth[seat][t]; return 1;
}
static int sample(void *ctx, rnet_u32 t, rnet_u32 delay) { (void)ctx; (void)delay; return t < TICKS; }
static void neutral(void *ctx, int seat, RNetInputSample *row) {
    (void)ctx; (void)seat; memset(row, 0, sizeof(*row)); row->size = 32; row->valid = 1;
}
static void scenario(int incremental, int unread, int right_stick_only) {
    Fixture *f = calloc(1, sizeof(*f));
    RNetLocalConfig cfg = {0}; RNetLocalHost host = {0};
    rnet_u32 t, state = 2166136261u; unsigned b, s;
    RNetTickTelemetry records[8]; char report[4096];
    CHECK(f != NULL); if (!f) return;
    f->unread = unread; f->state = state;
    for (t = 0; t < TICKS; ++t) {
        RNetInputSample rows[2];
        for (s = 0; s < 2; ++s) {
            RNetInputSample *row = &f->truth[s][t]; row->tick = t; row->size = 32; row->valid = 1;
            for (b = 0; b < 32; ++b)
                row->bytes[b] = right_stick_only ? (b == 31 ? (rnet_u8)(t * 13u + s) : 0u)
                    : (rnet_u8)((t * (s ? 13u : 3u) + b * 17u) ^ (t >> 2));
            rows[s] = *row;
        }
        /* Real rows are unavailable; an older unresolved row arrives after a newer correction. */
        f->release[t] = t + 3u + (t % 11u == 0u ? 4u : 0u);
        state = step(state, t, rows, unread); f->oracle[t] = state;
    }
    cfg.seats = 2; cfg.local_seat = 0; cfg.row_size = 32;
    cfg.delay = 1; cfg.rollback_depth = 12; cfg.tick_us = 16667;
    host.ctx = f; host.save = save; host.load = load; host.drop_after = drop;
    host.publish = publish; host.resim_begin = begin; host.resim_end = end;
    host.digest = digest; host.now_us = now; host.neutral = neutral;
    host.input = input; host.sample = sample; host.consumed_seats = consumed;
    if (!incremental) host.run_tick = run;
    f->driver = rnet_local_create(&cfg, &host); CHECK(f->driver != NULL);
    if (!f->driver) { free(f); return; }
    /* Finite input defines this workload; no wall-clock kill. */
    for (f->wall = 0; f->wall < TICKS + 40u; ++f->wall) {
        int a;
        do {
            a = rnet_local_poll(f->driver);
            if (a) { run(f, f->row_tick); CHECK(rnet_local_finish(f->driver)); }
        } while (a == 2);
        if (rnet_local_refusal(f->driver)) break;
    }
    if (rnet_local_refusal(f->driver)) fprintf(stderr,"refused: %s\n",rnet_local_refusal(f->driver));
    CHECK(!rnet_local_refusal(f->driver)); CHECK(rnet_local_tick(f->driver) == TICKS);
    CHECK(rnet_local_confirmed(f->driver) == TICKS); CHECK(f->state == f->oracle[TICKS - 1u]);
    CHECK(f->loads > 10 && f->begins == f->ends && f->replayed > 100);
    CHECK(rnet_telemetry_read(rnet_local_telemetry(f->driver), 0, records, 8) == 8);
    CHECK(rnet_telemetry_report(rnet_local_telemetry(f->driver), report, sizeof(report)) > 0);
    CHECK(strstr(report, "local-v4") && strstr(report, "\"episodes\":0"));
    printf("%s %s %s: ticks=%u replay=%u corrections=%u digest=%08x\n",
        incremental ? "incremental" : "inline", unread ? "30Hz polls" : "60Hz polls",
        right_stick_only ? "byte31" : "analog", rnet_local_tick(f->driver), f->replayed, f->loads, f->state);
    rnet_local_destroy(f->driver); free(f);
}
int main(void) {
    rnet_u32 depth;
    scenario(0, 0, 0); scenario(1, 0, 0); scenario(1, 1, 0); scenario(1, 0, 1);
    CHECK(rnet_local_budget_delay(100000, 10000, 1000, 8000, 16667, 12, &depth) == 1 && depth == 8);
    CHECK(rnet_local_budget_delay(100000, 10000, 8000, 15000, 16667, 12, &depth) == 7 && depth == 0);
    return failures ? 1 : 0;
}
