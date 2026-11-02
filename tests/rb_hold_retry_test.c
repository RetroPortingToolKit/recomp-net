/* Exercise the actual reconciliation path at a held frame. No socket timing
 * or validation injection is needed to establish the queued abort backoff. */
#include "../src/rollback/rnet_rb_driver.c"

static uint32_t clock_ms;
static uint32_t now_ms(void *ctx) { (void)ctx; return clock_ms; }
static void quiet(void *ctx, const char *line) { (void)ctx; (void)line; }
static int snapshot_has(void *ctx, uint32_t t) { (void)ctx; return t <= 34u; }
static int snapshot_oldest(void *ctx, uint32_t *t) { (void)ctx; *t=0; return 1; }
static int inert_tick(void *ctx, uint32_t t) { (void)ctx; (void)t; return 1; }
static uint8_t real_row(void *ctx, int32_t slot, uint32_t tick, RNetRbFrame *out)
{
    (void)ctx; (void)slot; memset(out,0,sizeof(*out));
    out->tick=tick; out->is_valid=1; return 1;
}
static void sample(uint32_t tick, RNetInputSample *out, void *ctx)
{ (void)tick; (void)ctx; memset(out,0,sizeof(*out)); out->size=out->valid=1; }
static void unused_publish(uint32_t tick, const RNetInputSample *rows, int slots, void *ctx)
{ (void)tick; (void)rows; (void)slots; (void)ctx; }

static int check_retry(int held, uint32_t last_retry, uint32_t now, int expect_open)
{
    RNetRbDriver *d=calloc(1,sizeof(*d));
    RNetConfig nc; RNetHostVTable nh={0}; RNetRbConfig rc={0}; RNetRollbackVTable vt={0};
    RNetSession *session; int failed;
    if(!d) return 1;
    rnet_config_init_defaults(&nc); nh.sample_local=sample; nh.publish=unused_publish;
    session=rnet_session_create(&nc,&nh);
    if(!session) { free(d); return 1; }
    rc.slot_count=2; vt.load_state=inert_tick; vt.advance_sim=inert_tick; vt.get_input_row=real_row;
    d->rb=rnet_rb_create(&rc,&vt);
    if(!d->rb) { rnet_session_destroy(session); free(d); return 1; }
    d->cfg.session=&session; d->host.now_ms=now_ms; d->host.log=quiet;
    d->host.snap_has=snapshot_has; d->host.snap_oldest=snapshot_oldest;
    d->snap_interval=1; d->sim=34; d->hold_active=held; d->hold_at=34;
    d->cooldown_until_tick=45; d->held_retry_ms=last_retry;
    rb_owed_reset(d); d->owed_retry_after=45; rb_owed_mark(d,21,1);
    rnet_ih_reset(&d->ih,2); clock_ms=now;
    rb_reconcile_wire(d);
    failed=d->sim!=34 || (expect_open ?
        (d->episode_count!=1 || d->stage!=kRbSealing ||
         d->corr.load_tick>21 || d->corr.target_tick>=34) :
        (d->episode_count!=0 || d->stage!=kRbIdle));
    if(failed) fprintf(stderr,"held=%d elapsed=%u: expected repair=%d, episodes=%u stage=%d sim=%u\n",
        held,(unsigned)(now-last_retry),expect_open,(unsigned)d->episode_count,(int)d->stage,(unsigned)d->sim);
    rnet_rb_destroy(d->rb); rnet_session_destroy(session); free(d); return failed;
}

int main(void)
{
    int failed=check_retry(1,0,100,1); /* Reached hold can repair before frame45. */
    failed+=check_retry(0,0,100,0);    /* Ordinary running backoff is preserved. */
    failed+=check_retry(1,90,100,0);   /* Repeated polls do not spam new attempts. */
    failed+=check_retry(1,90,140,1);   /* Wall-clock retry needs no guest frame. */
    if(!failed) puts("rb_hold_retry_test: held repair, ordinary backoff and bounded retry pass");
    return failed?1:0;
}
