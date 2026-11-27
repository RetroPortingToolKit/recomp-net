#ifndef RECOMP_NET_TRANSPORT_H
#define RECOMP_NET_TRANSPORT_H

#include "recomp_net/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RNetTransportMode
{
    RNET_TRANSPORT_NONE = 0,
    RNET_TRANSPORT_LAN_UDP = 1,
    RNET_TRANSPORT_ICE = 2,
    RNET_TRANSPORT_ICE_HUB = 3,
    RNET_TRANSPORT_EXTERNAL = 4
} RNetTransportMode;

/* An already-connected, host-owned datagram path, e.g. a launcher retaining
 * waiting-room ICE while the simulation runs in a child process. No pointers
 * to ICE agents cross that process boundary. The owner supplies bounded queues
 * and the same peer authentication/hub fan-out as its proven connection.
 * All callbacks run on the session thread and must be nonblocking. send returns
 * exactly len on success; recv returns one complete datagram, 0 when empty,
 * or -1 on disconnect/overflow/error. A partial send is a terminal failure.
 * pump is optional (0 success, -1 failure). The caller owns ctx until after
 * session destruction. path: 0 unknown, 1 direct, 2 host hub, 3 TURN. */
typedef struct RNetPacketTransport
{
    void *ctx;
    int (*send)(void *ctx, const rnet_u8 *bytes, size_t len);
    int (*recv)(void *ctx, rnet_u8 *bytes, size_t capacity);
    int (*pump)(void *ctx);
    rnet_u8 path;
} RNetPacketTransport;

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NET_TRANSPORT_H */
