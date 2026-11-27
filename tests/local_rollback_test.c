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
    rnet_u64 clock_offset;
    rnet_u32 simulate_cost, simulate_load_cost, transport_rtt;
    int network_ready;
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
static rnet_u64 now(void *ctx) {
    Fixture *f = ctx; return f->wall * 16667ull + f->clock_offset + 1u;
}
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
    f->state = f->snapshots[i].state; ++f->loads;
    f->clock_offset += f->simulate_load_cost; return 1;
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
    f->state = step(f->state, t, f->published, f->unread);
    f->clock_offset += f->simulate_cost; return 1;
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
static int network(void *ctx, RNetPeerTelemetry *peer) {
    Fixture *f = ctx; memset(peer, 0, sizeof(*peer));
    peer->rtt_us = f->transport_rtt; peer->received = 1;
    return f->network_ready;
}
static void controller_scenario(void) {
    Fixture *f = calloc(1, sizeof(*f));
    RNetLocalConfig cfg = {0}; RNetLocalHost host = {0};
    RNetLocalControlConfig control = {0}; RNetLocalControlState state;
    rnet_u32 t, previous = 1, largest = 1, zero_depth_ticks = 0;
    CHECK(f != NULL); if (!f) return;
    f->state = 2166136261u; f->simulate_cost = 8000; f->simulate_load_cost = 3000;
    f->network_ready = 1;
    for (t = 0; t < TICKS; ++t) {
        int s; for (s = 0; s < 2; ++s) {
            f->truth[s][t].size = 32; f->truth[s][t].valid = 1;
            f->truth[s][t].bytes[31] = (rnet_u8)(t * 13u + (unsigned)s);
        }
        /* Remote rows are genuinely late; depth zero must wait, not deadlock. */
        f->release[t] = t + 2u;
    }
    cfg.seats = 2; cfg.row_size = 32; cfg.delay = 1;
    cfg.rollback_depth = 12; cfg.tick_us = 16667;
    host.ctx = f; host.save = save; host.load = load; host.drop_after = drop;
    host.publish = publish; host.run_tick = run; host.resim_begin = begin; host.resim_end = end;
    host.digest = digest; host.now_us = now; host.neutral = neutral;
    host.input = input; host.sample = sample; host.network = network;
    f->driver = rnet_local_create(&cfg, &host); CHECK(f->driver != NULL);
    if (!f->driver) { free(f); return; }
    control.frame_budget_us = 11000; control.live_tick_us = 8000; control.replay_setup_us = 1000;
    control.replay_tick_us = 1000; control.network_valid = 1; control.interval_ticks = 8;
    control.one_way_us = 100000;
    CHECK(rnet_local_control_preview(&control, cfg.tick_us, cfg.rollback_depth, &state));
    CHECK(state.delay == 4 && state.rollback_depth == 2);
    control.replay_setup_us = 3000;
    CHECK(rnet_local_control_preview(&control, cfg.tick_us, cfg.rollback_depth, &state));
    CHECK(state.delay == 6 && state.rollback_depth == 0);
    control.replay_setup_us = 1000;
    control.one_way_us = 0;
    CHECK(rnet_local_control_begin(f->driver, &control));
    CHECK(!rnet_local_control_begin(f->driver, &control));
    CHECK(rnet_local_set_hold(f->driver, TICKS));
    for (f->wall = 0; f->wall < TICKS + 40u; ++f->wall) {
        int a;
        f->transport_rtt = f->wall < 8u || f->wall >= 100u ? 0u : 200000u;
        /* Missing peer observations cannot lower the settled delay. */
        f->network_ready = !(f->wall >= 100u && f->wall < 124u);
        if (f->wall >= 180u) f->simulate_cost = 10000;
        a = rnet_local_poll(f->driver);
        if (a) { run(f, f->row_tick); CHECK(rnet_local_finish(f->driver)); }
        CHECK(rnet_local_control_state(f->driver, &state));
        CHECK(state.delay <= previous + 1u && previous <= state.delay + 1u);
        if (!f->network_ready) CHECK(state.delay == previous);
        previous = state.delay; if (state.delay > largest) largest = state.delay;
        if (a == 1 && !state.rollback_depth) ++zero_depth_ticks;
        if (rnet_local_refusal(f->driver)) break;
    }
    CHECK(!rnet_local_refusal(f->driver)); CHECK(rnet_local_confirmed(f->driver) == TICKS);
    CHECK(largest > 1 && state.delay == 1 && zero_depth_ticks > 10);
    CHECK(state.replay_setup_us > 1000u && state.replay_setup_us <= 3000u);
    {
        rnet_u32 expected = 2166136261u;
        for (t = 0; t < TICKS; ++t) {
            RNetInputSample rows[2] = {f->truth[0][t], f->truth[1][t]};
            expected = step(expected, t, rows, 0);
        }
        CHECK(f->state == expected);
    }
    CHECK(!rnet_local_control_begin(f->driver, &control));
    printf("controller: peak_delay=%u final_delay=%u zero_depth_ticks=%u\n",
        largest, state.delay, zero_depth_ticks);
    rnet_local_destroy(f->driver); free(f);
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
    CHECK(rnet_local_set_hold(f->driver, TICKS));
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
    CHECK(rnet_local_report(f->driver, report, sizeof(report)) > 0);
    {
        unsigned long long replay_total = 0;
        const char *field = strstr(report, "\"replay_ticks\":");
        CHECK(field && sscanf(field, "\"replay_ticks\":%llu", &replay_total) == 1);
        CHECK(replay_total == f->replayed);
        {
            char again[4096];
            CHECK(rnet_local_report(f->driver, again, sizeof(again)) == strlen(report) + 1u);
            CHECK(!strcmp(again, report));
            CHECK(!rnet_telemetry_read(rnet_local_telemetry(f->driver), TICKS, records, 8));
        }
    }
    CHECK(strstr(report, "local-v4") && strstr(report, "\"episodes\":0"));
    printf("%s %s %s: ticks=%u replay=%u corrections=%u digest=%08x\n",
        incremental ? "incremental" : "inline", unread ? "30Hz polls" : "60Hz polls",
        right_stick_only ? "byte31" : "analog", rnet_local_tick(f->driver), f->replayed, f->loads, f->state);
    rnet_local_destroy(f->driver); free(f);
}
int main(void) {
    rnet_u32 depth;
    scenario(0, 0, 0); scenario(1, 0, 0); scenario(1, 1, 0); scenario(1, 0, 1);
    controller_scenario();
    CHECK(rnet_local_budget_delay(100000, 10000, 1000, 8000, 16667, 12, &depth) == 1 && depth == 8);
    CHECK(rnet_local_budget_delay(100000, 10000, 8000, 15000, 16667, 12, &depth) == 7 && depth == 0);
    return failures ? 1 : 0;
}
