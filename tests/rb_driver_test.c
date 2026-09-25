/*
 * rb_driver_test -- the rollback episode driver against itself.
 *
 * Two parts.
 *
 * In-process: the driver's contracts that need no peer -- epoch encoding, and
 * start() refusing (and saying why) when the host leaves a hole.
 *
 * Two processes: fork() a pair, each with its own RNetSession over UDP
 * loopback, its own driver, and a toy deterministic engine. Two processes,
 * not one, because the admission scheduler is process-global -- and because
 * the doctrine is "two processes, never one" for anything that decides whether
 * peers agree. The toy engine folds every seat's row into an accumulator, so
 * a replay that used a wrong row changes every later digest. Each child
 * records the digest it ended up with for every tick; the parent compares the
 * two timelines independently of the hash chain the driver itself uses.
 *
 * Scenarios cover both replay shapes and a MIXED pair (one peer inline, one
 * incremental), which is the claim that the shapes are behaviourally
 * identical on the wire. The link simulator puts real latency under the
 * incremental cases so tip-hold is reachable.
 *
 * This is the FSM's unit test, not the verdict: the verdict is an engine's
 * two-process harness over its real game (snesrecomp tools/rb_sweep.sh).
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* clock_gettime, nanosleep, setenv under -std=c11 */
#endif

#include "recomp_net/rb_driver.h"
#include "recomp_net/recomp_net.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
int main(void)
{
    printf("rb_driver_test: two-process part needs fork(); in-process part only on POSIX\n");
    return 0;
}
#else

#include <errno.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_failures;

static void expect_true(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_failures++;
    }
}

static uint32_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* ── toy engine ──────────────────────────────────────────────────────── */

#define TOY_SNAPS 128u
#define TOY_TICKS 4096u

typedef struct Toy {
    uint32_t tick;   /* next tick to run */
    uint32_t acc;
} Toy;

typedef struct Child {
    int slot;
    int mode;
    Toy state;
    struct { uint32_t tick; int valid; Toy s; } snaps[TOY_SNAPS];
    RNetRbFrame pub[RNET_RB_MAX_SLOTS];
    int pub_slots;
    uint32_t timeline[TOY_TICKS];   /* digest after tick t ran, last writer wins */
    FILE *log;
    /* ledger, counted from the driver's own lines */
    uint32_t n_ep_init, n_ep_follow, n_refused, n_abort, n_fork, n_timeout,
             n_tiphold_extend, n_unapplied, n_lobby, n_forced;
    uint32_t replay_admits;
} Child;

static Child g_c;

static uint32_t fold(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 16777619u;
    return h;
}

static uint16_t toy_pad(int slot, uint32_t tick)
{
    /* Changes every few ticks and differs by seat, so hold-last predictions
     * are regularly wrong under latency: organic mispredicts, not only the
     * injected ones. */
    uint16_t b = (uint16_t)((slot + 1) << 8);
    if (((tick / 5u) + (uint32_t)slot * 3u) % 7u == 0u) b |= 0x40u;
    if ((tick / 11u) & 1u) b |= 0x80u;
    return (uint16_t)(b & 0x0FFFu);
}

static uint32_t toy_digest(const Toy *t)
{
    return fold(fold(2166136261u, t->acc), t->tick);
}

static void toy_step(void)
{
    int i;
    uint32_t tick = g_c.state.tick;
    uint32_t acc = fold(g_c.state.acc, tick);
    for (i = 0; i < g_c.pub_slots; ++i)
        acc = fold(acc, (uint32_t)g_c.pub[i].buttons | ((uint32_t)i << 16));
    g_c.state.acc = acc;
    g_c.state.tick = tick + 1u;
    if (tick < TOY_TICKS)
        g_c.timeline[tick] = toy_digest(&g_c.state);
}

static int h_snap_save(void *ctx, uint32_t tick)
{
    unsigned i = tick % TOY_SNAPS;
    (void)ctx;
    g_c.snaps[i].tick = tick;
    g_c.snaps[i].valid = 1;
    g_c.snaps[i].s = g_c.state;
    return 1;
}

static int h_snap_load(void *ctx, uint32_t tick)
{
    unsigned i = tick % TOY_SNAPS;
    (void)ctx;
    if (!g_c.snaps[i].valid || g_c.snaps[i].tick != tick)
        return 0;
    g_c.state = g_c.snaps[i].s;
    return 1;
}

static int h_snap_has(void *ctx, uint32_t tick)
{
    unsigned i = tick % TOY_SNAPS;
    (void)ctx;
    return g_c.snaps[i].valid && g_c.snaps[i].tick == tick;
}

static int h_snap_oldest(void *ctx, uint32_t *oldest)
{
    unsigned i;
    int any = 0;
    uint32_t best = 0;
    (void)ctx;
    for (i = 0; i < TOY_SNAPS; ++i) {
        if (g_c.snaps[i].valid && (!any || g_c.snaps[i].tick < best)) {
            best = g_c.snaps[i].tick;
            any = 1;
        }
    }
    if (any)
        *oldest = best;
    return any;
}

static void h_snap_drop_after(void *ctx, uint32_t tick)
{
    unsigned i;
    (void)ctx;
    for (i = 0; i < TOY_SNAPS; ++i)
        if (g_c.snaps[i].valid && g_c.snaps[i].tick > tick)
            g_c.snaps[i].valid = 0;
}

static void h_publish(void *ctx, uint32_t tick, const RNetRbFrame *rows, int slots, int replay)
{
    (void)ctx;
    (void)replay;
    if (g_c.state.tick != tick) {
        fprintf(g_c.log, "TEST: publish for tick %u while the engine is at %u\n",
                (unsigned)tick, (unsigned)g_c.state.tick);
        g_c.n_fork++;   /* a skew here is a desync by construction */
    }
    memcpy(g_c.pub, rows, sizeof(RNetRbFrame) * (size_t)slots);
    g_c.pub_slots = slots;
}

static int h_run_tick(void *ctx, uint32_t tick)
{
    (void)ctx;
    (void)tick;
    toy_step();
    return 1;
}

static void h_resim(void *ctx) { (void)ctx; }

static uint32_t h_digest_master(void *ctx)
{
    (void)ctx;
    return toy_digest(&g_c.state);
}

static void h_digest_parts(void *ctx, RNetRbDigestParts *out)
{
    (void)ctx;
    out->master = toy_digest(&g_c.state);
    out->part[0] = g_c.state.acc & 0xffffu;
    out->part[1] = g_c.state.acc >> 16;
    out->part[2] = g_c.state.tick;
}

static void h_decode(void *ctx, int slot, const RNetInputSample *in, RNetRbFrame *out)
{
    (void)ctx;
    (void)slot;
    out->buttons = (uint16_t)((in->bytes[0] | ((uint16_t)in->bytes[1] << 8)) & 0x0FFFu);
}

static void h_sanitize(void *ctx, int slot, RNetRbFrame *f)
{
    (void)ctx;
    (void)slot;
    if (f->buttons == 0xFFFFu)
        f->buttons = 0;
    f->buttons &= 0x0FFFu;
}

static void h_neutral(void *ctx, int slot, RNetRbFrame *out)
{
    (void)ctx;
    (void)slot;
    out->buttons = 0;
}

static void h_lobby(void *ctx)
{
    (void)ctx;
    g_c.n_lobby++;
}

static uint32_t h_now(void *ctx)
{
    (void)ctx;
    return mono_ms();
}

static void h_log(void *ctx, const char *line)
{
    (void)ctx;
    if (strstr(line, "RESIM episode") && strstr(line, "initiator")) g_c.n_ep_init++;
    if (strstr(line, "RESIM episode") && strstr(line, "follower")) g_c.n_ep_follow++;
    if (strstr(line, "RB follow refused")) g_c.n_refused++;
    if (strstr(line, "RB abort")) g_c.n_abort++;
    if (strstr(line, "FORK") && !strstr(line, "FORCE FORK")) g_c.n_fork++;
    if (strstr(line, "timed out waiting")) g_c.n_timeout++;
    if (strstr(line, "RB tip-extend epoch")) g_c.n_tiphold_extend++;
    if (strstr(line, "UNAPPLIED")) g_c.n_unapplied++;
    if (strstr(line, "forced late row")) g_c.n_forced++;
    if (g_c.log)
        fputs(line, g_c.log);
}

static void sample_local(rnet_u32 tick, RNetInputSample *out, void *opaque)
{
    uint16_t b = toy_pad(g_c.slot, tick);
    (void)opaque;
    memset(out, 0, sizeof(*out));
    out->size = 2;
    out->bytes[0] = (rnet_u8)(b & 0xffu);
    out->bytes[1] = (rnet_u8)(b >> 8);
    out->valid = 1;
}

static void publish_unused(rnet_u32 tick, const RNetInputSample *by_slot, int slots, void *opaque)
{
    (void)tick;
    (void)by_slot;
    (void)slots;
    (void)opaque;
}

static void fill_host(RNetRbHost *h)
{
    memset(h, 0, sizeof(*h));
    h->snap_save = h_snap_save;
    h->snap_load = h_snap_load;
    h->snap_has = h_snap_has;
    h->snap_oldest = h_snap_oldest;
    h->snap_drop_after = h_snap_drop_after;
    h->publish = h_publish;
    h->run_tick = h_run_tick;
    h->resim_begin = h_resim;
    h->resim_end = h_resim;
    h->digest_master = h_digest_master;
    h->digest_parts = h_digest_parts;
    h->decode_sample = h_decode;
    h->sanitize_row = h_sanitize;
    h->neutral_row = h_neutral;
    h->request_return_to_lobby = h_lobby;
    h->log = h_log;
    h->now_ms = h_now;
}

/* ── in-process ──────────────────────────────────────────────────────── */

static char g_last_line[512];
static void capture_log(void *ctx, const char *line)
{
    (void)ctx;
    snprintf(g_last_line, sizeof(g_last_line), "%s", line);
}

static void in_process_tests(void)
{
    uint32_t seat;
    RNetRbDriver *d;
    RNetRbDriverConfig cfg;
    RNetRbHost h;
    RNetSession *null_session = NULL;

    for (seat = 0; seat < RNET_RB_MAX_SLOTS; ++seat) {
        uint32_t e = rnet_rb_epoch_make(41u, seat);
        expect_true(rnet_rb_epoch_initiator(e) == seat, "epoch carries its initiator seat");
        expect_true(e != 0u, "an epoch is never 0");
    }
    expect_true(rnet_rb_epoch_make(1u, 1u) != rnet_rb_epoch_make(1u, 2u),
                "two seats opening at once never share an epoch");

    d = rnet_rb_driver_create();
    expect_true(d != NULL, "driver create");

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = &null_session;
    fill_host(&h);
    h.log = capture_log;
    h.snap_has = NULL;
    expect_true(!rnet_rb_driver_start(d, &cfg, &h), "start refuses a host with a hole");
    expect_true(strstr(g_last_line, "snap_has") != NULL, "...and names the missing callback");

    fill_host(&h);
    h.log = capture_log;
    h.run_tick = NULL;
    cfg.replay_mode = RNET_RB_REPLAY_INLINE;
    expect_true(!rnet_rb_driver_start(d, &cfg, &h), "INLINE replay requires run_tick");
    expect_true(strstr(g_last_line, "run_tick") != NULL, "...and says so");

    fill_host(&h);
    h.log = capture_log;
    cfg.session = NULL;
    expect_true(!rnet_rb_driver_start(d, &cfg, &h), "start refuses no session binding");
    expect_true(strstr(g_last_line, "no session binding") != NULL, "...and says so");

    expect_true(rnet_rb_driver_poll_admit(d) == RNET_RB_ADMIT_STALL,
                "a driver that never started admits nothing");
    rnet_rb_driver_finish_frame(d);   /* must be a no-op, not a crash */
    expect_true(rnet_rb_driver_sim_tick(d) == 0u, "finish_frame without a start is inert");

    /* Identity and mod set survive the cold reset in start(). */
    rnet_rb_driver_set_identity(d, 0x1234u, 0x5678u);
    rnet_rb_driver_destroy(d);
}

/* ── two processes ───────────────────────────────────────────────────── */

typedef struct Report {
    int running;
    uint32_t sim, confirmed;
    uint32_t n_ep_init, n_ep_follow, n_refused, n_abort, n_fork, n_timeout,
             n_extend, n_unapplied, n_lobby, n_forced, replay_admits;
    uint64_t resim_ticks;
    uint32_t episodes_driver;
    uint32_t timeline[TOY_TICKS];
} Report;

typedef struct Scenario {
    const char *name;
    int mode_a, mode_b;
    const char *latency_ms;   /* per peer, one way; "" = none */
    const char *jitter_ms;
    int force_mispredict;     /* initiator only; 0 = off */
    uint32_t ticks;
} Scenario;

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k <= 0) {
            if (k < 0 && errno == EINTR) continue;
            return 0;
        }
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

static int read_all(int fd, void *buf, size_t n)
{
    char *p = (char *)buf;
    while (n) {
        ssize_t k = read(fd, p, n);
        if (k <= 0) {
            if (k < 0 && errno == EINTR) continue;
            return 0;
        }
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

static void run_child(const Scenario *sc, int slot, unsigned port_self, unsigned port_peer,
                      uint32_t session_id, int fd)
{
    RNetConfig rc;
    RNetHostVTable hv;
    RNetSession *s = NULL;
    RNetRbDriver *d = NULL;
    RNetRbDriverConfig cfg;
    RNetRbHost host;
    int local = slot, slots = 2, delay = 3;
    char bind[64], peer[64], logname[128];
    uint32_t start_ms, next_tick_ms;
    Report *r = (Report *)calloc(1, sizeof(Report));

    memset(&g_c, 0, sizeof(g_c));
    g_c.slot = slot;
    g_c.mode = slot == 0 ? sc->mode_a : sc->mode_b;
    snprintf(logname, sizeof(logname), "rb_driver_test_%s_%s.log", sc->name,
             slot == 0 ? "initiator" : "follower");
    g_c.log = fopen(logname, "w");

    setenv("RNET_SIM_LATENCY_MS", sc->latency_ms, 1);
    setenv("RNET_SIM_JITTER_MS", sc->jitter_ms, 1);
    setenv("RNET_SIM_SEED", slot == 0 ? "11" : "23", 1);
    /* Pinned per peer, not inherited: a knob exported for one side must never
     * reach the other (the SNES harness learned that twice). */
    if (slot == 0 && sc->force_mispredict > 0) {
        char v[16];
        snprintf(v, sizeof(v), "%d", sc->force_mispredict);
        setenv("RNET_RB_FORCE_MISPREDICT", v, 1);
    } else {
        setenv("RNET_RB_FORCE_MISPREDICT", "0", 1);
    }

    rnet_config_init_defaults(&rc);
    rc.slot_count = 2;
    rc.local_slot = (rnet_u8)slot;
    rc.input_delay = (rnet_u8)delay;
    rc.session_id = session_id;
    memset(&hv, 0, sizeof(hv));
    hv.sample_local = sample_local;
    hv.publish = publish_unused;
    s = rnet_session_create(&rc, &hv);
    snprintf(bind, sizeof(bind), "127.0.0.1:%u", port_self);
    snprintf(peer, sizeof(peer), "127.0.0.1:%u", port_peer);
    if (!s || rnet_session_start_lan(s, bind, peer) != 0)
        goto report;

    start_ms = mono_ms();
    while (!rnet_session_is_running(s) && (uint32_t)(mono_ms() - start_ms) < 5000u) {
        rnet_session_pump(s);
        sleep_ms(1);
    }
    if (!rnet_session_is_running(s))
        goto report;
    r->running = 1;

    d = rnet_rb_driver_create();
    memset(&cfg, 0, sizeof(cfg));
    cfg.session = &s;
    cfg.local_slot = &local;
    cfg.slot_count = &slots;
    cfg.input_delay = &delay;
    cfg.replay_mode = g_c.mode ? RNET_RB_REPLAY_INCREMENTAL : RNET_RB_REPLAY_INLINE;
    cfg.part_names[0] = "acc_lo";
    cfg.part_names[1] = "acc_hi";
    cfg.part_names[2] = "tick";
    cfg.snap_depth = TOY_SNAPS;
    cfg.log_prefix = "rb_test";
    fill_host(&host);
    if (!rnet_rb_driver_start(d, &cfg, &host))
        goto report;

    /* Paced like a frontend: a live tick every 4 ms; a replayed tick as soon
     * as the host gets it; a stall retries after pumping. */
    start_ms = mono_ms();
    next_tick_ms = start_ms;
    while (rnet_rb_driver_sim_tick(d) < sc->ticks &&
           (uint32_t)(mono_ms() - start_ms) < 20000u) {
        RNetRbAdmit a;
        rnet_session_pump(s);
        if (g_c.n_lobby)
            break;
        if ((int32_t)(mono_ms() - next_tick_ms) < 0 &&
            !rnet_rb_driver_in_resim(d)) {
            sleep_ms(1);
            continue;
        }
        a = rnet_rb_driver_poll_admit(d);
        if (a == RNET_RB_ADMIT_STALL) {
            sleep_ms(1);
            continue;
        }
        if (a == RNET_RB_ADMIT_REPLAY)
            g_c.replay_admits++;
        toy_step();   /* the same per-tick function for live and replay */
        rnet_rb_driver_finish_frame(d);
        if (a == RNET_RB_ADMIT_LIVE)
            next_tick_ms += 4u;
    }
    /* Keep answering for a moment so the peer's last episode can finish. */
    start_ms = mono_ms();
    while ((uint32_t)(mono_ms() - start_ms) < 400u) {
        RNetRbAdmit a;
        rnet_session_pump(s);
        a = rnet_rb_driver_poll_admit(d);
        if (a != RNET_RB_ADMIT_STALL) {
            toy_step();
            rnet_rb_driver_finish_frame(d);
        }
        if (a != RNET_RB_ADMIT_REPLAY)
            sleep_ms(4);
    }

report:
    if (d) {
        r->sim = rnet_rb_driver_sim_tick(d);
        r->confirmed = rnet_rb_driver_confirmed_through(d);
        r->resim_ticks = rnet_rb_driver_resim_ticks(d);
        r->episodes_driver = rnet_rb_driver_episode_count(d);
    }
    r->n_ep_init = g_c.n_ep_init;
    r->n_ep_follow = g_c.n_ep_follow;
    r->n_refused = g_c.n_refused;
    r->n_abort = g_c.n_abort;
    r->n_fork = g_c.n_fork;
    r->n_timeout = g_c.n_timeout;
    r->n_extend = g_c.n_tiphold_extend;
    r->n_unapplied = g_c.n_unapplied;
    r->n_lobby = g_c.n_lobby;
    r->n_forced = g_c.n_forced;
    r->replay_admits = g_c.replay_admits;
    memcpy(r->timeline, g_c.timeline, sizeof(r->timeline));
    (void)write_all(fd, r, sizeof(*r));
    if (s)
        rnet_session_send_bye(s);
    rnet_rb_driver_destroy(d);
    if (s)
        rnet_session_destroy(s);
    if (g_c.log)
        fclose(g_c.log);
    free(r);
    _exit(0);
}

static void run_scenario(const Scenario *sc, unsigned port_base)
{
    int pa[2], pb[2];
    pid_t a, b;
    Report *ra = (Report *)calloc(1, sizeof(Report));
    Report *rb = (Report *)calloc(1, sizeof(Report));
    uint32_t session_id = 0x52424456u ^ (uint32_t)getpid() ^ port_base;
    uint32_t upto, t, first_bad = 0;
    int got_a, got_b;
    int64_t ledger;
    char msg[256];

    if (pipe(pa) != 0 || pipe(pb) != 0) {
        expect_true(0, "pipe");
        return;
    }
    fflush(stdout);
    fflush(stderr);
    a = fork();
    if (a == 0) {
        close(pa[0]);
        run_child(sc, 0, port_base, port_base + 1u, session_id, pa[1]);
    }
    b = fork();
    if (b == 0) {
        close(pb[0]);
        run_child(sc, 1, port_base + 1u, port_base, session_id, pb[1]);
    }
    close(pa[1]);
    close(pb[1]);
    got_a = read_all(pa[0], ra, sizeof(*ra));
    got_b = read_all(pb[0], rb, sizeof(*rb));
    waitpid(a, NULL, 0);
    waitpid(b, NULL, 0);
    close(pa[0]);
    close(pb[0]);

    printf("%-22s init=%u+%u follow=%u+%u refused=%u abort=%u+%u timeout=%u fork=%u "
           "extend=%u unapplied=%u forced=%u replay_admits=%u+%u resim=%llu+%llu "
           "sim=%u/%u confirmed=%u/%u\n",
           sc->name, ra->n_ep_init, rb->n_ep_init, ra->n_ep_follow, rb->n_ep_follow,
           ra->n_refused + rb->n_refused, ra->n_abort, rb->n_abort,
           ra->n_timeout + rb->n_timeout, ra->n_fork + rb->n_fork,
           ra->n_extend + rb->n_extend, ra->n_unapplied + rb->n_unapplied,
           ra->n_forced, ra->replay_admits, rb->replay_admits,
           (unsigned long long)ra->resim_ticks, (unsigned long long)rb->resim_ticks,
           ra->sim, rb->sim, ra->confirmed, rb->confirmed);

    snprintf(msg, sizeof(msg), "%s: both children reported", sc->name);
    expect_true(got_a && got_b, msg);
    snprintf(msg, sizeof(msg), "%s: both sessions reached RUNNING", sc->name);
    expect_true(ra->running && rb->running, msg);
    snprintf(msg, sizeof(msg), "%s: both peers ran the match", sc->name);
    expect_true(ra->sim >= sc->ticks && rb->sim >= sc->ticks / 2u, msg);
    snprintf(msg, sizeof(msg), "%s: episodes ran (nothing exercised otherwise)", sc->name);
    expect_true(ra->n_ep_init + rb->n_ep_init > 0u &&
                    ra->n_ep_follow + rb->n_ep_follow > 0u,
                msg);
    snprintf(msg, sizeof(msg), "%s: 0 forks", sc->name);
    expect_true(ra->n_fork + rb->n_fork == 0u, msg);
    snprintf(msg, sizeof(msg), "%s: no match refused to start", sc->name);
    expect_true(ra->n_lobby + rb->n_lobby == 0u, msg);
    /* The exact ledger, by role and in both directions: every episode one
     * side opens is followed or refused by the other; a lost BEGIN (loss or
     * reordering) is explained only by a watchdog timeout. */
    ledger = (int64_t)(ra->n_ep_init + rb->n_ep_init) -
             (int64_t)(ra->n_ep_follow + rb->n_ep_follow) -
             (int64_t)(ra->n_refused + rb->n_refused);
    snprintf(msg, sizeof(msg), "%s: episode ledger balances (residual %lld, timeouts %u)",
             sc->name, (long long)ledger, ra->n_timeout + rb->n_timeout);
    expect_true(ledger >= 0 && ledger <= (int64_t)(ra->n_timeout + rb->n_timeout), msg);
    if (sc->mode_a || sc->mode_b) {
        snprintf(msg, sizeof(msg), "%s: the incremental peer replayed tick by tick", sc->name);
        expect_true((sc->mode_a ? ra->replay_admits : 0u) +
                        (sc->mode_b ? rb->replay_admits : 0u) > 0u,
                    msg);
    }
    if (!sc->mode_a) {
        snprintf(msg, sizeof(msg), "%s: an inline peer never hands back a replay tick",
                 sc->name);
        expect_true(ra->replay_admits == 0u, msg);
    }

    /* The two final timelines agree through what both sides confirmed. This
     * is the engine's own state, not the driver's hash chain. */
    upto = ra->confirmed < rb->confirmed ? ra->confirmed : rb->confirmed;
    if (upto >= TOY_TICKS) upto = TOY_TICKS - 1u;
    snprintf(msg, sizeof(msg), "%s: the confirmed frontier advanced (%u)", sc->name, upto);
    expect_true(upto >= sc->ticks / 3u, msg);
    for (t = 1; t <= upto; ++t) {
        if (ra->timeline[t] != rb->timeline[t]) {
            first_bad = t;
            break;
        }
    }
    snprintf(msg, sizeof(msg), "%s: final timelines agree through tick %u (first diff %u)",
             sc->name, upto, first_bad);
    expect_true(first_bad == 0u, msg);
    free(ra);
    free(rb);
}

int main(void)
{
    static const Scenario scenarios[] = {
        /* name                mode A, B   latency jitter force  ticks */
        { "inline-loopback",   0, 0,       "0",   "0",   11,    700u },
        { "mixed-60ms",        0, 1,       "30",  "6",   13,    700u },
        { "incremental-100ms", 1, 1,       "50",  "10",  17,    700u },
    };
    unsigned port_base = 30000u + ((unsigned)getpid() % 5000u) * 4u;
    size_t i;

    in_process_tests();
    for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i)
        run_scenario(&scenarios[i], port_base + (unsigned)i * 2u * 997u % 20000u);

    if (g_failures == 0) {
        printf("rb_driver_test: ok\n");
        return 0;
    }
    fprintf(stderr, "rb_driver_test: %d failure(s)\n", g_failures);
    return 1;
}

#endif /* !_WIN32 */
