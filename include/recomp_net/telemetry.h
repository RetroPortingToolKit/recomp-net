#ifndef RECOMP_NET_TELEMETRY_H
#define RECOMP_NET_TELEMETRY_H
/* Experimental query-only match history. Single simulation-thread owner. */
#include "recomp_net/types.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RNET_TELEMETRY_TICKS 36000u
typedef struct RNetTelemetry RNetTelemetry;
typedef enum RNetSyncMode { RNET_SYNC_EPISODE_V3 = 0, RNET_SYNC_LOCAL_V4 = 1 } RNetSyncMode;
typedef enum RNetTickStall {
    RNET_TICK_READY, RNET_TICK_STARTUP, RNET_TICK_INPUT,
    RNET_TICK_PREDICTION_LIMIT, RNET_TICK_EPISODE, RNET_TICK_REFUSED,
    RNET_TICK_VERIFICATION
} RNetTickStall;
typedef struct RNetTickTelemetry {
    rnet_u64 sequence, at_us, stall_us, snapshot_us, digest_us, replay_us;
    rnet_u64 input_latency_us;
    rnet_u32 tick, delay, prediction_depth, guessed_seats, mispredicts;
    rnet_u32 episode_id, episode_stage, episode_depth, replay_ticks;
    rnet_u32 stall_reason, input_latency_valid;
} RNetTickTelemetry;
typedef struct RNetPeerTelemetry {
    rnet_u32 rtt_us, jitter_us, sent, received, lost;
    /* 0 unknown, 1 direct/LAN, 2 host relay, 3 TURN. */
    rnet_u32 path;
} RNetPeerTelemetry;
RNetTelemetry *rnet_telemetry_create(RNetSyncMode mode, rnet_u32 tick_us);
void rnet_telemetry_destroy(RNetTelemetry *t);
/* One record per completed live tick; replay work is charged to the next live tick. */
void rnet_telemetry_record(RNetTelemetry *t, const RNetTickTelemetry *tick);
void rnet_telemetry_peer(RNetTelemetry *t, int seat, const RNetPeerTelemetry *peer);
/* Oldest-first, sequence >= first. A wrapped ring exposes its loss in JSON. */
size_t rnet_telemetry_read(const RNetTelemetry *t, rnet_u64 first,
                           RNetTickTelemetry *out, size_t capacity);
/* Returns required bytes INCLUDING NUL. Short buffers receive no partial JSON.
 * Latency is sample-to-input-publication, not display/USB latency. */
size_t rnet_telemetry_report(const RNetTelemetry *t, char *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
