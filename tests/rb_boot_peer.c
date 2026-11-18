/* A real session/driver per process, including on Windows. The Python UDP
 * proxy owns packet loss; no production fault-injection switch is required. */
#include "recomp_net/rb_driver.h"
#include "platform/rnet_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNAPS 256u
typedef struct State { uint32_t tick, hash; } State;
typedef struct Peer {
    State state, snap[SNAPS];
    uint8_t valid[SNAPS];
    RNetRbFrame rows[RNET_RB_MAX_SLOTS];
    unsigned slots, agreed, unverified, refused, failed;
} Peer;

static uint32_t fold(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static int snap_save(void *ctx, uint32_t tick) {
    Peer *p = ctx; unsigned i = tick % SNAPS;
    p->snap[i] = p->state; p->valid[i] = 1; return 1;
}
static int snap_has(void *ctx, uint32_t tick) {
    Peer *p = ctx; unsigned i = tick % SNAPS;
    return p->valid[i] && p->snap[i].tick == tick;
}
static int snap_load(void *ctx, uint32_t tick) {
    Peer *p = ctx;
    if (!snap_has(ctx, tick)) return 0;
    p->state = p->snap[tick % SNAPS]; return 1;
}
static int snap_oldest(void *ctx, uint32_t *out) {
    Peer *p = ctx; unsigned i; int any = 0;
    for (i = 0; i < SNAPS; ++i)
        if (p->valid[i] && (!any || p->snap[i].tick < *out)) {
            *out = p->snap[i].tick; any = 1;
        }
    return any;
}
static void snap_drop(void *ctx, uint32_t tick) {
    Peer *p = ctx; unsigned i;
    for (i = 0; i < SNAPS; ++i)
        if (p->valid[i] && p->snap[i].tick > tick) p->valid[i] = 0;
}
static void publish(void *ctx, uint32_t tick, const RNetRbFrame *rows, int slots, int replay) {
    Peer *p = ctx; (void)replay;
    if (tick != p->state.tick) ++p->failed;
    memcpy(p->rows, rows, sizeof(*rows) * (size_t)slots);
}
static int step(void *ctx, uint32_t tick) {
    Peer *p = ctx; unsigned i;
    if (tick != p->state.tick) ++p->failed;
    p->state.hash = fold(p->state.hash, tick);
    for (i = 0; i < p->slots; ++i) p->state.hash = fold(p->state.hash, p->rows[i].buttons + i);
    ++p->state.tick; return 1;
}
static uint32_t digest(void *ctx) {
    Peer *p = ctx; return fold(p->state.hash, p->state.tick);
}
static void parts(void *ctx, RNetRbDigestParts *out) {
    Peer *p = ctx; out->master = digest(ctx);
    out->part[0] = p->state.hash; out->part[1] = p->state.tick; out->part[2] = 0;
}
static void decode(void *ctx, int slot, const RNetInputSample *in, RNetRbFrame *out) {
    (void)ctx; (void)slot; out->buttons = in->bytes[0];
}
static void neutral(void *ctx, int slot, RNetRbFrame *out) {
    (void)ctx; (void)slot; out->buttons = 0;
}
static void sample(uint32_t tick, RNetInputSample *out, void *ctx) {
    (void)tick; (void)ctx; memset(out, 0, sizeof(*out)); out->size = 1; out->valid = 1;
}
static void unused_publish(uint32_t tick, const RNetInputSample *rows, int slots, void *ctx) {
    (void)tick; (void)rows; (void)slots; (void)ctx;
}
static void resim(void *ctx) { (void)ctx; }
static void refused(void *ctx) { ++((Peer *)ctx)->refused; }
static uint32_t now(void *ctx) { (void)ctx; return (uint32_t)rnet_os_monotonic_ms(); }
static void log_line(void *ctx, const char *line) {
    Peer *p = ctx;
    if (strstr(line, "RB boot digest agreed")) ++p->agreed;
    if (strstr(line, "UNVERIFIED")) ++p->unverified;
    fputs(line, stderr);
}

int main(int argc, char **argv) {
    Peer p; RNetConfig net; RNetHostVTable io; RNetRbDriverConfig config; RNetRbHost host;
    RNetSession *session; RNetRbDriver *driver;
    int slot, slots, delay = 3, started = 0, ready = 0, draining = 0, late_sent = 0;
    unsigned mismatch, late, target = 220, live_after_refusal = 0;
    uint32_t start, refused_at = 0, sim_at_refusal = 0;
    char ready_path[2048], stop_path[2048], reason[64] = "";
    const char *expected;
    if (argc != 10) return 2;
    slot = atoi(argv[1]); slots = atoi(argv[2]); expected = argv[6];
    mismatch = (unsigned)atoi(argv[7]); late = (unsigned)atoi(argv[8]);
    if (slot < 0 || slot >= slots || slots < 2 || slots > RNET_RB_MAX_SLOTS) return 2;
    if (snprintf(ready_path, sizeof(ready_path), "%s/%d.ready", argv[9], slot) >= (int)sizeof(ready_path) ||
        snprintf(stop_path, sizeof(stop_path), "%s/stop", argv[9]) >= (int)sizeof(stop_path)) return 2;
    memset(&p, 0, sizeof(p)); p.slots = (unsigned)slots;
    p.state.hash = 0x12345678u ^ (mismatch && slot == 0 ? 1u : 0u);
    rnet_config_init_defaults(&net); net.local_slot = (rnet_u8)slot;
    net.slot_count = (rnet_u8)slots; net.input_delay = (rnet_u8)delay;
    net.session_id = (rnet_u32)strtoul(argv[5], NULL, 10);
    memset(&io, 0, sizeof(io)); io.sample_local = sample; io.publish = unused_publish;
    session = rnet_session_create(&net, &io);
    if (!session) return 3;
    if ((slots > 2 && slot == 0 ? rnet_session_start_lan_hub(session, argv[3]) :
         rnet_session_start_lan(session, argv[3], argv[4])) != 0) return 3;
    driver = rnet_rb_driver_create();
    if (!driver) return 3;
    memset(&config, 0, sizeof(config)); config.session = &session;
    config.local_slot = &slot; config.slot_count = &slots; config.input_delay = &delay;
    config.snap_depth = SNAPS; config.log_prefix = "boot_test";
    memset(&host, 0, sizeof(host)); host.ctx = &p;
    host.snap_save = snap_save; host.snap_load = snap_load; host.snap_has = snap_has;
    host.snap_oldest = snap_oldest; host.snap_drop_after = snap_drop;
    host.publish = publish; host.run_tick = step; host.resim_begin = resim; host.resim_end = resim;
    host.digest_master = digest; host.digest_parts = parts;
    host.decode_sample = decode; host.neutral_row = neutral;
    host.request_return_to_lobby = refused; host.log = log_line; host.now_ms = now;
    start = now(NULL);
    while ((uint32_t)(now(NULL) - start) < 15000u) {
        RNetRbAdmit admit; FILE *marker;
        rnet_session_pump(session);
        if (!started) {
            if (!rnet_session_is_running(session)) { rnet_os_sleep_micros(1000); continue; }
            if (!rnet_rb_driver_start(driver, &config, &host)) break;
            started = 1;
        }
        if (late && slot == 1 && !late_sent && p.state.tick >= 180) {
            rnet_session_send_rb_sync(session, 0, 0, 0, 0, 0, RNET_RB_SYNC_OP_FRAME_COMMIT_REQUEST, 0);
            late_sent = 1;
        }
        if (!ready && p.state.tick >= target && !strcmp(expected, "-")) {
            marker = fopen(ready_path, "wb"); if (!marker) { ++p.failed; break; }
            fclose(marker); ready = 1;
        }
        if (ready && !draining && (marker = fopen(stop_path, "rb")) != NULL) {
            fclose(marker); rnet_rb_driver_request_quiesce(driver); draining = 1;
        }
        admit = rnet_rb_driver_poll_admit(driver);
        if (rnet_rb_driver_refusal(driver)) {
            if (!refused_at) {
                refused_at = now(NULL); sim_at_refusal = rnet_rb_driver_sim_tick(driver);
                snprintf(reason, sizeof(reason), "%s", rnet_rb_driver_refusal(driver));
            }
            if (admit == RNET_RB_ADMIT_LIVE) ++live_after_refusal;
            if ((uint32_t)(now(NULL) - refused_at) >= 200) break;
        }
        if (admit != RNET_RB_ADMIT_STALL) {
            step(&p, p.state.tick); rnet_rb_driver_finish_frame(driver);
        }
        if (p.unverified || rnet_rb_driver_quiesce_state(driver) == RNET_RB_QUIESCE_DRAINED ||
            rnet_rb_driver_quiesce_state(driver) == RNET_RB_QUIESCE_TIMED_OUT) break;
        rnet_os_sleep_micros(1000);
    }
    if (slots > 2 && slot == 0) {
        uint32_t until = now(NULL);
        while ((uint32_t)(now(NULL) - until) < 300) { rnet_session_pump(session); rnet_os_sleep_micros(1000); }
    }
    printf("{\"slot\":%d,\"sim\":%u,\"confirmed\":%u,\"agreed\":%u,\"unverified\":%u,"
           "\"refused\":%u,\"reason\":\"%s\",\"sim_at_refusal\":%u,\"live_after_refusal\":%u,"
           "\"desync\":%u,\"failed\":%u,\"late_sent\":%d}\n", slot, p.state.tick,
           rnet_rb_driver_confirmed_through(driver), p.agreed, p.unverified, p.refused, reason,
           sim_at_refusal, live_after_refusal, rnet_rb_driver_desync_count(driver), p.failed, late_sent);
    rnet_rb_driver_destroy(driver); rnet_session_destroy(session);
    return started && !p.failed ? 0 : 1;
}
