/* Exercise the actual wire NACK handler, including teardown, with a short
 * snapshot window. Other driver dependencies come from the normal library. */
#include "../src/rollback/rnet_rb_driver.c"

static uint32_t now_ms(void *ctx) { (void)ctx; return 100u; }
static void quiet(void *ctx, const char *line) { (void)ctx; (void)line; }
static uint32_t oldest;
static int snap_oldest(void *ctx, uint32_t *out) { (void)ctx; *out = oldest; return 1; }
static int snap_has(void *ctx, uint32_t tick) { (void)ctx; return tick >= oldest && tick <= 136u; }

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
    return check_case(118u, 1) || check_case(118u, 0) || check_case(0u, 1);
}
