#ifndef RECOMP_NET_ROLLBACK_H
#define RECOMP_NET_ROLLBACK_H

/*
 * Shared rollback episode orchestration — game-agnostic core.
 *
 * recomp-net owns the episode FSM (when to rewind vs promote), the correction
 * tuple, the sealed input table, and the resolved-through / shared frontier
 * watermarks. The host owns everything game-specific: snapshot save/load, the
 * deterministic sim step, state digests, and (initially) input prediction and
 * the wire transport that delivers seal/baseline/sync packets.
 *
 * This is the second rollback layer after the portable input contract
 * (recomp_net/input_contract.h). It is transport-agnostic: hosts call the API
 * from their own network ingress -- RNetSession's RNET_PKT_RB_* opcodes
 * (session.h, take_rb_*_from) or a host's own wire. N-peer (3+ seat)
 * coordination: see begin_episode_from / arbitrate_begin / RNetRbPeerAgree
 * below and docs/rollback.md.
 *
 * Single-threaded session ownership, same as delay-sync RNetSession.
 */

#include <stdint.h>
#include <stddef.h>

#include "recomp_net/input_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every sealed row needs a bit in the uint64_t peer-completion mask.
 * Hosts must split larger replay windows into separate episodes. */
#define RNET_RB_PEER_SEAL_MASK_BITS 64u
#define RNET_RB_SEAL_MAX_SPAN RNET_RB_PEER_SEAL_MASK_BITS
#define RNET_RB_MAX_SLOTS 8
/* Tip episode: target - load at or below this may skip the ready-ACK RTT
 * (digests still compared). Sized for tip-extend re-replay after TipHold. */
#define RNET_RB_LIGHT_TIP_MAX_DEPTH 16u
/* Live quiet window after POST match (TipHold) so a physical press→release
 * (typically 6–15 ticks later) tip-extends the same episode instead of a
 * second seal/baseline handshake. Also the cap for suggest_target slack. */
#define RNET_RB_TIP_RUNWAY_DEFAULT 12u
/* Initial seal headroom past live tip when opening an episode. Kept small —
 * TipHold covers the release; burning a long runway in catch-up finishes
 * before the finger lifts. */
#define RNET_RB_TIP_SEAL_SLACK_DEFAULT 2u
/* corr.flags */
#define RNET_RB_CORR_LIGHT_TIP 0x01u

typedef enum RNetRbPhase
{
    nRNetRbPhaseLive = 0,
    nRNetRbPhaseSealInputs,
    nRNetRbPhaseAwaitingBaseline,
    nRNetRbPhaseReplay,
    nRNetRbPhaseVerify,
    /* POST matched: seals stay open for tip-extend while host runs Live. */
    nRNetRbPhaseTipHold,
    nRNetRbPhaseCommit,
    nRNetRbPhaseAbort
} RNetRbPhase;

typedef enum RNetRbRole
{
    nRNetRbRoleInitiator = 0,
    nRNetRbRoleFollower
} RNetRbRole;

typedef enum RNetRbEventType
{
    nRNetRbEventNone = 0,
    nRNetRbEventInputMismatch,
    nRNetRbEventPeerSymmetric,
    nRNetRbEventStateDiverge,
    nRNetRbEventFrameCommit
} RNetRbEventType;

typedef struct RNetRbCorrection
{
    uint32_t epoch_id;
    uint32_t mismatch_tick;
    uint32_t load_tick;
    uint32_t target_tick;
    int32_t slot;            /* corrected player slot (-1 = whole state) */
    uint8_t initiator;       /* 1 when this host started the episode */
    uint8_t from_peer_notify;
    uint8_t flags;           /* RNET_RB_CORR_LIGHT_TIP, … */
} RNetRbCorrection;

/* Opaque per-slot input row the library seals and replays. The library never
 * interprets the payload beyond the stick-replace contract view.
 * analog: host pad type (0 = digital / poll 0x41, 1 = DualShock / 0x73).
 * Carried on the seal wire in RNetRbWireFrame.source (was always 0). */
typedef struct RNetRbFrame
{
    uint32_t tick;
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
    uint8_t is_predicted;
    uint8_t is_valid;
    uint8_t analog;
} RNetRbFrame;

typedef struct RNetRbEvent
{
    RNetRbEventType type;
    int32_t slot;
    uint32_t mismatch_tick;
    uint32_t target_tick;
    uint32_t load_tick;
    uint32_t epoch_id;
    uint8_t follower_local_auth;
} RNetRbEvent;

/*
 * Host callbacks. save/load/advance/digest are required; the gates mirror the
 * input contract and may be NULL (portable defaults). All runs on the host's
 * thread; the library does not spawn work.
 */
typedef struct RNetRollbackVTable
{
    void *ctx;
    /* Persist a snapshot at sim tick (library requests the deepest needed). */
    int (*save_state)(void *ctx, uint32_t tick);
    /* Restore the snapshot captured at sim tick before replay. */
    int (*load_state)(void *ctx, uint32_t tick);
    /* Advance exactly one deterministic sim tick using resolved inputs the
     * host published (sealed local + confirmed/peer-sealed remote rows). */
    int (*advance_sim)(void *ctx, uint32_t tick);
    /* Digest of canonical state at tick for agreement comparison; partition
     * selects a subsystem (0 = master). Must be identical across peers. */
    uint32_t (*state_digest)(void *ctx, uint32_t tick, uint32_t partition);
    /* 1 when the peer state/master-hash watermark has agreed through tick
     * (frame-commit). Backs input-contract hash_confirm_promote. */
    uint8_t (*hash_confirm_through)(void *ctx, uint32_t tick);
    /* Sample the authoritative row for a slot at a wire tick from the host's
     * input history (for sealing + self-seal fallback). */
    uint8_t (*get_input_row)(void *ctx, int32_t slot, uint32_t tick, RNetRbFrame *out_frame);
    /* Stick-replace contract gates; NULL = portable defaults. */
    RNetInputContractHostGates stick_gates;
} RNetRollbackVTable;

typedef struct RNetRbSession RNetRbSession;

typedef struct RNetRbConfig
{
    /* This host's player slot -- or `slot_count` exactly, which means
     * OBSERVER: a spectator that runs the simulation and owns no seat.
     *
     * local_slot is only ever compared, never used as an index, and every
     * comparison asks the same question: "is this the slot I own?" A value one
     * past the last seat answers no for every slot, and the four behaviours
     * fall out already correct -- every row is sealed from the wire rather
     * than from get_input_row, only CONFIRMED rows count as authority (an
     * observer must never credit its own prediction), row validity is read
     * from the peer mask, and admission waits on every seat instead of
     * skipping one. So an observer is not a mode with its own code path; it is
     * the ordinary path with no seat of its own, which is why it cannot drift
     * from the seated one. */
    uint32_t local_slot;
    uint32_t delay;            /* committed input delay D */
    uint32_t seal_max_span;    /* <= RNET_RB_SEAL_MAX_SPAN; 0 = default */
    /* Active seats in this match (1..RNET_RB_MAX_SLOTS). Peer-seal completion
     * only waits on OCCUPIED slots in [0, slot_count) excluding local_slot
     * (see occupied_mask) -- so an observer, whose local_slot is outside that
     * range, waits on all of them. 0 => 2. */
    uint32_t slot_count;
    /* TipHold quiet window after POST match (0 = finalize immediately;
     * RNET_RB_TIP_RUNWAY_DEFAULT recommended for digital hosts). Also the
     * maximum slack suggest_target may add (capped by tip_seal_slack). */
    uint32_t tip_runway;
    /* Initial seal headroom past max(sim, mismatch). 0 → TIP_SEAL_SLACK_DEFAULT
     * when tip_runway > 0; set UINT32_MAX to force 0 slack. */
    uint32_t tip_seal_slack;
    /* Max (target - load) depth eligible to skip the ready-ACK RTT (see
     * rnet_rb_is_light_tip_candidate). 0 → RNET_RB_LIGHT_TIP_MAX_DEPTH.
     * Hosts that widen tip_runway for TipHold coalescing (a tip-extended
     * episode's eventual depth can approach tip_runway) should set this to
     * match tip_runway — otherwise every episode that coalesces past the
     * library default of 16 silently loses the light-tip fast path and pays
     * a second RTT it didn't need to. Clamped like tip_runway (max 32). */
    uint32_t light_tip_max_depth;
    /* Bit i = seat i is occupied by a real peer (same meaning as
     * RNetConfig.occupied_mask). 0 = every seat in [0, slot_count) (legacy).
     * Sparse rooms (e.g. seats 0+2 of 4) must clear the empty bits, or
     * peer-seal completion and the N-way helpers below wait forever on a seat
     * nobody sits in. Appended last so a zero-initialised config keeps the
     * old behaviour. */
    uint32_t occupied_mask;
} RNetRbConfig;

/* "No seat": an initiator slot that is not known (legacy begin_episode on a
 * follower), or no mismatching seat. */
#define RNET_RB_SLOT_NONE 0xffffffffu

/* Lifecycle. */
RNetRbSession *rnet_rb_create(const RNetRbConfig *cfg, const RNetRollbackVTable *vt);
void rnet_rb_destroy(RNetRbSession *s);
void rnet_rb_session_reset(RNetRbSession *s);

/* Phase / tuple introspection (read-only). */
RNetRbPhase rnet_rb_get_phase(const RNetRbSession *s);
uint8_t rnet_rb_is_active(const RNetRbSession *s);
uint8_t rnet_rb_is_resimulating(const RNetRbSession *s);
uint8_t rnet_rb_is_tip_holding(const RNetRbSession *s);
uint32_t rnet_rb_get_tip_runway(const RNetRbSession *s);
uint32_t rnet_rb_get_tip_seal_slack(const RNetRbSession *s);
uint32_t rnet_rb_get_light_tip_max_depth(const RNetRbSession *s);
uint32_t rnet_rb_get_epoch_id(const RNetRbSession *s);
uint32_t rnet_rb_get_mismatch_tick(const RNetRbSession *s);
uint32_t rnet_rb_get_load_tick(const RNetRbSession *s);
uint32_t rnet_rb_get_target_tick(const RNetRbSession *s);
int32_t rnet_rb_get_corrected_slot(const RNetRbSession *s);

/* 1 when this session owns no seat: it simulates and displays the match and
 * contributes no input row to anybody. */
uint8_t rnet_rb_is_observer(const RNetRbSession *s);
uint8_t rnet_rb_is_from_peer_notify(const RNetRbSession *s);
uint8_t rnet_rb_get_corr_flags(const RNetRbSession *s);

/* 1 when (target - load) <= RNET_RB_LIGHT_TIP_MAX_DEPTH and load is at/after
 * the shared frontier (resolved_through). Hosts may skip ready-ACK RTT.
 * Convenience wrapper over rnet_rb_is_light_tip_candidate_ex using the
 * library-wide default depth; a session created with a non-default
 * cfg.light_tip_max_depth should call the _ex form (or just
 * rnet_rb_recommend_light_tip, which already does). */
uint8_t rnet_rb_is_light_tip_candidate(uint32_t load_tick, uint32_t target_tick,
                                       uint32_t resolved_through);
/* Same as above with an explicit max-depth ceiling — use this (with
 * rnet_rb_get_light_tip_max_depth(session)) when precomputing the flag for a
 * session configured with a non-default light_tip_max_depth. */
uint8_t rnet_rb_is_light_tip_candidate_ex(uint32_t load_tick, uint32_t target_tick,
                                          uint32_t resolved_through, uint32_t max_depth);
uint8_t rnet_rb_recommend_light_tip(const RNetRbSession *s);

/*
 * Correction entry points. Host calls begin_episode when it (or a symmetric
 * peer notice) identifies a mismatch; the library takes the phase to
 * SealInputs. The host then drives sealing and peer exchange, advancing the
 * FSM with set_phase as its transport confirms baseline/replay/verify.
 */
void rnet_rb_begin_episode(RNetRbSession *s, const RNetRbCorrection *corr);
void rnet_rb_set_phase(RNetRbSession *s, RNetRbPhase phase);

/*
 * N-peer episode coordination.
 *
 * With two seats the initiator of any episode is "me or the one other seat",
 * so the FSM never had to remember WHO opened it. With three or more, two
 * seats can open episodes concurrently and a third can be asked to follow
 * both, so the initiator's seat is part of the episode's identity.
 *
 * rnet_rb_begin_episode_from records it: pass the BEGIN's sender seat
 * (rnet_session_take_rb_sync_from) for a follow episode, or your own seat
 * when initiating. Plain rnet_rb_begin_episode records local_slot when
 * corr->initiator is set and RNET_RB_SLOT_NONE otherwise (unchanged N=2
 * behaviour: the other seat is implied).
 */
void rnet_rb_begin_episode_from(RNetRbSession *s, const RNetRbCorrection *corr,
                                uint32_t initiator_slot);
/* Initiator seat of the current episode; RNET_RB_SLOT_NONE when idle or
 * unknown. */
uint32_t rnet_rb_get_initiator_slot(const RNetRbSession *s);

/*
 * Deterministic lowest-slot arbitration for an inbound RB_SYNC BEGIN that is
 * not flagged REREPLAY (a tip-extend is never a new episode; handle it
 * first). Every seat applies the same rule to the same facts, so all seats
 * converge on the lowest initiating seat without another round trip:
 *
 *   Follow       -- idle (Live, or a finished Commit/Abort): follow it.
 *   SameEpisode  -- it is the episode we already hold (same initiator seat,
 *                   same epoch): a duplicate, not a new episode.
 *   Yield        -- busy, but the sender's seat is LOWER than the current
 *                   episode's initiator (ours or the one we follow): tear
 *                   ours down (no ABORT on the wire -- the sender's BEGIN
 *                   reaches every seat and each one yields for itself) and
 *                   follow the sender.
 *   Refuse       -- busy and the current initiator outranks the sender, the
 *                   current initiator is unknown, or the sender is our own
 *                   seat: reply RB_SYNC NACK so the sender aborts at once
 *                   instead of timing out.
 *
 * At N = 2 this is exactly the rule recomp-net hosts already implemented by
 * hand ("lower initiator slot wins; the loser yields and follows").
 */
typedef enum RNetRbBeginArb
{
    nRNetRbBeginFollow = 0,
    nRNetRbBeginSameEpisode,
    nRNetRbBeginYield,
    nRNetRbBeginRefuse
} RNetRbBeginArb;

RNetRbBeginArb rnet_rb_arbitrate_begin(const RNetRbSession *s, uint32_t sender_slot,
                                       uint32_t epoch_id);

/* Seats whose seal rows / digests / watermark this session must wait on:
 * rnet_expected_peer_mask(slot_count, occupied_mask, local_slot). */
uint32_t rnet_rb_expected_peer_mask(const RNetRbSession *s);
/* Membership change mid-match (a seat left). Same meaning as
 * RNetRbConfig.occupied_mask; re-evaluates the per-peer frontier. */
void rnet_rb_set_occupied_mask(RNetRbSession *s, uint32_t occupied_mask);

/*
 * Tip-extend / edge coalesce: grow target_tick and append seal rows for
 * (old_target, new_target]. Allowed in SealInputs / AwaitingBaseline /
 * Replay / Verify / TipHold. Verify drops to Replay; TipHold stays TipHold
 * (host schedules rereplay only when Live already invented past the tip).
 * Returns 1 on success (including already-at-target).
 *
 * Also refreshes local-authority sealed rows in the new range from
 * get_input_row (host must promote late wire into history first).
 */
uint8_t rnet_rb_extend_target(RNetRbSession *s, uint32_t new_target);

/* 1 if extend_target(new_target) would succeed (sealed, phase OK, span fits
 * seal_max_span and RNET_RB_PEER_SEAL_MASK_BITS). Does not mutate. */
uint8_t rnet_rb_can_extend_target(const RNetRbSession *s, uint32_t new_target);

/* Re-sample sealed rows for one slot from host history over [from, to]
 * inclusive (clamped to the sealed span). Used after promoting late wire
 * for a tick already inside the current target (press sealed, release
 * arrives before tip). */
uint8_t rnet_rb_resign_slot_range(RNetRbSession *s, int32_t slot, uint32_t from_tick,
                                  uint32_t to_tick);

/* Suggested target = max(sim_tip, mismatch) + tip_seal_slack (small;
 * TipHold covers physical release coalesce). */
uint32_t rnet_rb_suggest_target(const RNetRbSession *s, uint32_t mismatch_tick,
                                uint32_t sim_tip);

/* After POST digests match: advance resolved_through, keep seals, enter
 * TipHold so the host can run Live while late edges tip-extend. Returns 1
 * on success. Host calls session_reset (or on_post_match→Commit) when the
 * tip_runway quiet window expires. */
uint8_t rnet_rb_enter_tip_hold(RNetRbSession *s);

/* Stick-replace decision over a published vs authoritative wire row; thin
 * wrapper so hosts can resolve rewind-vs-promote with the shared contract and
 * their gates before queueing an episode. */
RNetInputContractDecision rnet_rb_decide_stick_replace(RNetRbSession *s,
                                                       const RNetInputContractFrame *published,
                                                       const RNetInputContractFrame *wire,
                                                       uint8_t completed_sim);

/* Sealed input table. Local-authority rows seal from the host's input history;
 * peer-authority rows arrive via apply_peer_seal_rows. The sealed table is the
 * sole replay read set. */
/* begin_tick..target_tick inclusive — pass load_tick (not only mismatch) so
 * Replay can publish sealed pads for every resim quantum. Invalid or oversized
 * ranges clear the seal without changing the correction target; check
 * inputs_sealed before replay. Valid calls replace the previous seal. */
void rnet_rb_seal_inputs(RNetRbSession *s, uint32_t begin_tick, uint32_t target_tick,
                         int32_t correction_slot);
uint8_t rnet_rb_inputs_sealed(const RNetRbSession *s);
uint8_t rnet_rb_tick_in_sealed_span(const RNetRbSession *s, uint32_t tick);
uint8_t rnet_rb_get_sealed_frame(const RNetRbSession *s, int32_t slot, uint32_t tick,
                                 RNetRbFrame *out_frame);
/* 1 iff the sealed row for (slot, tick) is safe to sim with: local seat with a
 * valid row, a peer-delivered SEAL_ROWS row, or a wire-confirmed
 * (!is_predicted) locally-sealed row. Predicted remote rows are NOT
 * authoritative — arming them forks the peers when the prediction is wrong. */
uint8_t rnet_rb_seat_row_authoritative(const RNetRbSession *s, int32_t slot, uint32_t tick);
uint32_t rnet_rb_get_seal_span(const RNetRbSession *s);
uint32_t rnet_rb_get_seal_base_tick(const RNetRbSession *s);

/* Peer seal-row exchange (host transports the chunks; library validates tuple
 * compatibility, marks completion, and gates forward replay). */
uint8_t rnet_rb_apply_peer_seal_rows(RNetRbSession *s, uint32_t epoch_id, uint32_t mismatch_tick,
                                     uint32_t target_tick, int32_t slot, uint32_t row_begin,
                                     const RNetRbFrame *rows, uint32_t row_count);
uint8_t rnet_rb_peer_seal_rows_complete(const RNetRbSession *s, int32_t slot);
uint8_t rnet_rb_all_peer_seal_rows_complete(const RNetRbSession *s);
uint8_t rnet_rb_export_seal_rows_chunk(const RNetRbSession *s, int32_t slot, uint32_t row_begin,
                                       uint32_t max_rows, RNetRbFrame *out_frames,
                                       uint32_t *out_row_count);

/* Shared frontier / resolved-through watermark (highest sim tick agreed with
 * peers). Drives live-sim caps and input-contract hash_confirm_promote. */
uint32_t rnet_rb_resolved_through(const RNetRbSession *s);
/* Unattributed advance (max-wins). Correct only with a single peer. */
void rnet_rb_set_peer_convergence(RNetRbSession *s, uint32_t peer_target);
/* N-way advance from an RB_RESOLVED sender (rnet_session_take_rb_resolved_
 * from). Keeps each expected seat's latest advertised frontier and advances
 * resolved_through to the MINIMUM over every expected seat once all have
 * advertised -- the highest tick every seat has proven. Never demotes (use
 * rnet_rb_demote_resolved_through). Returns 1 if the watermark moved; 0 for
 * a seat outside the expected mask (own seat, empty seat, spectator). With
 * one peer this is identical to set_peer_convergence. Per-seat values
 * survive session_reset, like resolved_through itself. */
uint8_t rnet_rb_note_peer_resolved(RNetRbSession *s, uint32_t slot, uint32_t resolved_through);
/* A seat's latest advertised frontier; 0 if it has not advertised. */
uint8_t rnet_rb_peer_resolved(const RNetRbSession *s, uint32_t slot, uint32_t *out_tick);
/* Pull resolved_through down to tick when a follow-NACK / unilateral tip is
 * refused (tick < current). set_peer_convergence only advances — without a
 * demote, session_reset keeps a poisoned frontier and the next light-tip /
 * HC advance reopens the refused load. No-op when tick >= current. */
void rnet_rb_demote_resolved_through(RNetRbSession *s, uint32_t tick);

/* Episode resolution. Host calls on_post_match / on_post_diverge after the
 * post-replay digest comparison; library commits the sealed rows or deepens /
 * aborts. */
void rnet_rb_on_post_match(RNetRbSession *s);
void rnet_rb_on_post_diverge(RNetRbSession *s);
void rnet_rb_commit_promote_sealed(RNetRbSession *s);

/*
 * N-way digest agreement for one episode checkpoint: RB_BASELINE at the load
 * tick, RB_POST at the target tick. At N = 2 the host compared one peer's
 * digest with its own; at N seats it must hear from every expected seat and
 * stop on the first that disagrees. Pure bookkeeping over caller-supplied
 * digest words (e.g. BASELINE master + 3 partitions = 4 words; POST master +
 * input digest = 2), keyed by (epoch, tick).
 *
 *   rnet_rb_agree_begin(&a, rnet_rb_expected_peer_mask(s), epoch, tick, 4);
 *   rnet_rb_agree_set_local(&a, my_words);       (before or after peers)
 *   while (take_rb_baseline_from(...)) rnet_rb_agree_note(&a, sender, ...);
 *   switch (rnet_rb_agree_status(&a, &slot, &word)) { ... }
 *
 * Status: Mismatch as soon as the local words and any reporting seat differ
 * (in any word) -- whether or not the others have reported; Match once every
 * expected seat has reported and all equal local; otherwise Pending. An
 * empty peer mask never matches (fail closed).
 */
#define RNET_RB_AGREE_MAX_WORDS 4u

typedef enum RNetRbAgreeStatus
{
    nRNetRbAgreePending = 0,
    nRNetRbAgreeMatch,
    nRNetRbAgreeMismatch
} RNetRbAgreeStatus;

typedef struct RNetRbPeerAgree
{
    uint32_t epoch_id;
    uint32_t tick;
    uint32_t word_count;
    uint32_t peer_mask;
    uint32_t reported_mask;
    uint8_t local_valid;
    uint32_t local[RNET_RB_AGREE_MAX_WORDS];
    uint32_t peer[RNET_RB_MAX_SLOTS][RNET_RB_AGREE_MAX_WORDS];
} RNetRbPeerAgree;

/* word_count is clamped to 1..RNET_RB_AGREE_MAX_WORDS. */
void rnet_rb_agree_begin(RNetRbPeerAgree *a, uint32_t peer_mask, uint32_t epoch_id,
                         uint32_t tick, uint32_t word_count);
void rnet_rb_agree_set_local(RNetRbPeerAgree *a, const uint32_t *words);
/* 1 if recorded. 0 when (epoch, tick) is not the checkpoint being agreed
 * (stale or future -- the caller decides whether to hold it), or the seat is
 * not expected. A seat reporting again replaces its words (latest wins). */
uint8_t rnet_rb_agree_note(RNetRbPeerAgree *a, uint32_t slot, uint32_t epoch_id,
                           uint32_t tick, const uint32_t *words);
/* Optional outs name the first (lowest) mismatching seat and word index;
 * RNET_RB_SLOT_NONE otherwise. */
RNetRbAgreeStatus rnet_rb_agree_status(const RNetRbPeerAgree *a, uint32_t *mismatch_slot,
                                       uint32_t *mismatch_word);
/* Expected seats that have not reported yet. */
uint32_t rnet_rb_agree_missing_mask(const RNetRbPeerAgree *a);
/* A seat left mid-checkpoint: drop it from the expected set. */
void rnet_rb_agree_set_peer_mask(RNetRbPeerAgree *a, uint32_t peer_mask);

/* Event queue (peer symmetric notices, frame-commit) drained by the host. */
void rnet_rb_enqueue_event(RNetRbSession *s, const RNetRbEvent *event);
uint8_t rnet_rb_drain_next_event(RNetRbSession *s, RNetRbEvent *out_event);
uint8_t rnet_rb_has_pending_events(const RNetRbSession *s);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NET_ROLLBACK_H */
