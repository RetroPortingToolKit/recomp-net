/* SPDX-License-Identifier: MIT */
#include "recomp_net/local_rollback.h"
#include "protocol/rnet_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PEERS 4
#define QUEUE 512
#define END_TICK 384u
typedef struct Packet { int from, size; rnet_u8 bytes[RNET_MAX_PACKET]; } Packet;
typedef struct Peer {
    RNetSession *session; RNetLocalDriver *driver;
    int seat, head, count; Packet packets[QUEUE];
    rnet_u32 state, tick, replay, begins, ends;
    RNetInputSample rows[MAX_PEERS];
    struct { rnet_u32 state, tick; int valid; } snaps[RNET_LOCAL_HISTORY];
} Peer;
static Peer peers[MAX_PEERS];
static int seats, failures, dropped, held, drop_inputs, dropped_inputs;
static unsigned round_index;
static rnet_u64 clock_ms=1000;
static int ready_reply_drops[MAX_PEERS],done_drops[MAX_PEERS];
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); ++failures; } } while(0)
static rnet_u64 now_ms(void *ctx) { (void)ctx;return clock_ms; }
static rnet_u64 now_us(void *ctx) { return now_ms(ctx)*1000u; }
static int enqueue(int seat,int from,const rnet_u8 *bytes,size_t size) {
    Peer *p=&peers[seat];Packet *out;RNetDecodedPacket packet;
    if(rnet_proto_decode(bytes,size,0x524e4554u,&packet))return -1;
    /* The caller-owned hub binds each packet to its authenticated sender.
     * Forwarding preserves that binding, rather than trusting a claimed seat. */
    if(packet.type!=RNET_PKT_START && packet.type!=RNET_PKT_DELAY_SYNC &&
       packet.local_slot!=(rnet_u8)from)return -1;
    if(seat==0 && packet.type==RNET_PKT_STATE_PROBE_REPLY && packet.state_op==RNET_STATE_OP_LOAD &&
       packet.state_total_size==0 && ready_reply_drops[from]>0) {
        --ready_reply_drops[from];return (int)size;
    }
    if(packet.type==RNET_PKT_STATE_PROBE_DONE && done_drops[seat]>0) {
        --done_drops[seat];return (int)size;
    }
    if(seat==(seats==2?0:1) && from==seats-1 && round_index<1500u &&
       packet.type==RNET_PKT_RB_FRAME_COMMIT && packet.rb_through_tick==4u) {
        ++dropped;return (int)size;
    }
    if(drop_inputs && seat==1 && from==0 && round_index<80u &&
       packet.type==RNET_PKT_INPUT) {
        int i;for(i=0;i<packet.frame_count;i++)if(packet.frames[i].tick==0u) {
            ++dropped;++dropped_inputs;return (int)size;
        }
    }
    if(p->count==QUEUE || size>sizeof(p->packets[0].bytes))return -1;
    out=&p->packets[(p->head+p->count++)%QUEUE];out->from=from;out->size=(int)size;
    memcpy(out->bytes,bytes,size);return (int)size;
}
static int send_packet(void *ctx,const rnet_u8 *bytes,size_t size) {
    Peer *p=ctx;int i;
    for(i=0;i<seats;i++)if(i!=p->seat && (!p->seat || !i))
        if(enqueue(i,p->seat,bytes,size)<0)return -1;
    return (int)size;
}
static int receive_packet(void *ctx,rnet_u8 *bytes,size_t cap) {
    Peer *p=ctx;Packet *in;int i,size;
    if(!p->count)return 0;
    in=&p->packets[p->head];size=in->size;if((size_t)size>cap)return -1;
    if(!p->seat)for(i=1;i<seats;i++)if(i!=in->from)
        if(enqueue(i,in->from,in->bytes,(size_t)size)<0)return -1;
    memcpy(bytes,in->bytes,(size_t)size);p->head=(p->head+1)%QUEUE;--p->count;return size;
}
static void sample(rnet_u32 tick,RNetInputSample *row,void *ctx) {
    Peer *p=ctx;int b;memset(row,0,sizeof(*row));row->valid=1;row->size=32;
    for(b=0;b<32;b++)row->bytes[b]=(rnet_u8)(tick*13u+(unsigned)b*7u+(unsigned)p->seat*11u);
}
static void neutral(void *ctx,int seat,RNetInputSample *row) {
    (void)ctx;(void)seat;memset(row,0,sizeof(*row));row->valid=1;row->size=32;
}
static void publish_unused(rnet_u32 tick,const RNetInputSample *rows,int count,void *ctx) {
    (void)tick;(void)rows;(void)count;(void)ctx;
}
static int save(void *ctx,rnet_u32 tick) {
    Peer *p=ctx;unsigned i=tick%RNET_LOCAL_HISTORY;
    p->snaps[i].state=p->state;p->snaps[i].tick=tick;p->snaps[i].valid=1;return 1;
}
static int load(void *ctx,rnet_u32 tick) {
    Peer *p=ctx;unsigned i=tick%RNET_LOCAL_HISTORY;
    if(!p->snaps[i].valid || p->snaps[i].tick!=tick)return 0;
    p->state=p->snaps[i].state;return 1;
}
static void drop_after(void *ctx,rnet_u32 tick) {
    Peer *p=ctx;unsigned i;for(i=0;i<RNET_LOCAL_HISTORY;i++)
        if(p->snaps[i].valid && p->snaps[i].tick>tick)p->snaps[i].valid=0;
}
static void publish(void *ctx,rnet_u32 tick,const RNetInputSample *rows,int count,int replay) {
    Peer *p=ctx;CHECK(count==seats);p->tick=tick;
    memcpy(p->rows,rows,sizeof(*rows)*(size_t)count);if(replay)++p->replay;
}
static rnet_u32 run_rows(rnet_u32 state,rnet_u32 tick,const RNetInputSample *rows) {
    int s,b;for(s=0;s<seats;s++)for(b=0;b<32;b++)state=(state^rows[s].bytes[b])*16777619u;
    return (state^tick)*16777619u;
}
static int run_tick(void *ctx,rnet_u32 tick) {
    Peer *p=ctx;CHECK(p->tick==tick);p->state=run_rows(p->state,tick,p->rows);return 1;
}
static void begin(void *ctx) {++((Peer*)ctx)->begins;}
static void end(void *ctx) {++((Peer*)ctx)->ends;}
static rnet_u32 digest(void *ctx) {return ((Peer*)ctx)->state;}
static void create_driver(Peer *p) {
    RNetLocalConfig cfg={0};RNetLocalHost host={0};
    cfg.session=p->session;cfg.seats=(rnet_u32)seats;cfg.local_seat=(rnet_u32)p->seat;
    cfg.delay=2;cfg.rollback_depth=8;cfg.row_size=32;cfg.tick_us=16667;
    host.ctx=p;host.save=save;host.load=load;host.drop_after=drop_after;host.publish=publish;
    host.run_tick=run_tick;host.resim_begin=begin;host.resim_end=end;host.digest=digest;
    host.now_us=now_us;host.neutral=neutral;
    p->driver=rnet_local_create(&cfg,&host);CHECK(p->driver);
    CHECK(rnet_local_set_hold(p->driver,END_TICK));
}
static int drive(void) {
    for(round_index=0;round_index<20000u;round_index++) {
        int i,complete=0;clock_ms++;
        for(i=0;i<seats;i++) {
            Peer *p=&peers[i];int admitted=rnet_local_poll(p->driver);
            if(rnet_local_refusal(p->driver)) {
                fprintf(stderr,"seat %d round %u tick %u: %s\n",i,round_index,
                    rnet_local_tick(p->driver),rnet_local_refusal(p->driver));return 0;
            }
            if(admitted) {CHECK(admitted==1);run_tick(p,p->tick);CHECK(rnet_local_finish(p->driver));}
            if(round_index==1000u && rnet_local_tick(p->driver)<END_TICK)++held;
            if(rnet_local_tick(p->driver)==END_TICK && rnet_local_verified(p->driver)==END_TICK)++complete;
        }
        if(complete==seats)return 1;
    }
    for(int i=0;i<seats;i++)fprintf(stderr,"unfinished seat %d tick=%u confirmed=%u verified=%u\n",i,
        rnet_local_tick(peers[i].driver),rnet_local_confirmed(peers[i].driver),rnet_local_verified(peers[i].driver));
    return 0;
}
static void check_state(void) {
    rnet_u32 expected=2166136261u,t;RNetInputSample rows[MAX_PEERS];int i;
    for(t=0;t<END_TICK;t++) {
        for(i=0;i<seats;i++)if(t<2u)neutral(NULL,i,&rows[i]);else sample(t-2u,&rows[i],&peers[i]);
        expected=run_rows(expected,t,rows);
    }
    for(i=0;i<seats;i++) {CHECK(peers[i].state==expected);CHECK(peers[i].begins==peers[i].ends);}
}
static void restart(Peer *p) {
    RNetInputSample row;
    rnet_local_destroy(p->driver);p->state=2166136261u;
    memset(p->snaps,0,sizeof(p->snaps));rnet_session_hard_resync(p->session);
    neutral(NULL,p->seat,&row);rnet_session_prime_delay_inputs(p->session,row.bytes,row.size);create_driver(p);
}
static void pump_sessions(void) {
    ++clock_ms;for(int i=0;i<seats;i++)rnet_session_pump(peers[i].session);
}
static int all_idle(void) {
    for(int i=1;i<seats;i++)if(rnet_session_state_busy(peers[i].session))return 0;
    return 1;
}
static void probe_epoch_roundtrip(void) {
    int changed[MAX_PEERS]={0},complete=0,i,k,match=0;rnet_u8 bytes[64];int size;
    for(i=1;i<seats;i++)ready_reply_drops[i]=3;
    CHECK(!rnet_session_state_probe(peers[0].session,RNET_STATE_OP_LOAD,4,0,0x4c4f4144u));
    for(k=0;k<5000 && !complete;k++) {
        pump_sessions();
        for(i=1;i<seats;i++)if(rnet_session_state_probe_pending(peers[i].session,NULL,NULL,NULL,NULL)) {
            CHECK(!changed[i]);CHECK(!rnet_session_state_probe_reply(peers[i].session,1));
            restart(&peers[i]);changed[i]=1;
        }
        if(rnet_session_state_probe_take_reply(peers[0].session,&match)) {
            CHECK(match);rnet_session_state_probe_finish(peers[0].session);restart(&peers[0]);complete=1;
        }
    }
    CHECK(complete);for(i=1;i<seats;i++)CHECK(changed[i] && !ready_reply_drops[i]);
    /* A delayed pre-load request cannot reapply/stall the new epoch. */
    size=rnet_proto_encode_state_probe(bytes,sizeof(bytes),0x524e4554u,1,0,RNET_STATE_OP_LOAD,4,123,0x7654u);
    size=rnet_proto_tag_rb_epoch(bytes,(size_t)size,sizeof(bytes),0);
    for(i=1;i<seats;i++)CHECK(enqueue(i,0,bytes,(size_t)size)>0);
    for(k=0;k<8;k++)pump_sessions();
    CHECK(all_idle());
    printf("%dp ready ACKs recovered after guest epoch advance; stale LOAD ignored\n",seats);
}
static void done_epoch_roundtrip(void) {
    int i,k,complete=0,match=0;rnet_u8 bytes[64];int size;
    done_drops[seats-1]=3;
    CHECK(!rnet_session_state_probe(peers[0].session,RNET_STATE_OP_SAVE,4,123,0x7654u));
    for(k=0;k<5000 && !complete;k++) {
        pump_sessions();for(i=1;i<seats;i++)
            if(rnet_session_state_probe_pending(peers[i].session,NULL,NULL,NULL,NULL))CHECK(!rnet_session_state_probe_reply(peers[i].session,1));
        if(rnet_session_state_probe_take_reply(peers[0].session,&match)) {
            CHECK(match);rnet_session_state_probe_finish(peers[0].session);restart(&peers[0]);complete=1;
        }
    }
    CHECK(complete);
    for(k=0;k<5000 && !all_idle();k++)pump_sessions();
    CHECK(all_idle() && !done_drops[seats-1]);
    for(i=1;i<seats;i++)restart(&peers[i]);
    /* Reuse the tuple in the next epoch: an old DONE cannot release it. */
    CHECK(!rnet_session_state_probe(peers[0].session,RNET_STATE_OP_SAVE,4,123,0x7654u));
    for(k=0;k<5000;k++) {
        int parked=0;pump_sessions();
        for(i=1;i<seats;i++) {
            if(rnet_session_state_probe_pending(peers[i].session,NULL,NULL,NULL,NULL))CHECK(!rnet_session_state_probe_reply(peers[i].session,1));
            if(rnet_session_state_busy(peers[i].session))++parked;
        }
        if(parked==seats-1)break;
    }
    size=rnet_proto_encode_state_probe_done(bytes,sizeof(bytes),0x524e4554u,1,0,RNET_STATE_OP_SAVE,4,123,0x7654u);
    size=rnet_proto_tag_rb_epoch(bytes,(size_t)size,sizeof(bytes),1);
    for(i=1;i<seats;i++)CHECK(enqueue(i,0,bytes,(size_t)size)>0);
    for(k=0;k<8;k++)pump_sessions();
    for(i=1;i<seats;i++)CHECK(rnet_session_state_busy(peers[i].session));
    for(k=0;k<5000 && !rnet_session_state_probe_take_reply(peers[0].session,&match);k++)pump_sessions();
    CHECK(match);rnet_session_state_probe_finish(peers[0].session);
    for(k=0;k<5000 && !all_idle();k++)pump_sessions();CHECK(all_idle());
    printf("%dp lost DONE recovered across host epoch advance; stale DONE ignored\n",seats);
}
static void scenario(int count) {
    RNetProfileCaps caps={1u,1u,1u,0u};int i;
    memset(peers,0,sizeof(peers));seats=count;dropped=held=drop_inputs=dropped_inputs=0;round_index=0;
    for(i=0;i<seats;i++) {
        Peer *p=&peers[i];RNetConfig cfg;RNetHostVTable host={0};RNetPacketTransport transport={0};
        rnet_config_init_defaults(&cfg);cfg.local_slot=(rnet_u8)i;cfg.slot_count=(rnet_u8)seats;
        p->seat=i;p->state=2166136261u;host.ctx=p;host.sample_local=sample;
        host.publish=publish_unused;host.now_ms=now_ms;p->session=rnet_session_create(&cfg,&host);
        CHECK(p->session);CHECK(!rnet_experiment_profiles_begin(p->session,0x1245u,&caps,RNET_SYNC_LOCAL_V4));
        transport.ctx=p;transport.send=send_packet;transport.recv=receive_packet;transport.path=3;
        CHECK(!rnet_session_start_external(p->session,&transport));create_driver(p);
    }
    CHECK(drive());CHECK(dropped>0);CHECK(held>0);check_state();
    printf("%dp withheld hash: dropped=%d held=%d verified=%u\n",seats,dropped,held,rnet_local_verified(peers[0].driver));
    round_index=0;drop_inputs=seats==2;
    probe_epoch_roundtrip();
    if(seats==2) {
        /* Recreate exactly the post-shared-load epoch. Losing the first new
         * neutral row must recover even after higher rows arrive. */
        drop_inputs=1;round_index=0;
        CHECK(drive());CHECK(dropped_inputs>0);check_state();
        printf("2p post-load missing row zero: dropped=%d verified=%u\n",dropped_inputs,rnet_local_verified(peers[0].driver));
    }
    done_epoch_roundtrip();
    /* A genuine occupied peer still cannot release future, unverified data. */
    CHECK(!rnet_session_send_rb_sync(peers[seats-1].session,0,END_TICK+1u,0,0,0,
        RNET_RB_SYNC_OP_FRAME_COMMIT_REQUEST,0));
    for(i=0;i<8;i++) {clock_ms++;for(int s=0;s<seats;s++)rnet_local_poll(peers[s].driver);}
    CHECK(rnet_local_refusal(peers[0].driver));
    CHECK(rnet_local_refusal(peers[0].driver) && !strcmp(rnet_local_refusal(peers[0].driver),"invalid verification credit"));
    for(i=0;i<seats;i++) {rnet_local_destroy(peers[i].driver);rnet_session_destroy(peers[i].session);}
}
int main(void) {scenario(2);scenario(3);scenario(4);return failures?1:0;}
