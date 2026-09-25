#ifndef RECOMP_NET_RB_DRIVER_H
#define RECOMP_NET_RB_DRIVER_H

/*
 * The rollback EPISODE DRIVER -- the code that actually runs an episode.
 *
 * RNetRbSession (rollback.h) is the passive half: it stores the phase, the
 * correction tuple, the sealed rows and the resolved-through watermark, and
 * the only host callback it ever invokes is get_input_row. Everything that
 * decides WHEN to rewind, talks to the peer about it, replays, verifies and
 * recovers lived in each engine's host until this driver was lifted out of
 * snesrecomp's snes_netplay_rb.c (2026-09-24). An engine now supplies only
 * what is genuinely its own -- snapshots, one tick of simulation, digests,
 * pad layout and presentation suppression -- through RNetRbHost, and inherits
 * every fix to the rest.
 *
 * What the driver owns:
 *   - live admit: local tip, remote rows, hold-last invent, the admission
 *     scheduler (sched.h) and every gate it asks for;
 *   - reconcile: late wire against predicted history -> open an episode,
 *     tip-extend, or log the loss (never silently);
 *   - the episode FSM: open / follow / dual-initiation arbitration / NACK,
 *     seal-row exchange, baseline digest gate, replay, POST verify, commit,
 *     tip-hold and tip-extend, abort classes and mirrored cooldowns, a
 *     watchdog on every stage that waits on a peer;
 *   - snapshot policy: interval, floor search, the baseline fork cap;
 *   - the hash chain (FRAME_COMMIT), the boot-digest gate, the mod-set and
 *     identity handshakes, lockstep degrade, the advisory chain-fork report;
 *   - cold reset on start: the struct is wiped and what survives is named.
 *
 * What the host owns (RNetRbHost): snapshot storage, publishing a tick's rows
 * and running one tick, digests, pad decode / sanitize / neutral per seat,
 * resim begin/end, boot diagnostics, a log sink and a clock.
 *
 * Two replay shapes, chosen by RNetRbDriverConfig.replay_mode:
 *
 *   INLINE       the driver loads the baseline and runs every replayed tick
 *                inside one poll_admit call through host->run_tick. Right when
 *                a tick is cheap and returns (SNES RtlRunFrame).
 *   INCREMENTAL  poll_admit publishes ONE replayed tick and returns
 *                RNET_RB_ADMIT_REPLAY; the host runs it with the same per-tick
 *                function it uses for Live, then calls finish_frame, then may
 *                pump / present / service audio before polling again. Right
 *                when a tick is expensive or must run in the host's own loop
 *                (an N64 field). Episode wire is not drained mid-replay in
 *                either shape, so the two are behaviourally identical.
 *
 * Seats: 1..RNET_RB_MAX_SLOTS. Epoch ids carry the initiator's seat in their
 * low RNET_RB_EPOCH_SLOT_BITS bits, so dual initiation is arbitrated by seat
 * (lower wins) without a wire change, and BASELINE / POST / COMMIT agreement
 * is tracked per peer (rnet_session_rb_last_take_from). With two seats every
 * behaviour is the one snesrecomp shipped. More than two peers is BUILT BUT
 * UNEXERCISED: no multi-peer harness exists yet (see docs/rollback.md).
 *
 * The admission scheduler is process-global (rnet_sched_bind), so one driver
 * may be started per process at a time.
 *
 * Single-threaded, like the session: every call on the host's sim thread.
 */

#include <stdint.h>

#include "recomp_net/input.h"
#include "recomp_net/rollback.h"
#include "recomp_net/session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Epoch ids: (sequence << RNET_RB_EPOCH_SLOT_BITS) | initiator seat. */
#define RNET_RB_EPOCH_SLOT_BITS 3u
#define RNET_RB_EPOCH_SLOT_MASK ((1u << RNET_RB_EPOCH_SLOT_BITS) - 1u)
static inline uint32_t rnet_rb_epoch_make(uint32_t seq, uint32_t initiator_slot)
{
    return (seq << RNET_RB_EPOCH_SLOT_BITS) | (initiator_slot & RNET_RB_EPOCH_SLOT_MASK);
}
static inline uint32_t rnet_rb_epoch_initiator(uint32_t epoch)
{
    return epoch & RNET_RB_EPOCH_SLOT_MASK;
}

typedef enum RNetRbReplayMode
{
    RNET_RB_REPLAY_INLINE = 0,
    RNET_RB_REPLAY_INCREMENTAL = 1
} RNetRbReplayMode;

typedef enum RNetRbAdmit
{
    /* Nothing to run this iteration: present the held frame, pump, retry. */
    RNET_RB_ADMIT_STALL = 0,
    /* Rows for the live tick were published: run one tick, then finish_frame. */
    RNET_RB_ADMIT_LIVE = 1,
    /* INCREMENTAL only: rows for one REPLAYED tick were published (inside
     * resim_begin/resim_end). Run it exactly as a live tick, then
     * finish_frame. Presentation is the host's to suppress. */
    RNET_RB_ADMIT_REPLAY = 2
} RNetRbAdmit;

/* Master digest plus the three partitions the BASELINE wire carries. A
 * baseline fork names the first partition that differs (part_names). */
typedef struct RNetRbDigestParts
{
    uint32_t master;
    uint32_t part[3];
} RNetRbDigestParts;

/*
 * Engine-specific pieces. REQUIRED unless marked optional; start() refuses,
 * naming the missing callback, when one is absent.
 */
typedef struct RNetRbHost
{
    void *ctx;

    /* ── snapshots (the host owns storage; recomp-net never sees bytes) ──
     * A snapshot keyed T is the state BEFORE tick T runs, so loading T and
     * replaying T..target re-runs the mismatch tick itself. */
    int (*snap_save)(void *ctx, uint32_t tick);
    int (*snap_load)(void *ctx, uint32_t tick);
    int (*snap_has)(void *ctx, uint32_t tick);
    /* 1 and *oldest set when the store holds anything; 0 when empty. */
    int (*snap_oldest)(void *ctx, uint32_t *oldest);
    /* Drop every snapshot keyed after `tick` (a replay re-keyed the timeline). */
    void (*snap_drop_after)(void *ctx, uint32_t tick);

    /* ── one tick ──
     * publish: the rows every seat simulates at `tick` (sanitized). Called
     * for every live tick from poll_admit, and for every replayed tick. */
    void (*publish)(void *ctx, uint32_t tick, const RNetRbFrame *rows, int slots, int replay);
    /* INLINE mode only (required there, ignored otherwise): run one tick of
     * simulation on the rows last published. Return 0 on failure. */
    int (*run_tick)(void *ctx, uint32_t tick);
    /* Bracket a replay: suppress presentation / rewind audio production so
     * ticks the player already saw and heard are not repeated. */
    void (*resim_begin)(void *ctx);
    void (*resim_end)(void *ctx);

    /* ── digests (simulation state only; identical across peers) ── */
    uint32_t (*digest_master)(void *ctx);
    void (*digest_parts)(void *ctx, RNetRbDigestParts *out);

    /* ── pads ──
     * decode: fill buttons / sticks / analog of `out` from an input sample.
     * tick, is_valid (1) and is_predicted (0) are already set. */
    void (*decode_sample)(void *ctx, int slot, const RNetInputSample *in, RNetRbFrame *out);
    /* Optional: force a row into the engine's legal domain (mask bits, clear
     * sticks on a digital pad, map a foreign neutral). NULL = rows as-is. */
    void (*sanitize_row)(void *ctx, int slot, RNetRbFrame *row);
    /* The row a seat holds when nothing is pressed. Active-high pads (SNES,
     * N64) and active-low pads (PSX) differ; the driver never assumes. */
    void (*neutral_row)(void *ctx, int slot, RNetRbFrame *out);
    /* Optional: the sample a LIVE admit took a seat's row from, for side data
     * that rides the pad bytes (SNES sync bytes). Never called from replay,
     * seal or reconcile. */
    void (*admit_sample)(void *ctx, int slot, uint32_t tick, const RNetInputSample *in);

    /* ── session control ── */
    /* Optional: tick 0's digest was just latched -- log whatever explains a
     * boot mismatch (partitions, frame counters). */
    void (*boot_digest_noted)(void *ctx);
    /* End the match and go back to the lobby (boot fork, mod refusal). */
    void (*request_return_to_lobby)(void *ctx);

    /* Optional log sink: one complete line (with '\n') per call. NULL writes
     * to stderr. The wording is parsed by harnesses (rb_loopback.sh ledger);
     * change a line only together with every script that reads it. */
    void (*log)(void *ctx, const char *line);
    /* Monotonic milliseconds. */
    uint32_t (*now_ms)(void *ctx);
} RNetRbHost;

typedef int (*RNetRbModSetCheckFn)(const char *want, char *reason, uint32_t cap);
typedef int (*RNetRbModSetAdoptFn)(const char *want, char *reason, uint32_t cap);

typedef struct RNetRbDriverConfig
{
    /* Live pointers into the host's session state, exactly as the admission
     * scheduler takes them (sched.h RNetSchedBridge). session is required. */
    RNetSession **session;
    int *local_slot;       /* NULL -> 0 */
    int *slot_count;       /* NULL -> 2 */
    int *input_delay;      /* NULL -> 2 */
    /* Session-settled prediction cap P (recomp-ui: P = 4 + D). NULL or < 2 ->
     * the same rule applied locally, clamped 6..16. Env overrides. */
    int *input_prediction;
    int force_turn;
    /* Seats that take part (bit i = seat i has a peer). 0 = every seat in
     * [0, slot_count). A sparse room must clear empty seats, or every episode
     * waits on a POST nobody will send. */
    uint32_t occupied_mask;

    RNetRbReplayMode replay_mode;
    /* Names of RNetRbDigestParts.part[0..2], for fork reports. */
    const char *part_names[3];
    /* Reporting only (the host owns the store): ring depth in slots, printed
     * in the start banner as reach = depth x snap interval. */
    uint32_t snap_depth;

    /* Tagged log lines read "<log_prefix>: ...". NULL -> "rnet_rb". */
    const char *log_prefix;
    /* Environment knobs are read as RNET_RB_<NAME>; when env_alias is set,
     * "<env_alias>_<NAME>" is consulted FIRST, so an engine's existing names
     * (SNES_RB_*) keep working and a harness that pins "<alias>_X=0" for one
     * peer cannot be overridden by a generic name leaking from the parent.
     * NAME: PREDICTION SNAP_INTERVAL EPISODE_TIMEOUT_MS TIP_RUNWAY
     * LOCKSTEP LOCKSTEP_TICKS FORCE_FORK FORCE_MISPREDICT FORCE_BOOT_FORK
     * FORCE_MOD_MISMATCH FORCE_MODSET ALLOW_BOOT_FORK ALLOW_MOD_MISMATCH. */
    const char *env_alias;
} RNetRbDriverConfig;

typedef struct RNetRbDriver RNetRbDriver;

RNetRbDriver *rnet_rb_driver_create(void);
void rnet_rb_driver_destroy(RNetRbDriver *d);

/* Identity and mod set are properties of the PROCESS, not of a match: they
 * survive start()'s cold reset. Set them before start(). Fingerprints are
 * opaque and compared for equality; 0 = not supplied. */
void rnet_rb_driver_set_identity(RNetRbDriver *d, uint32_t build_fp, uint32_t content_fp);
void rnet_rb_driver_set_modset(RNetRbDriver *d, const char *text, RNetRbModSetCheckFn check,
                               RNetRbModSetAdoptFn adopt);

/*
 * Start a match. Cold reset: everything from a previous match is wiped except
 * identity and mod set, then cfg/host are taken, the session and snapshot
 * policy built, the admission scheduler bound and a neutral row seeded per
 * seat. Returns 1 on success; 0 with a logged reason otherwise.
 */
int rnet_rb_driver_start(RNetRbDriver *d, const RNetRbDriverConfig *cfg, const RNetRbHost *host);
void rnet_rb_driver_shutdown(RNetRbDriver *d);

/*
 * The host loop, both shapes:
 *
 *   for (;;) {
 *       RNetRbAdmit a = rnet_rb_driver_poll_admit(d);
 *       if (a != RNET_RB_ADMIT_STALL) {
 *           run_one_tick();                 // the SAME function for both
 *           rnet_rb_driver_finish_frame(d);
 *       }
 *       present_and_pump();                 // held frame during replay
 *   }
 */
RNetRbAdmit rnet_rb_driver_poll_admit(RNetRbDriver *d);
void rnet_rb_driver_finish_frame(RNetRbDriver *d);

/* Diagnostics. */
uint32_t rnet_rb_driver_sim_tick(const RNetRbDriver *d);
uint32_t rnet_rb_driver_episode_count(const RNetRbDriver *d);
uint32_t rnet_rb_driver_invent_count(const RNetRbDriver *d);
uint32_t rnet_rb_driver_promote_count(const RNetRbDriver *d);
uint64_t rnet_rb_driver_resim_ticks(const RNetRbDriver *d);
uint32_t rnet_rb_driver_desync_count(const RNetRbDriver *d);
/* POST-handshake RTT EMA in ms; 0 until an episode has round-tripped. */
uint32_t rnet_rb_driver_rtt_estimate_ms(const RNetRbDriver *d);
/* Digest-agreed watermark, and how far the local sim runs past it. */
uint32_t rnet_rb_driver_confirmed_through(const RNetRbDriver *d);
uint32_t rnet_rb_driver_confirmed_remaining(const RNetRbDriver *d);
int rnet_rb_driver_episode_active(const RNetRbDriver *d);
/* 1 between resim_begin and resim_end. */
int rnet_rb_driver_in_resim(const RNetRbDriver *d);
const char *rnet_rb_driver_stall_tag(const RNetRbDriver *d);
/* Last digest fork: tick + partition name, and both peers' master digests. */
int rnet_rb_driver_last_fork(const RNetRbDriver *d, uint32_t *tick, const char **partition);
int rnet_rb_driver_fork_digests(const RNetRbDriver *d, uint32_t *mine, uint32_t *theirs);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NET_RB_DRIVER_H */
