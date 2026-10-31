/* Exercise the actual wire NACK handler, including teardown, with a short
 * snapshot window. Other driver dependencies come from the normal library. */
#define rnet_session_peek_remote_input test_peek_remote_input
#include "../src/rollback/rnet_rb_driver.c"
static unsigned wire_button;

/* Replace only the transport read. The actual reconciliation and prefix
 * guard run below, with a real rollback session and input history. */
int test_peek_remote_input(const RNetSession *s, int slot, rnet_u32 wire,
                          RNetInputSample *out)
{
    (void)s;
    if (slot != 1 || wire != 233u) return 0;
    memset(out, 0, sizeof(*out));
    out->valid = 1;
    out->bytes[0] = (rnet_u8)wire_button;
    return 1;
}

static int inert_tick(void *ctx, uint32_t tick) { (void)ctx; (void)tick; return 1; }
static uint8_t no_row(void *ctx, int32_t slot, uint32_t tick, RNetRbFrame *out)
{ (void)ctx; (void)slot; (void)tick; (void)out; return 0; }
static void decode_idle(void *ctx, int slot, const RNetInputSample *in, RNetRbFrame *out)
{ (void)ctx; (void)slot; out->buttons=in->bytes[0]; }

static int check_confirmed_prediction(void)
{
    RNetRbDriver *d = calloc(1, sizeof(*d));
    RNetRbConfig cfg = {0};
    RNetRollbackVTable vt = {0};
    RNetRbFrame row = {0};
    RNetSession *wire = (RNetSession *)d; /* never dereferenced by transport stub */
    uint32_t tick;
    int slot;
    if (!d) return 1;
    cfg.slot_count = 2;
    vt.load_state = inert_tick;
    vt.advance_sim = inert_tick;
    vt.get_input_row = no_row;
    d->rb = rnet_rb_create(&cfg, &vt);
    if (!d->rb) { free(d); return 1; }
    d->cfg.session = &wire;
    d->host.decode_sample = decode_idle;
    d->sim = 270u;
    rb_owed_reset(d);
    rnet_ih_reset(&d->ih, 2);
    row.tick = 233u;
    row.is_valid = row.is_predicted = 1;
    rnet_ih_put(&d->ih, 1, &row);
    /* Keep a genuinely unavailable prediction to prove the guard survives. */
    row.tick = 269u;
    rnet_ih_put(&d->ih, 1, &row);
    rnet_rb_set_peer_convergence(d->rb, 250u);
    rb_reconcile_wire(d);
    if (!rnet_ih_get(&d->ih, 1, 233u, &row) || row.is_predicted ||
        !rb_unsettled_before(d, 270u, &tick, &slot) || tick != 269u || slot != 1) {
        fprintf(stderr, "confirmed prediction was not reconciled against owner wire\n");
        rnet_rb_destroy(d->rb); free(d); return 1;
    }
    rnet_rb_destroy(d->rb); free(d); return 0;
}

static uint32_t now_ms(void *ctx) { (void)ctx; return 100u; }
static void quiet(void *ctx, const char *line) { (void)ctx; (void)line; }
static uint32_t oldest;
static int snap_oldest(void *ctx, uint32_t *out) { (void)ctx; *out = oldest; return 1; }
static int snap_has(void *ctx, uint32_t tick) { (void)ctx; return tick >= oldest && tick <= 136u; }

typedef struct ShortStore {
    uint32_t key[40], state[40], live, saves;
    unsigned char valid[40];
} ShortStore;
static int short_save(void *ctx, uint32_t tick) {
    ShortStore *s=ctx; unsigned i=tick%40u;
    s->key[i]=tick; s->state[i]=s->live; s->valid[i]=1; s->saves++;
    return 1;
}
static int short_has(void *ctx, uint32_t tick) {
    ShortStore *s=ctx; unsigned i=tick%40u;
    return s->valid[i] && s->key[i]==tick;
}
static int short_load(void *ctx, uint32_t tick) {
    ShortStore *s=ctx;
    if (!short_has(ctx,tick)) return 0;
    s->live=s->state[tick%40u]; return 1;
}
static int short_oldest(void *ctx, uint32_t *out) {
    ShortStore *s=ctx; unsigned i; uint32_t first=UINT32_MAX;
    for(i=0;i<40u;i++) if(s->valid[i] && s->key[i]<first) first=s->key[i];
    if(first==UINT32_MAX) return 0;
    *out=first; return 1;
}
static void resim_noop(void *ctx) { (void)ctx; }
static void init_short(ShortStore *s) {
    uint32_t t; memset(s,0,sizeof(*s));
    for(t=233u;t<273u;t++) { s->live=t; short_save(s,t); }
    s->live=273u; s->saves=0;
}

static void sample_noop(rnet_u32 tick, RNetInputSample *out, void *ctx)
{ (void)tick; (void)out; (void)ctx; }
static void publish_noop(rnet_u32 tick, const RNetInputSample *rows, int slots, void *ctx)
{ (void)tick; (void)rows; (void)slots; (void)ctx; }

static int check_begin_before_reconcile(unsigned different)
{
    RNetRbDriver *d=calloc(1,sizeof(*d));
    ShortStore store;
    RNetRbConfig cfg={0}; RNetRollbackVTable vt={0};
    RNetConfig netcfg={0}; RNetHostVTable nethost={0};
    RNetSession *session; RNetRbFrame row={0};
    uint32_t expected=different ? 233u : 269u;
    if(!d) return 1;
    netcfg.slot_count=2;
    nethost.sample_local=sample_noop; nethost.publish=publish_noop;
    session=rnet_session_create(&netcfg,&nethost);
    if(!session) { free(d); return 1; }
    cfg.slot_count=2;
    vt.load_state=inert_tick; vt.advance_sim=inert_tick; vt.get_input_row=no_row;
    d->rb=rnet_rb_create(&cfg,&vt);
    if(!d->rb) { rnet_session_destroy(session); free(d); return 1; }
    d->cfg.session=&session; d->sim=273u;
    d->host.ctx=&store; d->host.now_ms=now_ms; d->host.log=quiet;
    d->host.snap_save=short_save; d->host.snap_has=short_has;
    d->host.snap_oldest=short_oldest; d->host.decode_sample=decode_idle;
    rb_owed_reset(d); rnet_ih_reset(&d->ih,2); init_short(&store);
    row.tick=233u; row.is_valid=row.is_predicted=1;
    rnet_ih_put(&d->ih,1,&row);
    row.tick=269u; rnet_ih_put(&d->ih,1,&row);
    rnet_rb_set_peer_convergence(d->rb,250u);
    wire_button=different;
    /* Actual follower BEGIN before reconciliation, matching wire available
     * in this poll. Equal233 must not request the now-evicted233 baseline;
     * unavailable269 (and genuinely different233) must remain guarded. */
    if(rb_begin_episode(d,270u,1,0,270u,270u,25u,0u) || d->episode_count ||
       d->nacked_n!=1u || d->nacked[0].frontier!=expected ||
       d->nacked[0].flags!=RNET_RB_SYNC_FLAG_PREFIX ||
       !rnet_ih_get(&d->ih,1,233u,&row) || row.is_predicted!=(different!=0u)) {
        fprintf(stderr,"BEGIN before reconciliation misclassified prefix (different=%u)\n",different);
        rnet_rb_destroy(d->rb); rnet_session_destroy(session); free(d); wire_button=0; return 1;
    }
    rnet_rb_destroy(d->rb); rnet_session_destroy(session); free(d); wire_button=0; return 0;
}

static int check_tip_reservation(void)
{
    RNetRbDriver *d=calloc(1,sizeof(*d));
    ShortStore store;
    RNetRbConfig cfg={0}; RNetRollbackVTable vt={0};
    uint32_t load;
    if(!d) return 1;
    cfg.slot_count=2;
    vt.load_state=inert_tick; vt.advance_sim=inert_tick; vt.get_input_row=no_row;
    d->rb=rnet_rb_create(&cfg,&vt);
    if(!d->rb) { free(d); return 1; }
    d->host.ctx=&store; d->host.now_ms=now_ms; d->host.log=quiet;
    d->host.snap_save=short_save; d->host.snap_has=short_has;
    d->host.snap_load=short_load; d->host.snap_oldest=short_oldest;
    d->host.resim_begin=d->host.resim_end=resim_noop;
    d->cfg.replay_mode=RNET_RB_REPLAY_INCREMENTAL;
    d->sim=273u; rb_owed_reset(d); rnet_ih_reset(&d->ih,2);
    init_short(&store);
    /* A follower must reject 233 before opening: reserving the abort tip
     * evicts it from this exact forty-slot store. */
    if(rb_begin_episode(d,270u,1,0,233u,270u,9u,0u) ||
       d->episode_count || store.saves!=1u || short_has(&store,233u)) goto fail;
    if(!rb_snap_floor(d,234u,&load) || load!=234u) goto fail;
    /* The selected surviving baseline loads without a second save, and an
     * abort after partial replay restores the exact reserved live state. */
    d->corr.epoch_id=17u; d->corr.load_tick=234u; d->corr.target_tick=270u;
    d->stage=kRbSealing;
    rb_run_replay(d);
    if(d->stage!=kRbReplaying || store.live!=234u || store.saves!=1u) goto fail;
    store.live=999u;
    rb_episode_abort(d,RNET_RB_ABORT_CLASS_REALIGN,"fixture partial replay");
    if(store.live!=273u || d->in_resim || d->stage!=kRbIdle) goto fail;
    /* Tip extension reserves before checking the original load too. */
    init_short(&store); d->stage=kRbTipHold; d->corr.load_tick=233u;
    if(rb_tip_extend(d,274u,1,1) || store.saves!=1u || short_has(&store,233u)) goto fail;
    rnet_rb_destroy(d->rb); free(d); return 0;
fail:
    fprintf(stderr,"short-store tip reservation or abort restore failed\n");
    rnet_rb_destroy(d->rb); free(d); return 1;
}

static int check_case(uint32_t prefix, int already_replayed)
{
    RNetRbDriver *d = calloc(1, sizeof(*d));
    uint32_t tick, load;
    int slot;
    if (!d) return 1;
    d->host.now_ms = now_ms;
    d->host.log = quiet;
    d->host.snap_oldest = snap_oldest;
    d->host.snap_has = snap_has;
    rb_owed_reset(d);
    d->sim = 136u;
    d->stage = already_replayed ? kRbVerifying : kRbSealing;
    d->corr.epoch_id = 9u;
    d->corr.mismatch_tick = 135u;
    d->corr.load_tick = d->corr.target_tick = 135u;
    d->corr.slot = 0;
    if (!already_replayed) rb_owed_mark(d, 135u, 0);
    d->owed_retry_after = 166u;
    d->cooldown_until_tick = 166u;
    oldest = prefix ? 97u : 0u;
    rb_on_nack(d, 9u, prefix, RNET_RB_SYNC_FLAG_PREFIX);
    if (d->stage != kRbIdle || !d->prefix_load_cap_valid || d->prefix_load_cap != prefix ||
        !rb_owed_first(d, &tick, &slot) || tick != 135u || slot != 0 ||
        d->owed_retry_after != d->sim || d->cooldown_until_tick != d->sim ||
        !rb_snap_floor(d, d->prefix_load_cap, &load) || load > prefix) {
        fprintf(stderr, "prefix retry failed prefix=%u replayed=%d\n", prefix, already_replayed);
        free(d); return 1;
    }
    /* An ordinary frontier NACK retains its normal backoff and does not
     * invent a prefix repair obligation. */
    rb_owed_reset(d);
    d->stage = kRbVerifying;
    d->corr.epoch_id = 17u;
    d->corr.load_tick = d->corr.mismatch_tick = 135u;
    rb_on_nack(d, 17u, 134u, 0u);
    if (d->prefix_load_cap_valid || rb_owed_first(d, &tick, &slot) ||
        d->cooldown_until_tick != d->sim + RB_COOLDOWN_TICKS) {
        free(d); return 1;
    }
    free(d); return 0;
}

int main(void)
{
    return check_case(118u, 1) || check_case(118u, 0) || check_case(0u, 1) ||
           check_confirmed_prediction() || check_tip_reservation() ||
           check_begin_before_reconcile(0u) || check_begin_before_reconcile(1u);
}
