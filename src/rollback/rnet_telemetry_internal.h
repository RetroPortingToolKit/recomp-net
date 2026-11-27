#ifndef RNET_TELEMETRY_INTERNAL_H
#define RNET_TELEMETRY_INTERNAL_H
#include "recomp_net/telemetry.h"
/* A driver can retain work after its last completed live tick. Merge that
 * tail into a report without fabricating another tick or mutating the ring. */
size_t rnet_telemetry_report_extra(const RNetTelemetry *, const RNetTickTelemetry *,
                                  char *, size_t);
#endif
