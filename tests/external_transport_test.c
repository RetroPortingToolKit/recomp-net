/* SPDX-License-Identifier: MIT */
#include "recomp_net/recomp_net.h"
#include "transport/rnet_transport.h"
#include <stdio.h>
#include <string.h>

#define NODES 3
#define QMAX 256
typedef struct Packet { int from, size; rnet_u8 bytes[2048]; } Packet;
typedef struct Node {
    RNetSession *session;
    int seat, head, count, fail_send, fail_recv, fail_pump;
    Packet packets[QMAX];
} Node;
static Node nodes[NODES];
static rnet_u64 now = 1000;
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); ++failures; } } while (0)
static int enqueue(Node *n, int from, const rnet_u8 *b, size_t size) {
    Packet *p;
    if (n->count == QMAX || size > sizeof(n->packets[0].bytes)) return -1;
    p=&n->packets[(n->head+n->count++)%QMAX];p->from=from;p->size=(int)size;
    memcpy(p->bytes,b,size);return (int)size;
}
/* Model the launcher-owned hub, independently of each child session. */
static int send_packet(void *ctx,const rnet_u8 *b,size_t size) {
    Node *me=ctx;int i;
    if(me->fail_send)return (int)size-1;
    for(i=0;i<NODES;i++)if(i!=me->seat && (!me->seat || !i))
        if(enqueue(&nodes[i],me->seat,b,size)<0)return -1;
    return (int)size;
}
static int receive_packet(void *ctx,rnet_u8 *b,size_t cap) {
    Node *me=ctx;Packet *p;int i,size;
    if(me->fail_recv)return -1;
    if(!me->count)return 0;
    p=&me->packets[me->head];size=p->size;
    if((size_t)size>cap)return -1;
    if(!me->seat)for(i=1;i<NODES;i++)if(i!=p->from)
        if(enqueue(&nodes[i],p->from,p->bytes,(size_t)size)<0)return -1;
    memcpy(b,p->bytes,(size_t)size);me->head=(me->head+1)%QMAX;me->count--;return size;
}
static int pump(void *ctx) { return ((Node*)ctx)->fail_pump ? -1 : 0; }
static rnet_u64 clock_ms(void *ctx) { (void)ctx;return now; }
static void sample(rnet_u32 tick,RNetInputSample *out,void *ctx) {
    unsigned b;Node *n=ctx;memset(out,0,sizeof(*out));out->valid=1;out->size=32;
    for(b=0;b<32;b++)out->bytes[b]=(rnet_u8)(tick*13u+b*7u+(unsigned)n->seat*11u);
}
static void publish(rnet_u32 t,const RNetInputSample *r,int n,void *ctx) {
    (void)t;(void)r;(void)n;(void)ctx;
}
static void pump_all(void) { int i;now+=10;for(i=0;i<NODES;i++)rnet_session_pump(nodes[i].session); }
int main(void) {
    int i,j,k;unsigned tick;
    for(i=0;i<NODES;i++) {
        RNetConfig cfg;RNetHostVTable host={0};RNetPacketTransport transport={0};
        rnet_config_init_defaults(&cfg);cfg.slot_count=NODES;cfg.local_slot=(rnet_u8)i;cfg.input_delay=0;
        nodes[i].seat=i;host.sample_local=sample;host.publish=publish;host.now_ms=clock_ms;host.ctx=&nodes[i];
        nodes[i].session=rnet_session_create(&cfg,&host);CHECK(nodes[i].session);
        transport.ctx=&nodes[i];transport.send=send_packet;transport.recv=receive_packet;transport.pump=pump;transport.path=3;
        if(!i) { RNetPacketTransport bad=transport;bad.recv=NULL;CHECK(rnet_session_start_external(nodes[i].session,&bad)==-1); }
        CHECK(!rnet_session_start_external(nodes[i].session,&transport));
        CHECK(rnet_session_start_external(nodes[i].session,&transport)==-1);
    }
    for(k=0;k<200;k++)pump_all();
    for(i=0;i<NODES;i++)CHECK(rnet_session_is_running(nodes[i].session));
    for(tick=0;tick<48;tick++) {
        for(i=0;i<NODES;i++)CHECK(rnet_session_prepare_local_tip(nodes[i].session,tick)>0);
        for(k=0;k<8;k++)pump_all();
        for(i=0;i<NODES;i++) {
            for(j=0;j<NODES;j++) {
                RNetInputSample got,want;
                CHECK(rnet_session_peek_input(nodes[i].session,j,tick,&got)==1);
                sample(tick,&want,&nodes[j]);
                CHECK(got.size==32 && !memcmp(got.bytes,want.bytes,32));
            }
            rnet_session_advance(nodes[i].session);
        }
    }
    { RNetSessionStats stats;rnet_session_get_stats(nodes[0].session,&stats);CHECK(!strcmp(stats.ice_path,"relay")); }
    nodes[1].fail_recv=1;rnet_session_pump(nodes[1].session);
    CHECK(rnet_session_transport_failed(nodes[1].session));CHECK(!rnet_session_is_running(nodes[1].session));
    CHECK(!rnet_session_try_admit(nodes[1].session,48));
    nodes[2].fail_pump=1;rnet_session_pump(nodes[2].session);CHECK(rnet_session_transport_failed(nodes[2].session));
    { RNetTransport transport;RNetPacketTransport packet={&nodes[0],send_packet,receive_packet,pump,1};rnet_u8 b[20]={0};
      rnet_transport_init(&transport);CHECK(!rnet_transport_start_external(&transport,&packet));nodes[0].fail_send=1;
      CHECK(rnet_transport_send(&transport,b,sizeof(b))==-1);CHECK(transport.external_failed);rnet_transport_shutdown(&transport); }
    for(i=0;i<NODES;i++)rnet_session_destroy(nodes[i].session);
    if(!failures)puts("external_transport_test: three real protocol sessions, full 32-byte rows, latched failures passed");
    return failures?1:0;
}
