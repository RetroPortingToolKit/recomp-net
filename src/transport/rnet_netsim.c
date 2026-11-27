#include "rnet_netsim.h"

#include "protocol/rnet_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A legal state transfer spans up to RNET_STATE_MAX_CHUNKS datagrams.
 * Rollback baseline/seal traffic also arrives while simulation is parked;
 * frame rate does not bound the burst (the 80 ms path exceeded 1024).
 * Bound storage by one full protocol-sized transfer plus the previous 1024
 * control slots. This is about 10 MiB per engaged simulator on 64-bit hosts;
 * the ordinary, simulator-off receive path still allocates nothing. */
#define RNET_NETSIM_SLOTS (RNET_STATE_MAX_CHUNKS + 1024u)
#define RNET_NETSIM_BANNER_MS 10000u
/* Latency past this is a typo, not a test (a 5 s hold would look like a hang
 * and time out every handshake in the stack). */
#define RNET_NETSIM_MAX_MS 2000u

typedef struct NetSimEntry
{
    rnet_u8 data[RNET_MAX_PACKET];
    size_t len;
    rnet_u64 due_ms;
    struct sockaddr_in src;
    int in_use;
} NetSimEntry;

struct RNetNetSim
{
    NetSimEntry slot[RNET_NETSIM_SLOTS];
    rnet_u32 latency_ms;
    rnet_u32 jitter_ms;
    rnet_u32 loss_pct;
    rnet_u32 rng;
    rnet_u64 next_banner_ms;
    rnet_u64 held_total;
    rnet_u64 overflow_total;
    rnet_u64 dropped_total;
    rnet_u32 live;
    rnet_u32 live_peak;
    rnet_u32 duplicate_pct, burst_pct, burst_packets, burst_remaining, kbps;
    rnet_u64 next_wire_us;
};

static rnet_u32 env_u32(const char *name, rnet_u32 fallback)
{
    const char *v = getenv(name);
    char *end = NULL;
    unsigned long n;

    if ((v == NULL) || (v[0] == '\0'))
    {
        return fallback;
    }
    n = strtoul(v, &end, 10);
    if ((end == v) || (n > 0xffffffffUL))
    {
        return fallback;
    }
    return (rnet_u32)n;
}

static rnet_u32 netsim_rand(RNetNetSim *sim)
{
    rnet_u32 x = sim->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    sim->rng = x;
    return x;
}

static void netsim_banner(RNetNetSim *sim, rnet_u64 now)
{
    fprintf(stderr,
            "rnet: *** LINK SIMULATOR ENGAGED *** +%ums recv delay, +/-%ums "
            "jitter, %u%% loss (this side only; peer adds its own). "
            "held=%llu dropped=%llu overflow=%llu queue=%u/%u\n",
            (unsigned)sim->latency_ms, (unsigned)sim->jitter_ms,
            (unsigned)sim->loss_pct,
            (unsigned long long)sim->held_total,
            (unsigned long long)sim->dropped_total,
            (unsigned long long)sim->overflow_total,
            (unsigned)sim->live_peak, (unsigned)RNET_NETSIM_SLOTS);
    fflush(stderr);
    sim->next_banner_ms = now + RNET_NETSIM_BANNER_MS;
}

static RNetNetSim *netsim_create(int send)
{
    RNetNetSim *sim;
    const char *profile = send ? NULL : getenv("RNET_SIM_PROFILE");
    rnet_u32 profile_latency = 0, profile_jitter = 0, profile_loss = 0;
    rnet_u32 latency, loss, dup, burst, kbps;
    rnet_u32 jitter;
    if (profile && *profile) {
        if (!strcmp(profile,"same-region")) { profile_latency=10; profile_jitter=2; }
        else if (!strcmp(profile,"cross-country")) { profile_latency=40; profile_jitter=8; }
        else if (!strcmp(profile,"transatlantic")) { profile_latency=80; profile_jitter=12; }
        else if (!strcmp(profile,"transpacific")) { profile_latency=125; profile_jitter=20; }
        else if (!strcmp(profile,"wifi")) { profile_latency=12; profile_jitter=12; profile_loss=2; }
        else { fprintf(stderr,"rnet: unknown RNET_SIM_PROFILE '%s'\n",profile); return NULL; }
    }
    latency = env_u32(send ? "RNET_SIM_SEND_LATENCY_MS" : "RNET_SIM_LATENCY_MS", profile_latency);
    loss = env_u32(send ? "RNET_SIM_SEND_LOSS_PCT" : "RNET_SIM_LOSS_PCT", profile_loss);
    dup = env_u32(send ? "RNET_SIM_SEND_DUPLICATE_PCT" : "RNET_SIM_DUPLICATE_PCT", 0u);
    burst = env_u32(send ? "RNET_SIM_SEND_BURST_PCT" : "RNET_SIM_BURST_PCT", 0u);
    kbps = env_u32(send ? "RNET_SIM_SEND_KBPS" : "RNET_SIM_KBPS", 0u);

    /* Loss is independent of latency: dropping on a fast link is a legitimate
     * and quite different test, so either knob alone engages the simulator. */
    if ((latency == 0u) && (loss == 0u) && !dup && !burst && !kbps)
    {
        return NULL;
    }
    if (loss > 90u)
    {
        fprintf(stderr,
                "rnet: RNET_SIM_LOSS_PCT=%u clamped to 90 — above that the "
                "handshakes never complete and the run measures nothing\n",
                (unsigned)loss);
        fflush(stderr);
        loss = 90u;
    }
    if (latency > RNET_NETSIM_MAX_MS)
    {
        fprintf(stderr,
                "rnet: RNET_SIM_LATENCY_MS=%u exceeds the %u ms ceiling — "
                "refusing to engage (a hold that long is indistinguishable "
                "from a hung link)\n",
                (unsigned)latency, (unsigned)RNET_NETSIM_MAX_MS);
        fflush(stderr);
        return NULL;
    }
    jitter = env_u32(send ? "RNET_SIM_SEND_JITTER_MS" : "RNET_SIM_JITTER_MS", profile_jitter);
    /* Jitter wider than the latency would ask for a negative delay half the
     * time; clamping here keeps the distribution symmetric and honest rather
     * than silently squashing one tail against zero. */
    if (jitter > latency)
    {
        fprintf(stderr,
                "rnet: RNET_SIM_JITTER_MS=%u clamped to the %u ms latency "
                "(a wider spread would need negative delay)\n",
                (unsigned)jitter, (unsigned)latency);
        fflush(stderr);
        jitter = latency;
    }

    sim = (RNetNetSim *)calloc(1u, sizeof(*sim));
    if (sim == NULL)
    {
        return NULL;
    }
    sim->latency_ms = latency;
    sim->jitter_ms = jitter;
    sim->loss_pct = loss;
    sim->duplicate_pct = dup > 100u ? 100u : dup;
    sim->burst_pct = burst > 100u ? 100u : burst;
    sim->burst_packets = env_u32(send ? "RNET_SIM_SEND_BURST_PACKETS" : "RNET_SIM_BURST_PACKETS", 3u);
    if (sim->burst_packets > 1024u) sim->burst_packets = 1024u;
    sim->kbps = kbps;
    sim->rng = env_u32("RNET_SIM_SEED", 0x5eed1234u);
    if (send) sim->rng ^= 0xa511e9b3u;
    if (sim->rng == 0u)
    {
        sim->rng = 0x5eed1234u; /* xorshift is absorbing at zero */
    }
    netsim_banner(sim, rnet_os_monotonic_ms());
    fprintf(stderr,"rnet: simulator direction=%s duplicate=%u%% burst=%u%%/%u packets bandwidth=%u kbit/s (0=unlimited)\n",
        send ? "send" : "receive", sim->duplicate_pct, sim->burst_pct, sim->burst_packets, sim->kbps);
    return sim;
}
RNetNetSim *rnet_netsim_create(void) { return netsim_create(0); }
RNetNetSim *rnet_netsim_create_send(void) { return netsim_create(1); }

void rnet_netsim_destroy(RNetNetSim *sim)
{
    if (sim == NULL)
    {
        return;
    }
    fprintf(stderr,
            "rnet: link simulator released — held=%llu dropped=%llu "
            "overflow=%llu peak queue=%u\n",
            (unsigned long long)sim->held_total,
            (unsigned long long)sim->dropped_total,
            (unsigned long long)sim->overflow_total,
            (unsigned)sim->live_peak);
    fflush(stderr);
    free(sim);
}

int rnet_netsim_hold(RNetNetSim *sim, const rnet_u8 *buf, size_t len,
                     const struct sockaddr_in *src)
{
    rnet_u32 i;
    rnet_u64 delay;

    if ((sim == NULL) || (buf == NULL) || (len == 0u) || (len > RNET_MAX_PACKET))
    {
        return 0;
    }
    if (!sim->burst_remaining && sim->burst_pct &&
        netsim_rand(sim) % 100u < sim->burst_pct) sim->burst_remaining = sim->burst_packets;
    if (sim->burst_remaining) {
        --sim->burst_remaining; ++sim->dropped_total; return 1;
    }
    if (sim->loss_pct > 0u)
    {
        /* Drawn from the same seeded stream as the jitter, so a seeded run
         * drops the same datagrams every time. A loss test that cannot be
         * repeated cannot confirm a fix. */
        if ((netsim_rand(sim) % 100u) < sim->loss_pct)
        {
            sim->dropped_total++;
            return 1; /* taken and discarded: the caller must not deliver it */
        }
    }
    for (i = 0u; i < RNET_NETSIM_SLOTS; ++i)
    {
        if (sim->slot[i].in_use == 0)
        {
            break;
        }
    }
    if (i == RNET_NETSIM_SLOTS)
    {
        sim->overflow_total++;
        /* Loud: from here the run's timings are not the timings that were
         * asked for, and a reader must not take the result at face value. */
        fprintf(stderr,
                "rnet: LINK SIMULATOR OVERFLOW — hold queue full at %u, "
                "delivering undelayed (total %llu). Timings in this run are "
                "NOT the configured ones.\n",
                (unsigned)RNET_NETSIM_SLOTS,
                (unsigned long long)sim->overflow_total);
        fflush(stderr);
        return 0;
    }

    delay = sim->latency_ms;
    if (sim->jitter_ms > 0u)
    {
        rnet_u32 span = (sim->jitter_ms * 2u) + 1u;
        delay += (rnet_u64)(netsim_rand(sim) % span);
        delay -= sim->jitter_ms;
    }

    memcpy(sim->slot[i].data, buf, len);
    sim->slot[i].len = len;
    sim->slot[i].due_ms = rnet_os_monotonic_ms() + delay;
    if (sim->kbps) {
        rnet_u64 at = rnet_os_monotonic_ms() * 1000u;
        if (sim->next_wire_us < at) sim->next_wire_us = at;
        sim->next_wire_us += ((rnet_u64)len * 8000u + sim->kbps - 1u) / sim->kbps;
        sim->slot[i].due_ms = (sim->next_wire_us + 999u) / 1000u + delay;
    }
    if (src != NULL)
    {
        sim->slot[i].src = *src;
    }
    else
    {
        memset(&sim->slot[i].src, 0, sizeof(sim->slot[i].src));
    }
    sim->slot[i].in_use = 1;
    sim->held_total++;
    sim->live++;
    if (sim->live > sim->live_peak)
    {
        sim->live_peak = sim->live;
    }
    if (sim->duplicate_pct && netsim_rand(sim) % 100u < sim->duplicate_pct) {
        rnet_u32 copy;
        for (copy = 0; copy < RNET_NETSIM_SLOTS; ++copy) if (!sim->slot[copy].in_use) break;
        if (copy < RNET_NETSIM_SLOTS) {
            sim->slot[copy] = sim->slot[i]; ++sim->slot[copy].due_ms;
            if (sim->kbps) {
                sim->next_wire_us += ((rnet_u64)len * 8000u + sim->kbps - 1u) / sim->kbps;
                sim->slot[copy].due_ms = (sim->next_wire_us + 999u) / 1000u + delay;
            }
            ++sim->live; ++sim->held_total;
            if (sim->live > sim->live_peak) sim->live_peak = sim->live;
        } else { ++sim->overflow_total; fprintf(stderr,"rnet: LINK SIMULATOR OVERFLOW duplicating packet\n"); }
    }
    return 1;
}

int rnet_netsim_due(RNetNetSim *sim, rnet_u8 *buf, size_t cap,
                    struct sockaddr_in *out_src)
{
    rnet_u64 now;
    rnet_u32 i;
    rnet_u32 best = RNET_NETSIM_SLOTS;
    int n;

    if ((sim == NULL) || (buf == NULL) || (cap == 0u))
    {
        return 0;
    }
    now = rnet_os_monotonic_ms();
    /* Earliest DUE, not oldest held: jitter is allowed to reorder, and a
     * bounded scan also preserves deterministic due-time ordering; it
     * does not release any packet before its configured delay. */
    for (i = 0u; i < RNET_NETSIM_SLOTS; ++i)
    {
        if (sim->slot[i].in_use == 0)
        {
            continue;
        }
        if (sim->slot[i].due_ms > now)
        {
            continue;
        }
        if ((best == RNET_NETSIM_SLOTS) ||
            (sim->slot[i].due_ms < sim->slot[best].due_ms))
        {
            best = i;
        }
    }
    if (best == RNET_NETSIM_SLOTS)
    {
        return 0;
    }
    if (sim->slot[best].len > cap)
    {
        /* Cannot happen while every caller passes RNET_MAX_PACKET, which is
         * also the hold cap — but dropping silently on a shrunk buffer would
         * read as packet loss the simulator was never asked to inject. */
        fprintf(stderr,
                "rnet: link simulator dropping a %u-byte datagram into a "
                "%u-byte buffer — this is a caller bug, not simulated loss\n",
                (unsigned)sim->slot[best].len, (unsigned)cap);
        fflush(stderr);
        sim->slot[best].in_use = 0;
        sim->live--;
        return 0;
    }
    n = (int)sim->slot[best].len;
    memcpy(buf, sim->slot[best].data, sim->slot[best].len);
    if (out_src != NULL)
    {
        *out_src = sim->slot[best].src;
    }
    sim->slot[best].in_use = 0;
    sim->live--;
    return n;
}

void rnet_netsim_heartbeat(RNetNetSim *sim)
{
    rnet_u64 now;

    if (sim == NULL)
    {
        return;
    }
    now = rnet_os_monotonic_ms();
    if (now >= sim->next_banner_ms)
    {
        netsim_banner(sim, now);
    }
}
