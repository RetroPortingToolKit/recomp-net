#ifndef RECOMP_NET_HASH_CONFIRM_H
#define RECOMP_NET_HASH_CONFIRM_H

/*
 * Local digest ring + peer FRAME_COMMIT matching → resolved_through watermark.
 *
 * rnet_hc_confirm_through(T) is 1 iff every tick in (prev_resolved, T] has a
 * local digest that matched a peer FRAME_COMMIT (contiguous from the prior
 * watermark). Bind to RNetInputContractHostGates.hash_confirm_promote /
 * RNetRollbackVTable.hash_confirm_through.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNET_HC_RING 128u

typedef struct RNetHashConfirm {
    uint32_t local_tick[RNET_HC_RING];
    uint32_t local_digest[RNET_HC_RING];
    uint8_t  local_valid[RNET_HC_RING];
    uint32_t peer_tick[RNET_HC_RING];
    uint32_t peer_digest[RNET_HC_RING];
    uint8_t  peer_valid[RNET_HC_RING];
    uint32_t resolved_through; /* inclusive; 0 = none yet (tick 0 may match) */
    uint8_t  resolved_valid;   /* 0 until first match advances watermark */
} RNetHashConfirm;

void rnet_hc_reset(RNetHashConfirm *hc);

/* Clear the ring and set resolved_through = last_ok so the next compared tick
 * is last_ok+1. Used at Replay entry to drop live invent FRAME_COMMITs. */
void rnet_hc_prime_after(RNetHashConfirm *hc, uint32_t last_ok);

void rnet_hc_note_local(RNetHashConfirm *hc, uint32_t tick, uint32_t digest);
void rnet_hc_note_peer(RNetHashConfirm *hc, uint32_t tick, uint32_t digest);

uint32_t rnet_hc_resolved_through(const RNetHashConfirm *hc);
uint8_t  rnet_hc_confirm_through(const RNetHashConfirm *hc, uint32_t tick);

uint8_t rnet_hc_local_digest(const RNetHashConfirm *hc, uint32_t tick,
                            uint32_t *digest_out);
uint8_t rnet_hc_peer_digest(const RNetHashConfirm *hc, uint32_t tick,
                           uint32_t *digest_out);

uint8_t rnet_hc_peek_mismatch(const RNetHashConfirm *hc, uint32_t *tick_out,
                             uint32_t *local_out, uint32_t *peer_out);

/* Heal a stuck watermark when the next tick aged out of the ring and is no
 * longer a live mismatch. Returns 1 if the watermark moved. */
uint8_t rnet_hc_heal_stale_gap(RNetHashConfirm *hc);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NET_HASH_CONFIRM_H */
