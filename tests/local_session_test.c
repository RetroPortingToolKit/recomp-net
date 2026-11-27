#include "recomp_net/local_rollback.h"
#include "platform/rnet_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
#define N 180u
typedef struct Peer {
    RNetSession *session; RNetLocalDriver *driver;
    rnet_u32 seat, state, tick, replay, begins, ends;
    struct { rnet_u32 state, tick; int valid; } snaps[RNET_LOCAL_HISTORY];
    RNetInputSample rows[2]; int fork;
} Peer;
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); ++failures; } } while (0)
static rnet_u64 now_ms(void *ctx) { (void)ctx; return rnet_os_monotonic_ms(); }
static rnet_u64 now_us(void *ctx) { return now_ms(ctx) * 1000u; }
static void sample(rnet_u32 t, RNetInputSample *out, void *ctx) {
    Peer *p = ctx; unsigned b;
    memset(out,0,sizeof(*out)); out->valid=1; out->size=32;
    for(b=0;b<32;b++)out->bytes[b]=(rnet_u8)(t*13u+b*7u+p->seat*11u);
}
static void canonicalize(void *ctx,RNetInputSample *row) {
    (void)ctx; row->bytes[31]&=0xfcu;
}
static void neutral(void *ctx,int seat,RNetInputSample *out) {
    (void)ctx;(void)seat;memset(out,0,sizeof(*out));out->size=32;out->valid=1;
}
static void noop_publish(rnet_u32 t,const RNetInputSample *r,int n,void *ctx) {
    (void)t;(void)r;(void)n;(void)ctx;
}
static int save(void *ctx,rnet_u32 t) {
    Peer *p=ctx;unsigned i=t%RNET_LOCAL_HISTORY;
    p->snaps[i].state=p->state;p->snaps[i].tick=t;p->snaps[i].valid=1;return 1;
}
static int load(void *ctx,rnet_u32 t) {
    Peer *p=ctx;unsigned i=t%RNET_LOCAL_HISTORY;
    if(!p->snaps[i].valid || p->snaps[i].tick!=t)return 0;
    p->state=p->snaps[i].state;return 1;
}
static void drop(void *ctx,rnet_u32 t) {
    Peer *p=ctx;unsigned i;for(i=0;i<RNET_LOCAL_HISTORY;i++)
        if(p->snaps[i].valid && p->snaps[i].tick>t)p->snaps[i].valid=0;
}
static void publish(void *ctx,rnet_u32 t,const RNetInputSample *rows,int count,int replay) {
    Peer *p=ctx;CHECK(count==2);p->tick=t;memcpy(p->rows,rows,sizeof(p->rows));
    if(replay)++p->replay;
}
static void run(Peer *p) {
    unsigned s,b;for(s=0;s<2;s++)for(b=0;b<32;b++)p->state=(p->state^p->rows[s].bytes[b])*16777619u;
    p->state=(p->state^p->tick)*16777619u;
    if(p->fork && p->tick==40u)p->state^=0x55aa55aau;
}
static void begin(void *ctx) {++((Peer*)ctx)->begins;}
static void end(void *ctx) {++((Peer*)ctx)->ends;}
static rnet_u32 digest(void *ctx) {return ((Peer*)ctx)->state;}
static void scenario(int mismatch,int fork,int adaptive) {
    Peer peers[2]={{0}};RNetProfileCaps caps={1u,1u,1u,0u};
    char addresses[2][48],report[4096];rnet_u64 next[2]={0,0};int s,done=0;
    unsigned port=22000u+((unsigned)getpid()%12000u)*2u+(unsigned)mismatch*4u+(unsigned)fork*8u+(unsigned)adaptive*12u;
    for(s=0;s<2;s++) {
        Peer *p=&peers[s];RNetConfig cfg;RNetHostVTable vt={0};
        RNetLocalConfig lc={0};RNetLocalHost host={0};
        rnet_config_init_defaults(&cfg);cfg.local_slot=(rnet_u8)s;cfg.input_delay=(rnet_u8)(s?3u:1u);
        cfg.session_id=0x183300u+(unsigned)mismatch+(unsigned)fork*2u;
        p->seat=(rnet_u32)s;p->state=2166136261u;p->fork=fork && s;
        vt.ctx=p;vt.sample_local=sample;vt.publish=noop_publish;vt.now_ms=now_ms;
        p->session=rnet_session_create(&cfg,&vt);CHECK(p->session);if(!p->session)goto cleanup;
        CHECK(!rnet_session_set_input_transform(p->session,canonicalize,NULL));
        CHECK(rnet_experiment_profiles_begin(p->session,0,&caps,RNET_SYNC_LOCAL_V4)==-1);
        CHECK(!rnet_session_local_inputs_enabled(p->session));
        {
            RNetProfileCaps invalid=caps;invalid.profiles=0;
            CHECK(rnet_experiment_profiles_begin(p->session,0x123456u,&invalid,RNET_SYNC_LOCAL_V4)==-1);
            CHECK(!rnet_session_local_inputs_enabled(p->session));
        }
        CHECK(!rnet_experiment_profiles_begin(p->session,0x123456u,&caps,
            mismatch && s ? RNET_SYNC_EPISODE_V3 : RNET_SYNC_LOCAL_V4));
        snprintf(addresses[s],sizeof(addresses[s]),"127.0.0.1:%u",port+(unsigned)s);
        if(mismatch && s)continue;
        lc.session=p->session;lc.seats=2;lc.local_seat=(rnet_u32)s;lc.row_size=32;
        lc.delay=cfg.input_delay;lc.rollback_depth=16;lc.tick_us=16667;
        host.ctx=p;host.save=save;host.load=load;host.drop_after=drop;host.publish=publish;
        host.resim_begin=begin;host.resim_end=end;host.digest=digest;host.now_us=now_us;host.neutral=neutral;
        p->driver=rnet_local_create(&lc,&host);CHECK(p->driver);if(!p->driver)goto cleanup;
        if(adaptive) {
            RNetLocalControlConfig control={0};RNetLocalControlState state;
            control.frame_budget_us=16667;control.live_tick_us=12000;control.replay_tick_us=2000;
            control.replay_setup_us=1000;
            control.one_way_us=200000;control.network_valid=1;control.interval_ticks=8;
            CHECK(rnet_local_control_begin(p->driver,&control));
            CHECK(rnet_local_control_state(p->driver,&state));CHECK(state.delay==11);
        }
        CHECK(rnet_local_set_hold(p->driver,N));
    }
    for(s=0;s<2;s++)CHECK(!rnet_session_start_lan(peers[s].session,addresses[s],s?addresses[0]:NULL));
    while(!done) {
        rnet_u64 now=now_ms(NULL);int complete=0;
        for(s=0;s<2;s++) {
            Peer *p=&peers[s];int a;
            rnet_session_pump(p->session);
            if(mismatch) {
                if(rnet_session_profiles_status(p->session,NULL,NULL)<0)done=1;
                CHECK(!p->driver || rnet_local_tick(p->driver)==0);continue;
            }
            if(now<next[s])continue;
            a=rnet_local_poll(p->driver);
            if(rnet_local_refusal(p->driver)) {
                CHECK(fork && !strcmp(rnet_local_refusal(p->driver),"confirmed state desync"));
                done=1;break;
            }
            if(a) {
                run(p);CHECK(rnet_local_finish(p->driver));
                if(a==1)next[s]=now+16u;
            }
            if(rnet_local_tick(p->driver)==N && rnet_local_verified(p->driver)==N)++complete;
        }
        if(complete==2)done=1;
        if(!done)rnet_session_wait_recv(peers[0].session,1);
    }
    if(!mismatch && !fork) {
        RNetPeerTelemetry probe;
        CHECK(peers[0].state==peers[1].state);
        CHECK(peers[0].replay+peers[1].replay>0 || !getenv("RNET_SIM_LATENCY_MS"));
        for(s=0;s<2;s++) {
            if(adaptive) {
                RNetLocalControlState state;
                CHECK(rnet_local_control_state(peers[s].driver,&state));
                /* A coarse clock can retain the measured startup replay cost;
                 * it must not turn an unmeasurable tiny fixture into free replay. */
                CHECK(state.delay>=1u && state.delay<11u);
                CHECK(rnet_session_committed_delay(peers[s].session)==state.delay);
                printf("adaptive seat=%d delay=%u target=%u depth=%u live_us=%u replay_us=%u\n",
                    s,state.delay,state.target_delay,state.rollback_depth,state.live_tick_us,state.replay_tick_us);
            } else CHECK(rnet_session_committed_delay(peers[s].session)==(s?3u:1u));
            CHECK(peers[s].begins==peers[s].ends);
            CHECK(rnet_local_verified(peers[s].driver)==N);
            CHECK(rnet_local_report(peers[s].driver,report,sizeof(report))>0);
            {
                unsigned long long replay_total=0;
                const char *field=strstr(report,"\"replay_ticks\":");
                CHECK(field && sscanf(field,"\"replay_ticks\":%llu",&replay_total)==1);
                CHECK(replay_total==peers[s].replay);
            }
            puts(report);
            CHECK(rnet_session_get_peer_telemetry(peers[s].session,1-s,&probe));
            CHECK(probe.received>0 && probe.sent>=probe.received);
        }
    }
    printf("socket scenario mode_mismatch=%d deliberate_fork=%d adaptive=%d ticks=%u/%u replay=%u/%u\n",
        mismatch,fork,adaptive,rnet_local_tick(peers[0].driver),rnet_local_tick(peers[1].driver),peers[0].replay,peers[1].replay);
cleanup:
    for(s=0;s<2;s++){rnet_local_destroy(peers[s].driver);rnet_session_destroy(peers[s].session);}
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"trace")) {scenario(0,0,0);scenario(0,0,1);}
    else {scenario(1,0,0);scenario(0,0,0);scenario(0,1,0);scenario(0,0,1);}
    return failures?1:0;
}
