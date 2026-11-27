#include "recomp_net/local_rollback.h"
#include "recomp_net/hash_confirm.h"
#include "rnet_telemetry_internal.h"
#include "platform/rnet_platform.h"
#include <stdlib.h>
#include <string.h>

typedef struct LocalTick {
    rnet_u32 tick, predicted, consumed, digest;
    int valid, complete;
    RNetInputSample rows[RNET_MAX_SLOTS];
} LocalTick;
struct RNetLocalDriver {
    RNetLocalConfig cfg;
    RNetLocalHost host;
    LocalTick history[RNET_LOCAL_HISTORY];
    RNetHashConfirm hashes;
    RNetTelemetry *telemetry;
    RNetTickTelemetry work;
    rnet_u32 tick, confirmed, emitted, replay_next, replay_end, pending, prepared;
    rnet_u32 occupied, peers;
    rnet_u64 poll_at, run_at, last_hash_request;
    int prepared_valid, replaying;
    int hold;
    rnet_u32 hold_at;
    const char *refusal;
};
static rnet_u64 clock_us(RNetLocalDriver *d) {
    return d->host.now_us ? d->host.now_us(d->host.ctx) : rnet_os_monotonic_ms() * 1000u;
}
static int refuse(RNetLocalDriver *d, const char *why) {
    d->refusal = why;
    if (d->replaying) {
        d->host.resim_end(d->host.ctx); d->replaying = 0;
    }
    d->work.stall_reason = RNET_TICK_REFUSED;
    return 0;
}
static LocalTick *history(RNetLocalDriver *d, rnet_u32 tick) {
    LocalTick *h = &d->history[tick % RNET_LOCAL_HISTORY];
    return h->valid && h->tick == tick ? h : NULL;
}
static int equal(const RNetInputSample *a, const RNetInputSample *b) {
    return a->size == b->size && !memcmp(a->bytes, b->bytes, a->size);
}
static int input(RNetLocalDriver *d, int seat, rnet_u32 tick, RNetInputSample *out) {
    int got;
    memset(out, 0, sizeof(*out));
    got = d->cfg.session ? rnet_session_peek_input(d->cfg.session, seat, tick, out)
        : d->host.input(d->host.ctx, seat, tick, out);
    if (!got || !out->valid) return 0;
    /* Session priming uses its protocol-neutral empty sample. The host owns
     * the concrete neutral bytes (e.g. unsigned centered GameCube sticks). */
    if (!out->size) d->host.neutral(d->host.ctx, seat, out);
    if (out->size != d->cfg.row_size) return -1;
    out->tick = tick;
    return 1;
}
static int save(RNetLocalDriver *d, rnet_u32 tick) {
    rnet_u64 begin = clock_us(d);
    int ok = d->host.save(d->host.ctx, tick);
    d->work.snapshot_us += clock_us(d) - begin;
    return ok ? 1 : refuse(d, "snapshot save failed");
}
static int make_tick(RNetLocalDriver *d, rnet_u32 tick, int replay) {
    LocalTick *h = &d->history[tick % RNET_LOCAL_HISTORY];
    int seat;
    if (!replay) {
        memset(h, 0, sizeof(*h)); h->valid = 1; h->tick = tick;
    } else if (!h->valid || h->tick != tick) return refuse(d, "replay history exhausted");
    h->complete = 0; h->predicted = 0;
    for (seat = 0; seat < (int)d->cfg.seats; ++seat) {
        RNetInputSample row;
        int got;
        if (!(d->occupied & (1u << seat))) {
            memset(&row, 0, sizeof(row));
            d->host.neutral(d->host.ctx, seat, &row);
        } else {
            got = input(d, seat, tick, &row);
            if (got < 0) return refuse(d, "input row size differs");
            if (!got) {
                LocalTick *prev = tick ? history(d, tick - 1u) : NULL;
                if ((rnet_u32)seat == d->cfg.local_seat)
                    return refuse(d, "local input missing");
                if (prev) row = prev->rows[seat];
                else { memset(&row, 0, sizeof(row)); d->host.neutral(d->host.ctx, seat, &row); }
                if (d->host.predict) {
                    RNetInputSample previous = row;
                    d->host.predict(d->host.ctx, seat, tick, &previous, &row);
                }
                h->predicted |= 1u << seat;
            }
        }
        if (row.size != d->cfg.row_size) return refuse(d, "predictor or neutral row size differs");
        row.tick = tick; row.valid = 1;
        h->rows[seat] = row;
    }
    if (!save(d, tick)) return 0;
    d->host.publish(d->host.ctx, tick, h->rows, (int)d->cfg.seats, replay);
    if (!replay) d->work.guessed_seats = h->predicted;
    d->run_at = clock_us(d);
    d->pending = replay ? 2u : 1u;
    d->poll_at = 0;
    return (int)d->pending;
}
/* Scan the entire unconfirmed prefix before selecting a load. The load is the
 * OLD confirmed frontier, never a snapshot that contains an unresolved input.
 * Equal late rows promote without replay; unread rows may promote only because
 * the host explicitly attests that the guest did not consume them. */
static int reconcile(RNetLocalDriver *d) {
    rnet_u32 t, old = d->confirmed, wrong = 0, next = old;
    int have_wrong = 0, prefix = 1;
    for (t = old; t < d->tick; ++t) {
        LocalTick *h = history(d, t);
        int seat;
        if (!h || !h->complete) return refuse(d, "unconfirmed input history exhausted");
        for (seat = 0; seat < (int)d->cfg.seats; ++seat) {
            RNetInputSample row; int got;
            if (!(h->predicted & (1u << seat))) continue;
            got = input(d, seat, t, &row);
            if (got < 0) return refuse(d, "late input row size differs");
            if (!got) continue;
            if (!equal(&h->rows[seat], &row) && (h->consumed & (1u << seat))) {
                if (!have_wrong) { wrong = t; have_wrong = 1; }
                ++d->work.mispredicts;
            }
            h->rows[seat] = row;
            h->predicted &= ~(1u << seat);
        }
        if (h->predicted) prefix = 0;
        if (prefix) next = t + 1u;
    }
    if (have_wrong) {
        rnet_u64 begin = clock_us(d);
        /* Retain the unresolved anchor even when a later mismatch arrives first. */
        if (!d->host.load(d->host.ctx, old)) return refuse(d, "confirmed snapshot unavailable");
        d->work.snapshot_us += clock_us(d) - begin;
        d->host.drop_after(d->host.ctx, old);
        d->replay_next = old; d->replay_end = d->tick;
        d->work.episode_depth = d->tick - wrong;
        d->replaying = 1; d->host.resim_begin(d->host.ctx);
        return 1;
    }
    d->confirmed = next;
    return 1;
}
static int hashes(RNetLocalDriver *d) {
    RNetSession *s = d->cfg.session;
    rnet_u32 t, digest, epoch, mismatch, load, target;
    rnet_u8 initiator, op, flags;
    rnet_u64 now;
    if (!s) return 1;
    /* Publish only immutable confirmed digests. A speculative digest never
     * reaches the peer, so a genuine mismatch needs no settle timer. */
    while (d->emitted < d->confirmed) {
        LocalTick *h = history(d, d->emitted);
        if (!h) return refuse(d, "confirmed digest history exhausted");
        rnet_hc_note_local(&d->hashes, h->tick, h->digest);
        rnet_session_send_rb_frame_commit(s, h->tick, h->digest);
        ++d->emitted;
    }
    while (rnet_session_take_rb_frame_commit(s, &t, &digest)) {
        if (t >= d->tick + RNET_LOCAL_HISTORY) return refuse(d, "invalid future confirmed digest");
        rnet_hc_note_peer_from(&d->hashes, rnet_session_rb_last_take_from(s), t, digest);
    }
    while (rnet_session_take_rb_sync(s, &epoch, &mismatch, &load, &target, &initiator, &op, &flags)) {
        if (op == RNET_RB_SYNC_OP_FRAME_COMMIT_REQUEST &&
            rnet_hc_local_digest(&d->hashes, mismatch, &digest))
            rnet_session_send_rb_frame_commit(s, mismatch, digest);
    }
    if (rnet_hc_peek_mismatch(&d->hashes, &t, NULL, NULL))
        return refuse(d, "confirmed state desync");
    t = d->hashes.resolved_valid ? rnet_hc_resolved_through(&d->hashes) + 1u : 0u;
    if (d->confirmed - t >= RNET_HC_RING)
        return refuse(d, "background verification history exhausted");
    now = clock_us(d);
    if (now - d->last_hash_request >= 100000u) {
        t = d->hashes.resolved_valid ? rnet_hc_resolved_through(&d->hashes) + 1u : 0u;
        if (t < d->confirmed)
            rnet_session_send_rb_sync(s, 0u, t, 0u, 0u, 0u,
                                      RNET_RB_SYNC_OP_FRAME_COMMIT_REQUEST, 0u);
        d->last_hash_request = now;
    }
    return 1;
}
int rnet_experiment_profiles_begin(RNetSession *s, rnet_u32 qualification,
                                   const RNetProfileCaps *caps, RNetSyncMode mode) {
    if (!s || !qualification || !caps || !(caps->profiles & 1u) ||
        caps->architecture > 255u || rnet_session_sim_tick(s)) return -1;
    if (mode != RNET_SYNC_EPISODE_V3 && mode != RNET_SYNC_LOCAL_V4) return -1;
    if (mode == RNET_SYNC_EPISODE_V3 && rnet_session_local_inputs_enabled(s)) return -1;
    if (mode == RNET_SYNC_LOCAL_V4 && rnet_session_enable_local_inputs(s)) return -1;
    qualification = (qualification ^ (0x4c523400u | (rnet_u32)mode)) * 16777619u;
    return rnet_session_profiles_begin(s, qualification ? qualification : 1u, caps);
}
RNetLocalDriver *rnet_local_create(const RNetLocalConfig *cfg, const RNetLocalHost *host) {
    RNetLocalDriver *d;
    RNetSessionStats stats;
    if (!cfg || !host || cfg->seats < 2u || cfg->seats > RNET_MAX_SLOTS ||
        cfg->local_seat >= cfg->seats || !cfg->row_size || cfg->row_size > RNET_INPUT_MAX ||
        !cfg->tick_us || !cfg->rollback_depth || cfg->rollback_depth > 64u || cfg->delay > 20u ||
        !host->save || !host->load || !host->drop_after || !host->publish ||
        !host->resim_begin || !host->resim_end || !host->digest || !host->neutral ||
        (!cfg->session && (!host->input || !host->sample)) ||
        (cfg->session && !rnet_session_local_inputs_enabled(cfg->session))) return NULL;
    if (cfg->session) {
        rnet_session_get_stats(cfg->session, &stats);
        if (stats.slot_count != cfg->seats || stats.local_slot != cfg->local_seat || stats.sim_tick) return NULL;
    }
    d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->cfg = *cfg; d->host = *host;
    d->occupied = cfg->occupied_mask ? cfg->occupied_mask : (1u << cfg->seats) - 1u;
    if (!(d->occupied & (1u << cfg->local_seat)) || (d->occupied >> cfg->seats)) { free(d); return NULL; }
    d->peers = d->occupied & ~(1u << cfg->local_seat);
    if (!d->peers) { free(d); return NULL; }
    rnet_hc_reset(&d->hashes); rnet_hc_set_peer_mask(&d->hashes, d->peers);
    d->telemetry = rnet_telemetry_create(RNET_SYNC_LOCAL_V4, cfg->tick_us);
    if (!d->telemetry) { free(d); return NULL; }
    if (cfg->session && rnet_session_set_local_delay(cfg->session, (rnet_u8)cfg->delay)) {
        rnet_local_destroy(d); return NULL;
    }
    return d;
}
void rnet_local_destroy(RNetLocalDriver *d) {
    if (!d) return;
    if (d->replaying) d->host.resim_end(d->host.ctx);
    rnet_telemetry_destroy(d->telemetry); free(d);
}
int rnet_local_poll(RNetLocalDriver *d) {
    rnet_u64 now;
    if (!d || d->refusal) return 0;
    if (d->pending) return refuse(d, "finish required before next poll");
    now = clock_us(d);
    if (d->poll_at) d->work.stall_us += now - d->poll_at;
    d->poll_at = now;
    if (d->cfg.session) {
        int status;
        RNetSessionStats stats;
        rnet_session_pump(d->cfg.session);
        rnet_session_get_stats(d->cfg.session, &stats);
        if (stats.peer_gone) return refuse(d, "peer left the match");
        status = rnet_session_profiles_status(d->cfg.session, NULL, NULL);
        if (status < 0 || rnet_session_peer_wire_version_mismatch(d->cfg.session))
            return refuse(d, "experimental startup or wire mismatch");
        if (!status || !rnet_session_is_running(d->cfg.session)) {
            d->work.stall_reason = RNET_TICK_STARTUP; return 0;
        }
        if (stats.state_busy) { d->work.stall_reason = RNET_TICK_INPUT; return 0; }
    }
    if (!d->replaying) {
        if (!reconcile(d) || !hashes(d)) return 0;
    }
    if (d->replaying) {
        do {
            int admitted = make_tick(d, d->replay_next, 1);
            if (!admitted || !d->host.run_tick) return admitted;
            if (!d->host.run_tick(d->host.ctx, d->replay_next)) return refuse(d, "replay tick failed");
            if (!rnet_local_finish(d)) return 0;
        } while (d->replaying);
        /* Reconcile the replay before publishing any digest from it. */
        if (!reconcile(d) || !hashes(d)) return 0;
        if (d->replaying) return 0;
    }
    if (d->hold && d->tick >= d->hold_at) {
        d->poll_at = 0; /* an intentional pause is not network stall time */
        return 0;
    }
    if (!d->prepared_valid || d->prepared != d->tick) {
        int ok = d->cfg.session ? rnet_session_prepare_local_tip(d->cfg.session, d->tick)
            : d->host.sample(d->host.ctx, d->tick, d->cfg.delay);
        if (!ok) { d->work.stall_reason = RNET_TICK_INPUT; return 0; }
        d->prepared = d->tick; d->prepared_valid = 1;
    }
    if (d->tick - d->confirmed >= d->cfg.rollback_depth) {
        d->work.stall_reason = RNET_TICK_PREDICTION_LIMIT; return 0;
    }
    return make_tick(d, d->tick, 0);
}
int rnet_local_finish(RNetLocalDriver *d) {
    LocalTick *h; rnet_u64 now, begin; int replay;
    if (!d || !d->pending || d->refusal) return 0;
    replay = d->pending == 2u;
    h = history(d, replay ? d->replay_next : d->tick);
    if (!h) return refuse(d, "completed tick history unavailable");
    now = clock_us(d); begin = now;
    h->consumed = d->host.consumed_seats ? d->host.consumed_seats(d->host.ctx, h->tick) : d->occupied;
    h->digest = d->host.digest(d->host.ctx); h->complete = 1;
    d->work.digest_us += clock_us(d) - begin;
    d->pending = 0;
    if (replay) {
        ++d->work.replay_ticks; d->work.replay_us += now - d->run_at;
        ++d->replay_next;
        if (d->replay_next == d->replay_end) {
            d->host.resim_end(d->host.ctx); d->replaying = 0;
        }
    } else {
        d->work.tick = d->tick; d->work.at_us = now; d->work.delay = d->cfg.delay;
        d->work.prediction_depth = d->tick - d->confirmed;
        if (d->cfg.session) {
            int seat;
            for (seat = 0; seat < (int)d->cfg.seats; ++seat) {
                RNetPeerTelemetry peer;
                if (rnet_session_get_peer_telemetry(d->cfg.session, seat, &peer))
                    rnet_telemetry_peer(d->telemetry, seat, &peer);
            }
            d->work.input_latency_valid = rnet_session_input_age_us(d->cfg.session, d->tick,
                                                                    &d->work.input_latency_us);
        }
        rnet_telemetry_record(d->telemetry, &d->work);
        memset(&d->work, 0, sizeof(d->work));
        ++d->tick;
        if (d->cfg.session) rnet_session_advance(d->cfg.session);
    }
    /* Time in the simulation itself is not a stall. */
    d->poll_at = 0;
    return 1;
}
rnet_u32 rnet_local_tick(const RNetLocalDriver *d) { return d ? d->tick : 0u; }
rnet_u32 rnet_local_confirmed(const RNetLocalDriver *d) { return d ? d->confirmed : 0u; }
rnet_u32 rnet_local_verified(const RNetLocalDriver *d) {
    return d && d->hashes.resolved_valid ? rnet_hc_resolved_through(&d->hashes) + 1u : 0u;
}
int rnet_local_set_hold(RNetLocalDriver *d, rnet_u32 tick) {
    if (!d || d->pending || tick < d->tick) return 0;
    d->hold = 1; d->hold_at = tick; return 1;
}
void rnet_local_clear_hold(RNetLocalDriver *d) { if (d) d->hold = 0; }
const char *rnet_local_refusal(const RNetLocalDriver *d) { return d ? d->refusal : "no local driver"; }
const RNetTelemetry *rnet_local_telemetry(const RNetLocalDriver *d) { return d ? d->telemetry : NULL; }
size_t rnet_local_report(const RNetLocalDriver *d, char *out, size_t capacity) {
    return d ? rnet_telemetry_report_extra(d->telemetry, &d->work, out, capacity) : 0;
}
rnet_u32 rnet_local_budget_delay(rnet_u32 transit, rnet_u32 jitter, rnet_u32 replay,
    rnet_u32 live, rnet_u32 budget, rnet_u32 max_depth, rnet_u32 *depth) {
    rnet_u32 affordable = replay && budget > live ? (budget - live) / replay : 0u;
    rnet_u64 needed;
    if (affordable > max_depth) affordable = max_depth;
    if (depth) *depth = affordable;
    if (!budget) return 20u;
    needed = ((rnet_u64)transit + jitter + budget - 1u) / budget;
    needed = needed > affordable ? needed - affordable : 1u;
    return needed > 20u ? 20u : (rnet_u32)needed;
}
