/* SPDX-License-Identifier: MIT */
#ifndef RECOMP_NET_PACKET_PIPE_H
#define RECOMP_NET_PACKET_PIPE_H
#include "recomp_net/transport.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Windows child-process adapter for an already-private pair of anonymous
 * pipes. Only the explicitly selected child inherits the two endpoint handles;
 * the adapter opens no port and carries no account/ICE credentials. Other
 * platforms keep using the portable host-supplied packet transport interface.
 * One caller thread owns send/recv/close. Internal workers perform blocking
 * pipe IO; callbacks only copy a bounded datagram to/from a bounded queue.
 * Overflow, malformed framing, EOF and IO failure are terminal, never loss.
 */
#define RNET_PACKET_PIPE_MAX 1200u
#define RNET_PACKET_PIPE_CAPACITY 256u
typedef struct RNetPacketPipe RNetPacketPipe;
enum RNetPacketPipeError {
    RNET_PIPE_OK=0, RNET_PIPE_CLOSED=1, RNET_PIPE_IO=2,
    RNET_PIPE_FRAME=3, RNET_PIPE_OVERFLOW=4
};
int rnet_packet_pipe_available(void);
/* Takes ownership of both handles only on success. Clears their inheritance
 * flags in this process. path is the actual selected external path (0..3). */
RNetPacketPipe *rnet_packet_pipe_open(uintptr_t read_handle, uintptr_t write_handle,
                                    rnet_u8 path);
void rnet_packet_pipe_close(RNetPacketPipe *pipe);
RNetPacketTransport rnet_packet_pipe_transport(RNetPacketPipe *pipe);
int rnet_packet_pipe_send(RNetPacketPipe *pipe,const rnet_u8 *bytes,size_t size);
int rnet_packet_pipe_recv(RNetPacketPipe *pipe,rnet_u8 *bytes,size_t capacity);
int rnet_packet_pipe_error(const RNetPacketPipe *pipe);

/* Validate a complete gameplay datagram against its authenticated ICE sender
 * before a host hub forwards it. Includes normal protocol decoding/checksum,
 * session identity and source slot; START/DELAY_SYNC are host-only. */
int rnet_packet_source_valid(const rnet_u8 *bytes,size_t size,rnet_u32 magic,
                             rnet_u32 session_id,int source_slot,int slot_count);
#ifdef __cplusplus
}
#endif
#endif
