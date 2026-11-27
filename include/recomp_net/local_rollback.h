#ifndef RECOMP_NET_LOCAL_ROLLBACK_H
#define RECOMP_NET_LOCAL_ROLLBACK_H
#include "recomp_net/session.h"
#include "recomp_net/startup_profiles.h"
#include "recomp_net/telemetry.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RNET_EXPERIMENT_LOBBY_VERSION "experimental/local-rollback-v4"
#define RNET_LOCAL_HISTORY 128u
typedef struct RNetLocalDriver RNetLocalDriver;
typedef struct RNetLocalConfig {
    /* NULL permits a host-owned input transport through host.input/sample. */
    RNetSession *session;
    rnet_u32 seats, local_seat, occupied_mask, row_size;
    rnet_u32 delay, rollback_depth, tick_us;
} RNetLocalConfig;
typedef struct RNetLocalHost {
    void *ctx;
    /* Snapshots are BEFORE tick T. Must retain rollback_depth+2 ticks. */
    int (*save)(void *, rnet_u32 tick);
    int (*load)(void *, rnet_u32 tick);
    void (*drop_after)(void *, rnet_u32 tick);
    void (*publish)(void *, rnet_u32 tick, const RNetInputSample *rows, int seats, int replay);
    /* NULL: incremental replay through poll/finish; otherwise replay inline. */
    int (*run_tick)(void *, rnet_u32 tick);
    void (*resim_begin)(void *);
    void (*resim_end)(void *);
    rnet_u32 (*digest)(void *);
    rnet_u64 (*now_us)(void *);
    /* Optional deterministic predictor. Default hold-last; relative aim uses zero delta. */
    void (*predict)(void *, int seat, rnet_u32 tick, const RNetInputSample *previous,
                    RNetInputSample *out);
    void (*neutral)(void *, int seat, RNetInputSample *out);
    /* Latched by finish for the tick just executed, including replay. NULL = all read. */
    rnet_u32 (*consumed_seats)(void *, rnet_u32 tick);
    /* Only for a host-owned transport. input must return immutable canonical rows. */
    int (*input)(void *, int seat, rnet_u32 tick, RNetInputSample *out);
    int (*sample)(void *, rnet_u32 tick, rnet_u32 delay);
    /* Host-owned transports: worst occupied peer's measured path. Return 0
     * until every peer has a measurement. Session-backed hosts may leave NULL. */
    int (*network)(void *, RNetPeerTelemetry *worst);
    /* Optional monotonic work clock, in microseconds. Exclude only deliberate
     * host pacing; retain snapshot/digest/restore, GPU/I/O waits and execution.
     * now_us remains the wall clock for stalls, latency and transport timers.
     * NULL uses now_us. Never rewind this clock with simulation state. */
    rnet_u64 (*work_us)(void *);
} RNetLocalHost;
typedef struct RNetLocalControlConfig {
    rnet_u32 frame_budget_us;
    /* Seed with built-host measurements including snapshot/digest overhead.
     * The driver updates both costs from executed live/replay ticks. */
    rnet_u32 live_tick_us, replay_tick_us;
    /* Fixed rewind/drop/resim-bracket cost, reserved once per correction.
     * A nonzero measured seed is required for coarse-clock hosts too. */
    rnet_u32 replay_setup_us;
    rnet_u32 one_way_us, jitter_us, network_valid;
    /* 0 uses 60 live ticks. Both directions require two stable intervals,
     * then move at most one delay tick per interval. */
    rnet_u32 interval_ticks;
} RNetLocalControlConfig;
typedef struct RNetLocalControlState {
    rnet_u32 delay, target_delay, rollback_depth;
    rnet_u32 live_tick_us, replay_tick_us, replay_setup_us;
    rnet_u32 one_way_us, jitter_us, network_valid;
} RNetLocalControlState;
/* Startup is immutable and uses the existing capability agreement. Both modes
 * must call this BEFORE transport start; qualification retains build/content identity.
 * A mode disagreement refuses startup instead of falling back. */
int rnet_experiment_profiles_begin(RNetSession *, rnet_u32 qualification,
                                   const RNetProfileCaps *, RNetSyncMode mode);
int rnet_experiment_profiles_begin_full(RNetSession *,rnet_u32 qualification,
    const RNetProfileCaps *,RNetSyncMode mode,const rnet_u8 identity[32]);
RNetLocalDriver *rnet_local_create(const RNetLocalConfig *, const RNetLocalHost *);
void rnet_local_destroy(RNetLocalDriver *);
/* 0 stalled, 1 live tick, 2 replay tick. Host completes exactly one admitted
 * tick then calls finish. No handshake is sent for a correction. */
int rnet_local_poll(RNetLocalDriver *);
int rnet_local_finish(RNetLocalDriver *);
rnet_u32 rnet_local_tick(const RNetLocalDriver *);
/* Number of leading ticks with all inputs real and corrected (exclusive). */
rnet_u32 rnet_local_confirmed(const RNetLocalDriver *);
/* Exclusive verified prefix; network verification never gates a live tick. */
rnet_u32 rnet_local_verified(const RNetLocalDriver *);
/* Stop before T while continuing reconciliation and hash exchange. */
int rnet_local_set_hold(RNetLocalDriver *, rnet_u32 tick);
void rnet_local_clear_hold(RNetLocalDriver *);
const char *rnet_local_refusal(const RNetLocalDriver *);
const RNetTelemetry *rnet_local_telemetry(const RNetLocalDriver *);
/* End-of-match report includes corrections after the last completed live tick. */
size_t rnet_local_report(const RNetLocalDriver *, char *out, size_t capacity);
/* Pure pre-match preview. tick_us is simulation cadence, distinct from the
 * host's available frame budget. No CPU feature implies a qualified cost. */
int rnet_local_control_preview(const RNetLocalControlConfig *, rnet_u32 tick_us,
    rnet_u32 max_depth, RNetLocalControlState *);
/* Configure once before tick 0 / sampling; NULL is not an enable/disable switch.
 * Still reserve cfg.rollback_depth snapshots even when affordable depth is lower. */
int rnet_local_control_begin(RNetLocalDriver *, const RNetLocalControlConfig *);
int rnet_local_control_state(const RNetLocalDriver *, RNetLocalControlState *);
/* Static budget decision, using measured costs in microseconds. No TURN floor.
 * one_way is an ESTIMATE when obtained from half an RTT. */
rnet_u32 rnet_local_budget_delay(rnet_u32 one_way, rnet_u32 jitter,
    rnet_u32 replay_tick, rnet_u32 live_tick, rnet_u32 frame_budget,
    rnet_u32 max_depth, rnet_u32 *affordable_depth);
#ifdef __cplusplus
}
#endif
#endif
