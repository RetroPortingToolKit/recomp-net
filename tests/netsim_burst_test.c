#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "transport/rnet_netsim.h"
#include "protocol/rnet_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static rnet_u64 now_ms = 1000;
rnet_u64 rnet_os_monotonic_ms(void) { return now_ms; }

static void environment(const char *name, const char *value)
{
#ifdef _WIN32
    (void)_putenv_s(name, value);
#else
    (void)setenv(name, value, 1);
#endif
}

static int delayed_burst(unsigned count, unsigned delay)
{
    RNetNetSim *sim;
    struct sockaddr_in source;
    unsigned i, value;
    char delay_text[16];
    snprintf(delay_text, sizeof(delay_text), "%u", delay);
    environment("RNET_SIM_LATENCY_MS", delay_text);
    sim = rnet_netsim_create();
    if (!sim) return 2;
    memset(&source, 0, sizeof(source));
    for (i = 0; i < count; ++i) {
        if (!rnet_netsim_hold(sim, (const rnet_u8 *)&i, sizeof(i), &source)) {
            fprintf(stderr, "burst packet %u escaped its configured delay\n", i);
            rnet_netsim_destroy(sim);
            return 3;
        }
    }
    now_ms += delay - 1u;
    if (rnet_netsim_due(sim, (rnet_u8 *)&value, sizeof(value), NULL)) return 4;
    now_ms++;
    for (i = 0; i < count; ++i) {
        if (rnet_netsim_due(sim, (rnet_u8 *)&value, sizeof(value), NULL) != sizeof(value) || value != i)
            return 5;
    }
    if (rnet_netsim_due(sim, (rnet_u8 *)&value, sizeof(value), NULL)) return 6;
    rnet_netsim_destroy(sim);
    printf("delayed control burst: all %u packets held for %u ms and delivered in order\n", count, delay);
    return 0;
}

int main(void)
{
    RNetNetSim *sim;
    unsigned held, value, i;
    int result;
    environment("RNET_SIM_LATENCY_MS", "0");
    environment("RNET_SIM_LOSS_PCT", "0");
    environment("RNET_SIM_JITTER_MS", "0");
    if (rnet_netsim_create() != NULL) return 1;
    result = delayed_burst(300u, 60u);
    if (result) return result;
    /* A legal eight-MiB transfer can arrive while simulation is parked.
     * Keep control traffic alongside its complete fragmented payload. */
    result = delayed_burst(RNET_STATE_MAX_CHUNKS + 512u, 80u);
    if (result) return result;

    /* The queue must still have a finite resource bound, loudly refusing an
     * extra packet rather than claiming the requested latency was preserved. */
    sim = rnet_netsim_create();
    if (!sim) return 7;
    for (held = 0; held < RNET_STATE_MAX_CHUNKS + 2048u; ++held) {
        if (!rnet_netsim_hold(sim, (const rnet_u8 *)&held, sizeof(held), NULL)) break;
    }
    if (held < RNET_STATE_MAX_CHUNKS + 512u || held == RNET_STATE_MAX_CHUNKS + 2048u)
        return 8;
    now_ms += 79u;
    if (rnet_netsim_due(sim, (rnet_u8 *)&value, sizeof(value), NULL)) return 9;
    now_ms++;
    for (i = 0; i < held; ++i) {
        if (rnet_netsim_due(sim, (rnet_u8 *)&value, sizeof(value), NULL) != sizeof(value) || value != i)
            return 10;
    }
    if (rnet_netsim_due(sim, (rnet_u8 *)&value, sizeof(value), NULL)) return 11;
    rnet_netsim_destroy(sim);
    puts("bounded queue: explicit over-capacity refusal; all accepted packets retain delay/order");
    return 0;
}
