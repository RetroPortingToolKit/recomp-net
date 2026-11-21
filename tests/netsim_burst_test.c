#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "transport/rnet_netsim.h"

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

int main(void)
{
    RNetNetSim *sim;
    struct sockaddr_in source;
    unsigned i, value;
    environment("RNET_SIM_LATENCY_MS", "0");
    environment("RNET_SIM_LOSS_PCT", "0");
    environment("RNET_SIM_JITTER_MS", "0");
    if (rnet_netsim_create() != NULL) return 1;
    environment("RNET_SIM_LATENCY_MS", "60");
    sim = rnet_netsim_create();
    if (!sim) return 2;
    memset(&source, 0, sizeof(source));
    for (i = 0; i < 300; ++i) {
        if (!rnet_netsim_hold(sim, (const rnet_u8 *)&i, sizeof(i), &source)) {
            fprintf(stderr, "burst packet %u escaped its configured delay\n", i);
            rnet_netsim_destroy(sim);
            return 3;
        }
    }
    now_ms = 1059;
    if (rnet_netsim_due(sim, (rnet_u8 *)&value, sizeof(value), NULL)) return 4;
    now_ms = 1060;
    for (i = 0; i < 300; ++i) {
        if (rnet_netsim_due(sim, (rnet_u8 *)&value, sizeof(value), NULL) != sizeof(value) || value != i)
            return 5;
    }
    if (rnet_netsim_due(sim, (rnet_u8 *)&value, sizeof(value), NULL)) return 6;
    rnet_netsim_destroy(sim);
    puts("delayed control burst: all 300 packets held for 60 ms and delivered in order");
    return 0;
}
