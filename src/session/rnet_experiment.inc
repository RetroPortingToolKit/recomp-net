/* Included after session_now, before packet dispatch. Echoes use the same
 * ICE/host relay/TURN transport as gameplay; no separate probe route. */
int rnet_session_enable_local_inputs(RNetSession *s) {
    if (!s || s->phase != RNET_PHASE_IDLE || s->profiles.active || s->is_observer) return -1;
    s->local_inputs = 1; return 0;
}
int rnet_session_local_inputs_enabled(const RNetSession *s) { return s && s->local_inputs; }
int rnet_session_set_local_delay(RNetSession *s, rnet_u8 delay) {
    if (!s || !s->local_inputs || delay > 20u) return -1;
    s->delay = delay; s->cfg.input_delay = delay; s->delay_pending = 0;
    return 0;
}
int rnet_session_set_input_transform(RNetSession *s, RNetCanonicalizeFn fn, void *ctx) {
    if (!s || s->phase != RNET_PHASE_IDLE) return -1;
    s->canonicalize = fn; s->canonicalize_ctx = ctx; return 0;
}
static void experiment_canonicalize(RNetSession *s, RNetInputSample *sample) {
    if (s->canonicalize) s->canonicalize(s->canonicalize_ctx, sample);
    if (sample->size > RNET_INPUT_MAX) sample->size = RNET_INPUT_MAX;
}
static void experiment_sample_stamp(RNetSession *s, rnet_u32 tick) {
    rnet_u32 i = tick % RNET_HISTORY_LENGTH;
    s->sample_tick[i] = tick; s->sample_ms[i] = session_now(s); s->sample_valid[i] = 1;
}
int rnet_session_input_age_us(const RNetSession *s, rnet_u32 tick, rnet_u64 *age) {
    rnet_u32 i = tick % RNET_HISTORY_LENGTH;
    rnet_u64 now;
    if (!s || !age || !s->sample_valid[i] || s->sample_tick[i] != tick) return 0;
    now = session_now((RNetSession *)s);
    *age = (now >= s->sample_ms[i] ? now - s->sample_ms[i] : 0u) * 1000u;
    return 1;
}
static void experiment_echo_receive(RNetSession *s, const RNetDecodedPacket *pkt) {
    rnet_u32 seat = pkt->local_slot, seq = pkt->rb_mismatch_tick, i = seq % 32u;
    if (seat >= s->cfg.slot_count || seat == s->cfg.local_slot ||
        !rnet_config_slot_occupied(&s->cfg, (rnet_u8)seat)) return;
    if (pkt->rb_initiator == 9u) {
        rnet_session_send_rb_sync(s, 0u, seq, pkt->rb_load_tick, seat, 0u, 10u, 0u);
    } else if (pkt->rb_target_tick == s->cfg.local_slot &&
               s->echo[i].seq == seq && s->echo[i].sent_ms &&
               !(s->echo[i].seen & (1u << seat))) {
        RNetPeerTelemetry *p = &s->echo_peer[seat];
        rnet_u64 elapsed = session_now(s) - s->echo[i].sent_ms;
        rnet_u32 rtt = elapsed > 0xffffffffu / 1000u ? 0xffffffffu : (rnet_u32)elapsed * 1000u;
        rnet_u32 delta = p->rtt_us > rtt ? p->rtt_us - rtt : rtt - p->rtt_us;
        if (p->received) p->jitter_us = (rnet_u32)(((rnet_u64)p->jitter_us * 7u + delta) / 8u);
        p->rtt_us = rtt; ++p->received; s->echo[i].seen |= 1u << seat;
    }
}
static void experiment_echo_drive(RNetSession *s) {
    rnet_u64 now;
    rnet_u32 i, seat, seq, expected = 0;
    if (s->phase != RNET_PHASE_RUNNING || s->is_observer) return;
    now = session_now(s);
    if (s->echo_last_ms && now - s->echo_last_ms < 250u) return;
    seq = ++s->echo_seq; i = seq % 32u;
    for (seat = 0; seat < s->cfg.slot_count; ++seat) {
        rnet_u32 bit = 1u << seat;
        if ((s->echo[i].expected & bit) && !(s->echo[i].seen & bit)) ++s->echo_peer[seat].lost;
        if (seat != s->cfg.local_slot && rnet_config_slot_occupied(&s->cfg, (rnet_u8)seat)) {
            expected |= bit; ++s->echo_peer[seat].sent;
        }
    }
    s->echo[i].seq = seq; s->echo[i].sent_ms = now ? now : 1u;
    s->echo[i].expected = expected; s->echo[i].seen = 0;
    s->echo_last_ms = now ? now : 1u;
    rnet_session_send_rb_sync(s, 0u, seq, (rnet_u32)now, 0u, 0u, 9u, 0u);
}
int rnet_session_get_peer_telemetry(const RNetSession *s, int seat, RNetPeerTelemetry *out) {
    RNetSessionStats stats;
    if (!s || !out || seat < 0 || seat >= s->cfg.slot_count || seat == s->cfg.local_slot) return 0;
    *out = s->echo_peer[seat];
    rnet_session_get_stats(s, &stats);
    out->path = !strcmp(stats.ice_path, "relay") ? 3u :
        (s->transport.hub_mode || s->transport.mode == RNET_TRANSPORT_ICE_HUB || s->cfg.slot_count > 2u) ? 2u :
        (!strcmp(stats.ice_path, "pending") || !strcmp(stats.ice_path, "unknown")) ? 0u : 1u;
    return 1;
}
