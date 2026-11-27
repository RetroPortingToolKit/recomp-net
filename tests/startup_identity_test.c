/* SPDX-License-Identifier: MIT */
#include "recomp_net/recomp_net.h"
#include "recomp_net/startup_profiles.h"
#include "recomp_net/local_rollback.h"
#include "protocol/rnet_protocol.h"
#include <stdio.h>
#include <string.h>

enum { Seats=3, Queue=256 };
typedef struct {int size;rnet_u8 bytes[1200];} Packet;
typedef struct {RNetSession* session;int seat,first,count;Packet packets[Queue];} Node;
static Node nodes[Seats];
static rnet_u64 now;
static rnet_u32 magic;
static int failures,hold_fragment,held,early_ack;
static void check(int ok,const char* why) {if(!ok){fprintf(stderr,"FAIL: %s\n",why);++failures;}}
static rnet_u64 clock_ms(void* user) {(void)user;return now;}
static void sample(rnet_u32 tick,RNetInputSample* row,void* user) {
    (void)user;memset(row,0,sizeof(*row));row->tick=tick;row->size=1;row->valid=1;
}
static void publish(rnet_u32 tick,const RNetInputSample* rows,int count,void* user) {
    (void)tick;(void)rows;(void)count;(void)user;
}
static int send_packet(void* user,const rnet_u8* bytes,size_t size) {
    Node* from=user;RNetDecodedPacket decoded;int i;
    const int valid=rnet_proto_decode(bytes,size,magic,&decoded)==0;
    for(i=0;i<Seats;++i)if(i!=from->seat) {
        Node* to=&nodes[i];Packet* packet;
        if(hold_fragment && from->seat==2 && i==1 && valid && decoded.type==RNET_PKT_RB_SYNC &&
           decoded.rb_initiator==RNET_RB_SYNC_OP_PROFILE_IDENTITY && decoded.rb_flags==2) {++held;continue;}
        if(hold_fragment && i==1 && valid && decoded.type==RNET_PKT_RB_SYNC &&
           decoded.rb_initiator==RNET_RB_SYNC_OP_PROFILE_ACK)++early_ack;
        if(size>sizeof(to->packets[0].bytes) || to->count==Queue) {check(0,"bounded fixture queue");return -1;}
        packet=&to->packets[(to->first+to->count)%Queue];++to->count;
        packet->size=(int)size;memcpy(packet->bytes,bytes,size);
    }
    return (int)size;
}
static int recv_packet(void* user,rnet_u8* bytes,size_t size) {
    Node* node=user;Packet* packet;int count;
    if(!node->count)return 0;
    packet=&node->packets[node->first];count=packet->size;
    if(size<(size_t)count)return -1;
    memcpy(bytes,packet->bytes,count);node->first=(node->first+1)%Queue;--node->count;return count;
}
static void pump(void) {int i;++now;for(i=0;i<Seats;++i)rnet_session_pump(nodes[i].session);}
/* A full match; an aliased 32-bit prefix; missing full identity; missing one
 * fragment while ACKs arrive early; and full identity plus mode disagreement. */
static void run_case(int fault) {
    RNetProfileCaps caps={1,1,1,1};rnet_u8 identity[32];int i,step,ready=0,refused=0;
    memset(nodes,0,sizeof(nodes));now=1000;held=early_ack=0;hold_fragment=fault==3;
    for(i=0;i<32;++i)identity[i]=(rnet_u8)(i*7+3);
    for(i=0;i<Seats;++i) {
        RNetConfig config;RNetHostVTable host;RNetPacketTransport transport;rnet_u8 own[32];int result;
        rnet_config_init_defaults(&config);config.slot_count=Seats;config.local_slot=(rnet_u8)i;config.session_id=0x5141;
        magic=config.protocol_magic;memset(&host,0,sizeof(host));host.sample_local=sample;host.publish=publish;host.now_ms=clock_ms;
        nodes[i].seat=i;nodes[i].session=rnet_session_create(&config,&host);
        if(!nodes[i].session) {check(0,"session creation");goto done;}
        memcpy(own,identity,32);if(fault==1 && i==2)own[31]^=1;
        check(rnet_session_profiles_begin_full(nodes[i].session,42,&caps,NULL)==-1,"missing local identity rejected without setup");
        if(fault==2 && i==2)result=rnet_session_profiles_begin(nodes[i].session,42,&caps);
        else if(fault==4)result=rnet_experiment_profiles_begin_full(nodes[i].session,42,&caps,
            i==2?RNET_SYNC_LOCAL_V4:RNET_SYNC_EPISODE_V3,own);
        else result=rnet_session_profiles_begin_full(nodes[i].session,42,&caps,own);
        check(result==0,"immutable full startup configuration");
        check(rnet_session_profiles_begin_full(nodes[i].session,42,&caps,own)==-1,"repeated identity setup rejected");
        transport=(RNetPacketTransport){&nodes[i],send_packet,recv_packet,NULL,2};
        check(rnet_session_start_external(nodes[i].session,&transport)==0,"bounded external transport");
    }
    for(step=0;step<1500;++step) {
        pump();ready=refused=0;
        for(i=0;i<Seats;++i) {
            int status=rnet_session_profiles_status(nodes[i].session,NULL,NULL);
            ready+=status==1;refused+=status<0;
            if(status!=1)check(rnet_session_try_admit(nodes[i].session,0)!=1,"frame zero blocked before exact agreement");
        }
        if(fault==3 && step<400) {
            check(rnet_session_profiles_status(nodes[1].session,NULL,NULL)==0,"missing fragment cannot release or falsely refuse barrier");
            if(step==399) {check(held>0 && early_ack>0,"missing-fragment/early-ACK ordering exercised");hold_fragment=0;}
            continue;
        }
        if(fault==1 || fault==2 || fault==4) {if(refused)break;}
        else if(ready==Seats)break;
    }
    if(fault==1 || fault==2 || fault==4) {
        check(refused>0 && ready==0,"aliased identity, missing identity or mismatched mode refuses before frame zero");
    } else {
        rnet_u32 transcript=0;
        check(ready==Seats,"all full identities eventually agree");
        for(i=0;i<Seats;++i) {
            rnet_u32 selected=99,value=0;
            check(rnet_session_profiles_status(nodes[i].session,&selected,&value)==1 && selected==0,"portable profile agreed");
            if(!i)transcript=value;
            check(value==transcript,"full-identity capability transcript matches");
        }
        /* All fragments were immutable, even after successful selection. */
        rnet_u32 first=(rnet_u32)identity[0]|((rnet_u32)identity[1]<<8)|((rnet_u32)identity[2]<<16)|((rnet_u32)identity[3]<<24);
        rnet_session_send_rb_sync(nodes[2].session,42,first^1u,0,0,0,RNET_RB_SYNC_OP_PROFILE_IDENTITY,0);
        for(step=0;step<10;++step)pump();
        check(rnet_session_profiles_status(nodes[0].session,NULL,NULL)==-1,"changed identity refuses instead of renegotiating");
    }
done:
    for(i=0;i<Seats;++i)if(nodes[i].session)rnet_session_destroy(nodes[i].session);
}
int main(void) {
    int fault;for(fault=0;fault<5;++fault)run_case(fault);
    printf("exact startup identity: %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
