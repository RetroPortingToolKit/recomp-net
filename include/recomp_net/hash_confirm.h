#ifndef RECOMP_NET_HASH_CONFIRM_H
#define RECOMP_NET_HASH_CONFIRM_H

/*
 * Local digest ring + peer FRAME_COMMIT matching → resolved_through watermark.
 *
 * rnet_hc_confirm_through(T) is 1 iff every tick in (prev_resolved, T] has a
 * local digest that matched the peer FRAME_COMMITs (contiguous from the prior
 * watermark). Bind to RNetInputContractHostGates.hash_confirm_promote /
 * RNetRollbackVTable.hash_confirm_through.
 *
 * Two modes, fixed at (re)initialisation:
 *
 *   Legacy (rnet_hc_reset): exactly one implicit peer, fed by
 *     rnet_hc_note_peer. This is the original two-player tracker and its
 *     behaviour is unchanged.
 *
 *   N-way (rnet_hc_init_n): one digest ring per seat in `peer_mask` (bit i =
 *     seat i, normally rnet_expected_peer_mask(...) -- every occupied seat
 *     except our own; spectators never appear). Fed by rnet_hc_note_peer_slot
 *     with the sender's seat (rnet_session_take_rb_frame_commit_from). A tick
 *     resolves only when EVERY expected seat has reported it and every digest
 *     equals the local one. A mismatch from ANY seat -- even before the
 *     others have reported -- is a stop: the watermark holds and
 *     rnet_hc_peek_mismatch_slot names the seat.
 *
 * The legacy single-peer tracker fed from an unattributed queue advanced on a
 * 1-of-N match at N > 2: whichever seat's commit happened to land last in the
 * one-per-tick cell decided agreement, so a diverged seat could be masked by
 * a healthy one. N-way mode exists so that cannot happen.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNET_HC_RING 128u
/* One ring per possible seat (== RNET_MAX_SLOTS). */
#define RNET_HC_MAX_PEERS 8u

typedef struct RNetHcRing {
    uint32_t tick[RNET_HC_RING];
    uint32_t digest[RNET_HC_RING];
    uint8_t  valid[RNET_HC_RING];
} RNetHcRing;

typedef struct RNetHashConfirm {
    RNetHcRing local;
    /* Legacy: peer[0] is the implicit peer. N-way: peer[i] is seat i. */
    RNetHcRing peer[RNET_HC_MAX_PEERS];
    uint32_t resolved_through; /* inclusive; 0 = none yet (tick 0 may match) */
    uint8_t  resolved_valid;   /* 0 until first match advances watermark */
    uint8_t  nway;             /* 0 = legacy single implicit peer */
    uint32_t peer_mask;        /* N-way: seats that must agree */
} RNetHashConfirm;

/* Legacy (two-player) tracker: clear everything, one implicit peer. */
void rnet_hc_reset(RNetHashConfirm *hc);

/* N-way tracker: clear everything; every seat in peer_mask must agree.
 * Bits at or above RNET_HC_MAX_PEERS are dropped. peer_mask == 0 is a
 * tracker with nobody to agree with: it never resolves (fail closed). */
void rnet_hc_init_n(RNetHashConfirm *hc, uint32_t peer_mask);

/* Change the expected seats mid-session (a seat left). Keeps every digest
 * and the watermark, then re-evaluates: removing a seat that was the only
 * one missing lets the watermark move. Switches a legacy tracker to N-way. */
void rnet_hc_set_peer_mask(RNetHashConfirm *hc, uint32_t peer_mask);

uint8_t  rnet_hc_is_nway(const RNetHashConfirm *hc);
uint32_t rnet_hc_peer_mask(const RNetHashConfirm *hc);

/* Clear the rings and set resolved_through = last_ok so the next compared tick
 * is last_ok+1. Used at Replay entry to drop live invent FRAME_COMMITs. Keeps
 * the mode and peer mask. */
void rnet_hc_prime_after(RNetHashConfirm *hc, uint32_t last_ok);

void rnet_hc_note_local(RNetHashConfirm *hc, uint32_t tick, uint32_t digest);

/* Legacy feed (the implicit peer). Ignored by an N-way tracker: a digest
 * with no sender can never be allowed to satisfy a seat. */
void rnet_hc_note_peer(RNetHashConfirm *hc, uint32_t tick, uint32_t digest);

/* N-way feed. Returns 1 if recorded; 0 when the tracker is legacy, the slot
 * is outside peer_mask (our own seat, an empty seat, a spectator), or hc is
 * NULL. Same tick again = latest wins (identical to the legacy cell). */
uint8_t rnet_hc_note_peer_slot(RNetHashConfirm *hc, uint32_t slot, uint32_t tick,
                               uint32_t digest);

uint32_t rnet_hc_resolved_through(const RNetHashConfirm *hc);
uint8_t  rnet_hc_confirm_through(const RNetHashConfirm *hc, uint32_t tick);

uint8_t rnet_hc_local_digest(const RNetHashConfirm *hc, uint32_t tick,
                             uint32_t *digest_out);

/* Legacy: the implicit peer's digest. N-way: a consensus view that keeps an
 * N=2-shaped consumer (compare against local) correct -- returns the first
 * (lowest-seat) peer digest that DIFFERS from the reference (the local
 * digest, or the lowest reporting seat's when local is absent) as soon as
 * one exists; otherwise returns the shared digest once every expected seat
 * has reported; otherwise 0. */
uint8_t rnet_hc_peer_digest(const RNetHashConfirm *hc, uint32_t tick,
                            uint32_t *digest_out);

/* One seat's digest (N-way; legacy answers for slot 0 = implicit peer). */
uint8_t rnet_hc_peer_digest_slot(const RNetHashConfirm *hc, uint32_t slot, uint32_t tick,
                                 uint32_t *digest_out);

/* Seats in peer_mask that have reported `tick` (N-way; legacy: bit 0). */
uint32_t rnet_hc_reported_mask(const RNetHashConfirm *hc, uint32_t tick);

/* Mismatch at the next unresolved tick (resolved_through + 1). */
uint8_t rnet_hc_peek_mismatch(const RNetHashConfirm *hc, uint32_t *tick_out,
                              uint32_t *local_out, uint32_t *peer_out);

/* Same, naming the first (lowest) mismatching seat. Legacy reports slot 0.
 * N-way: 1 as soon as the local digest exists and ANY reporting seat
 * differs, whether or not every seat has reported. */
uint8_t rnet_hc_peek_mismatch_slot(const RNetHashConfirm *hc, uint32_t *tick_out,
                                   uint32_t *slot_out, uint32_t *local_out,
                                   uint32_t *peer_out);

/* Heal a stuck watermark when the next tick aged out of the ring and is no
 * longer a live mismatch. Returns 1 if the watermark moved. N-way: jumps to
 * the highest later tick that every expected seat reported equal to local,
 * and refuses if any later tick shows any seat disagreeing. */
uint8_t rnet_hc_heal_stale_gap(RNetHashConfirm *hc);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NET_HASH_CONFIRM_H */
