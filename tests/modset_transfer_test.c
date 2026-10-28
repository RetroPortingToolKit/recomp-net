#include "recomp_net/recomp_net.h"
#include "protocol/rnet_protocol.h"
#include "session/rnet_modset.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#define test_pid _getpid
#else
#include <unistd.h>
#define test_pid getpid
#endif

static char settings[RNET_MODSET_TEXT_MAX];
static rnet_u64 now;
static rnet_u64 Clock(void *ctx) { (void)ctx; return now; }
static void Sample(rnet_u32 tick,RNetInputSample *out,void *ctx) {
    (void)tick;(void)ctx; memset(out,0,sizeof(*out)); out->size=2; out->valid=1;
}
static void Publish(rnet_u32 tick,const RNetInputSample *slots,int count,void *ctx) {
    (void)tick;(void)slots;(void)count;(void)ctx;
}
static void pump(RNetSession *a,RNetSession *b) {
    now+=8; rnet_session_pump(a); rnet_session_pump(b);
}
static void reassembly_tests(void) {
    RNetModsetRx rx={0}; RNetDecodedPacket packet;
    rnet_u8 bytes[RNET_MAX_PACKET];
    unsigned total=RNET_MODSET_TEXT_MAX-1u,offset,length;
    rnet_u32 checksum;
    int n,result;
    memset(settings,'x',total); settings[total]=0;
    settings[total-1]='y'; checksum=rnet_proto_checksum((const rnet_u8 *)settings,total);
    assert(rnet_proto_encode_modset(bytes,sizeof(bytes),1,2,0,settings)<0);
    /* Last-to-first arrival, with duplicates. No partial list is ready. */
    offset=(total-1u)/RNET_MODSET_CHUNK_BYTES*RNET_MODSET_CHUNK_BYTES;
    for (;;) {
        length=total-offset; if (length>RNET_MODSET_CHUNK_BYTES) length=RNET_MODSET_CHUNK_BYTES;
        n=rnet_proto_encode_modset_chunk(bytes,sizeof(bytes),1,2,0,(rnet_u16)total,
            (rnet_u16)offset,settings+offset,(rnet_u16)length,checksum);
        assert(n>0 && n<=RNET_MAX_PACKET);
        assert(!rnet_proto_decode(bytes,(size_t)n,1,&packet));
        result=rnet_modset_accept_chunk(&rx,packet.local_slot,packet.modset_total,
            packet.modset_offset,packet.modset_text,packet.modset_len,packet.modset_checksum);
        assert(result==(offset?0:1) && rx.ready==(!offset));
        assert(rnet_modset_accept_chunk(&rx,0,(rnet_u16)total,(rnet_u16)offset,
            settings+offset,(rnet_u16)length,checksum)==result);
        if (!offset) break;
        offset-=RNET_MODSET_CHUNK_BYTES;
    }
    assert(!strcmp(rx.text,settings));
    assert(rnet_modset_accept_chunk(&rx,0,(rnet_u16)total,1,settings,960,checksum)<0);
    assert(rnet_modset_accept_chunk(&rx,0,RNET_MODSET_TEXT_MAX,0,settings,960,checksum)<0);
    /* A new valid tuple invalidates the old pending result. */
    settings[total-1]='z'; checksum=rnet_proto_checksum((const rnet_u8 *)settings,total);
    assert(!rnet_modset_accept_chunk(&rx,0,(rnet_u16)total,0,settings,960,checksum) && !rx.ready);
    /* A conflicting duplicate invalidates partial data, then retries recover. */
    settings[0]='q';
    assert(rnet_modset_accept_chunk(&rx,0,(rnet_u16)total,0,settings,960,checksum)<0 && !rx.ready);
    settings[0]='x';
    for (offset=0;offset<total;offset+=960) {
        length=total-offset; if(length>960)length=960;
        result=rnet_modset_accept_chunk(&rx,0,(rnet_u16)total,(rnet_u16)offset,
            settings+offset,(rnet_u16)length,checksum);
    }
    assert(result==1 && !strcmp(rx.text,settings));
    /* A complete set with a bad aggregate checksum is never published. */
    memset(&rx,0,sizeof(rx));
    for (offset=0;offset<total;offset+=960) {
        length=total-offset; if(length>960)length=960;
        if(offset+length==total)settings[offset]='!';
        result=rnet_modset_accept_chunk(&rx,0,(rnet_u16)total,(rnet_u16)offset,
            settings+offset,(rnet_u16)length,checksum);
    }
    assert(result<0 && !rx.ready);
    settings[(total-1u)/960u*960u]='x';
    /* Checksummed packets with a malformed offset must still be rejected. */
    n=rnet_proto_encode_modset_chunk(bytes,sizeof(bytes),1,2,0,2526,0,settings,960,checksum);
    assert(n>0); bytes[14]=1;
    { rnet_u32 sum=rnet_proto_checksum(bytes,(size_t)n-4u); unsigned i;
      for(i=0;i<4;i++)bytes[n-4+i]=(rnet_u8)(sum>>(8*i)); }
    assert(rnet_proto_decode(bytes,(size_t)n,1,&packet)<0);
    /* Legacy small packets retain their exact limit, without prefixes. */
    memset(settings,'a',1023);settings[1023]=0;
    n=rnet_proto_encode_modset(bytes,sizeof(bytes),1,2,0,settings);
    assert(n>0 && !rnet_proto_decode(bytes,(size_t)n,1,&packet) && !strcmp(packet.modset_text,settings));
    settings[1023]='b';settings[1024]=0;
    assert(rnet_proto_encode_modset(bytes,sizeof(bytes),1,2,0,settings)<0);
}
static void wire_test(const char *text) {
    RNetConfig cfg; RNetHostVTable host={0}; RNetSession *a,*b;
    char addr_a[64],addr_b[64],received[RNET_MODSET_TEXT_MAX];
    unsigned port=20000u+((unsigned)test_pid()%15000u)*2u;
    int i,got=0;
    rnet_config_init_defaults(&cfg); cfg.local_slot=0; cfg.slot_count=2;
    host.sample_local=Sample;host.publish=Publish;host.now_ms=Clock;
    a=rnet_session_create(&cfg,&host);cfg.local_slot=1;b=rnet_session_create(&cfg,&host);
    assert(a && b);
    snprintf(addr_a,sizeof(addr_a),"127.0.0.1:%u",port);
    snprintf(addr_b,sizeof(addr_b),"127.0.0.1:%u",port+1);
    assert(!rnet_session_start_lan(a,addr_a,addr_b) && !rnet_session_start_lan(b,addr_b,addr_a));
    for(i=0;i<10000;i++) { pump(a,b); if(rnet_session_is_running(a)&&rnet_session_is_running(b))break; }
    assert(i<10000);
    for(i=0;i<10000;i++) {
        char tiny[2];
        if(i%8==0)assert(!rnet_session_send_modset(a,text));
        pump(a,b);
        assert(!rnet_session_take_modset(b,tiny,sizeof(tiny)));
        if(rnet_session_take_modset(b,received,sizeof(received))) { got=1;break; }
    }
    assert(got && !strcmp(received,text));
    assert(!rnet_session_sim_tick(a) && !rnet_session_sim_tick(b));
    rnet_session_destroy(a);rnet_session_destroy(b);
}
int main(int argc,char **argv) {
    reassembly_tests();
    if(argc==2) {
        FILE *f=fopen(argv[1],"rb");size_t n; assert(f);
        n=fread(settings,1,sizeof(settings),f);assert(!ferror(f)&&n<sizeof(settings));fclose(f);settings[n]=0;
    } else { memset(settings,'m',2526);settings[2526]=0; }
    wire_test(settings);
    puts("PASS complete settings reassembly, malformed/duplicate/conflict guards and actual UDP transfer before frame zero");
    return 0;
}
