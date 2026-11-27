#include "recomp_net/telemetry.h"
#include "recomp_net/config.h"
#include "rnet_telemetry_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct RNetTelemetry {
    RNetTickTelemetry ring[RNET_TELEMETRY_TICKS];
    RNetPeerTelemetry peers[RNET_MAX_SLOTS];
    rnet_u64 count, stalls, replays, corrections, latency, latency_n;
    rnet_u64 delay_total, episodes;
    rnet_u32 tick_us;
    RNetSyncMode mode;
};
RNetTelemetry *rnet_telemetry_create(RNetSyncMode mode, rnet_u32 tick_us) {
    RNetTelemetry *t = calloc(1, sizeof(*t));
    if (t) { t->mode = mode; t->tick_us = tick_us; }
    return t;
}
void rnet_telemetry_destroy(RNetTelemetry *t) { free(t); }
void rnet_telemetry_record(RNetTelemetry *t, const RNetTickTelemetry *row) {
    RNetTickTelemetry *dst;
    if (!t || !row) return;
    dst = &t->ring[t->count % RNET_TELEMETRY_TICKS];
    *dst = *row; dst->sequence = t->count++;
    t->stalls += row->stall_us; t->replays += row->replay_ticks;
    t->corrections += row->mispredicts; t->delay_total += row->delay;
    if (row->input_latency_valid) { t->latency += row->input_latency_us; ++t->latency_n; }
    /* The driver supplies its match-local episode serial. Several aborted
     * episodes may happen without a live tick between them. */
    if (row->episode_id > t->episodes) t->episodes = row->episode_id;
}
void rnet_telemetry_peer(RNetTelemetry *t, int seat, const RNetPeerTelemetry *p) {
    if (t && p && seat >= 0 && seat < RNET_MAX_SLOTS) t->peers[seat] = *p;
}
size_t rnet_telemetry_read(const RNetTelemetry *t, rnet_u64 first,
                           RNetTickTelemetry *out, size_t cap) {
    rnet_u64 oldest; size_t n = 0;
    if (!t || !out) return 0;
    oldest = t->count > RNET_TELEMETRY_TICKS ? t->count - RNET_TELEMETRY_TICKS : 0;
    if (first < oldest) first = oldest;
    while (first < t->count && n < cap) out[n++] = t->ring[first++ % RNET_TELEMETRY_TICKS];
    return n;
}
size_t rnet_telemetry_report_extra(const RNetTelemetry *t, const RNetTickTelemetry *pending,
                                  char *out, size_t cap) {
    char json[4096]; size_t used; int i, n;
    rnet_u64 stalls, replays, corrections, episodes;
    if (!t) return 0;
    stalls = t->stalls + (pending ? pending->stall_us : 0u);
    replays = t->replays + (pending ? pending->replay_ticks : 0u);
    corrections = t->corrections + (pending ? pending->mispredicts : 0u);
    episodes = t->episodes;
    if (pending && pending->episode_id > episodes) episodes = pending->episode_id;
    n = snprintf(json, sizeof(json),
        "{\"schema\":1,\"mode\":\"%s\",\"ticks\":%llu,\"retained_ticks\":%llu,"
        "\"dropped_ticks\":%llu,\"tick_us\":%u,\"latency_boundary\":\"sample-to-publish\","
        "\"effective_input_latency_ms\":%.3f,\"latency_samples\":%llu,"
        "\"mean_delay_frames\":%.3f,\"stall_ms\":%.3f,\"stall_ms_per_sim_minute\":%.3f,"
        "\"mispredicts\":%llu,\"episodes\":%llu,\"replay_ticks\":%llu,\"peers\":[",
        t->mode == RNET_SYNC_LOCAL_V4 ? "local-v4" : "episode-v3",
        (unsigned long long)t->count,
        (unsigned long long)(t->count < RNET_TELEMETRY_TICKS ? t->count : RNET_TELEMETRY_TICKS),
        (unsigned long long)(t->count > RNET_TELEMETRY_TICKS ? t->count - RNET_TELEMETRY_TICKS : 0),
        t->tick_us, t->latency_n ? (double)t->latency / t->latency_n / 1000.0 : 0.0,
        (unsigned long long)t->latency_n, t->count ? (double)t->delay_total / t->count : 0.0,
        (double)stalls / 1000.0,
        t->count && t->tick_us ? (double)stalls * 60000.0 / ((double)t->count * t->tick_us) : 0.0,
        (unsigned long long)corrections, (unsigned long long)episodes,
        (unsigned long long)replays);
    if (n < 0 || (size_t)n >= sizeof(json)) return 0;
    used = (size_t)n;
    for (i = 0; i < RNET_MAX_SLOTS; ++i) {
        const RNetPeerTelemetry *p = &t->peers[i];
        n = snprintf(json + used, sizeof(json) - used,
            "%s{\"seat\":%d,\"path\":%u,\"rtt_ms\":%.3f,\"jitter_ms\":%.3f,"
            "\"echo_sent\":%u,\"echo_received\":%u,\"echo_lost\":%u}",
            i ? "," : "", i, p->path, p->rtt_us / 1000.0, p->jitter_us / 1000.0,
            p->sent, p->received, p->lost);
        if (n < 0 || (size_t)n >= sizeof(json) - used) return 0;
        used += (size_t)n;
    }
    memcpy(json + used, "]}", 3); used += 3;
    if (out && cap >= used) memcpy(out, json, used);
    return used;
}
size_t rnet_telemetry_report(const RNetTelemetry *t, char *out, size_t cap) {
    return rnet_telemetry_report_extra(t, NULL, out, cap);
}
