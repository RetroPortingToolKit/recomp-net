/* Actual UDP socket contract tests. Separate-process launcher/session tests
 * are still required; this executable does not claim simulation coverage. */
#include "recomp_net/lan_room.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static void sleep_ms(void){Sleep(1);}
static unsigned long long now(void){return GetTickCount64();}
#else
#include <time.h>
static void sleep_ms(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
static unsigned long long now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000+t.tv_nsec/1000000;}
#endif
static RNetLanRoomPeer* p[5];
static void pump_all(void) {int i;for(i=0;i<5;++i)if(p[i])rnet_lan_room_pump(p[i]);sleep_ms();}
static unsigned count(const RNetLanRoom* room){unsigned i,n=0;for(i=0;i<4;++i)n+=room->member[i].id!=0;return n;}
static void settle(unsigned expected,unsigned ready) {
    unsigned long long deadline=now()+4000;
    for(;;) {
        int i,okay=1;const RNetLanRoom* host;
        pump_all();host=rnet_lan_room_state(p[0]);
        if(count(host)!=expected || host->ready_mask!=ready)okay=0;
        for(i=1;i<4;++i)if(p[i] && rnet_lan_room_local_slot(p[i])>=0) {
            const RNetLanRoom* guest=rnet_lan_room_state(p[i]);
            if(guest->revision!=host->revision || memcmp(guest,host,sizeof(*host)))okay=0;
        }
        if(okay)return;
        if(now()>deadline){
            fprintf(stderr,"settle failed count=%u ready=%u rev=%u\n",count(host),host->ready_mask,host->revision);
            for(i=1;i<4;++i)if(p[i]) {
                const RNetLanRoom* guest=rnet_lan_room_state(p[i]);size_t off;
                fprintf(stderr,"peer%d status=%d slot=%d rev=%u\n",i,rnet_lan_room_pump(p[i]),rnet_lan_room_local_slot(p[i]),guest->revision);
                for(off=0;off<sizeof(*host);++off)if(((const unsigned char*)host)[off]!=((const unsigned char*)guest)[off]) {
                    fprintf(stderr,"first byte %u host=%u guest=%u\n",(unsigned)off,((const unsigned char*)host)[off],((const unsigned char*)guest)[off]);break;
                }
            }
            assert(0);
        }
    }
}
static void open_guest(unsigned i,int cap,const char* password) {
    assert(!p[i]);assert(rnet_lan_room_guest_open(&p[i],"127.0.0.1:0","127.0.0.1:19971","Room test","test-v1",password,"Same name",cap)==0);
}
static void expect_reject(int expected) {
    unsigned long long deadline=now()+3000;int status;
    do {pump_all();status=rnet_lan_room_pump(p[4]);assert(now()<deadline);}while(status>=0);
    assert(status==expected);rnet_lan_room_close(&p[4]);
}
int main(void) {
    RNetLanRoom room;unsigned i;unsigned long long deadline;unsigned previous_revision;int previous_slot;
    memset(&room,0,sizeof(room));strcpy(room.name,"Four seats");strcpy(room.game,"Room test");strcpy(room.game_version,"test-v1");
    strcpy(room.endpoint,"127.0.0.1:19971");strcpy(room.member[0].name,"Same name");
    room.max_slots=4;room.input_delay=2;room.rollback=1;room.input_prediction=6;
    assert(rnet_lan_room_host_open(&p[0],"127.0.0.1:19971",&room,"private")==0);
    for(i=1;i<4;++i)open_guest(i,4,"private");
    settle(4,0);
    for(i=0;i<4;++i)assert(rnet_lan_room_set_ready(p[i],1)==0);
    settle(4,15);assert(rnet_lan_room_all_ready(p[0]));
    assert(rnet_lan_room_send_chat(p[0],"Host line")==0);
    assert(rnet_lan_room_send_chat(p[1],"Guest line")==0);
    {int received[4]={0};deadline=now()+3000;
        for(;;) {
            int done=1;pump_all();
            for(i=0;i<4;++i) {
                RNetLanChatLine line;
                while(rnet_lan_room_take_chat(p[i],&line)) {
                    assert(!strcmp(line.from,"Same name"));
                    assert(!strcmp(line.text,received[i]?"Guest line":"Host line"));++received[i];
                }
                done&=received[i]==2;
            }
            if(done)break;assert(now()<deadline);
        }
    }
    open_guest(4,4,"wrong");expect_reject(RNET_LAN_ROOM_PASSWORD);
    open_guest(4,2,"private");expect_reject(RNET_LAN_ROOM_IDENTITY);
    open_guest(4,4,"private");expect_reject(RNET_LAN_ROOM_FULL);
    assert(rnet_lan_room_set_caps(p[0],6,1,10,7,"package@1/feature value=2")==0);
    settle(4,0);assert(rnet_lan_room_start(p[0])==RNET_LAN_ROOM_NOT_READY);
    for(i=1;i<4;++i) {
        const RNetLanRoom* r=rnet_lan_room_state(p[i]);
        assert(r->input_delay==6 && r->input_prediction==10 && r->session_variant==7 && !strcmp(r->mod_set,"package@1/feature value=2"));
    }
    /* A ready sent for older caps cannot approve changed caps. */
    assert(rnet_lan_room_set_ready(p[1],1)==0);
    assert(rnet_lan_room_set_caps(p[0],7,1,11,8,"")==0);settle(4,0);
    /* A guest cannot move somebody else. A used destination is refused. */
    previous_slot=rnet_lan_room_local_slot(p[1]);
    assert(rnet_lan_room_move(p[1],0,previous_slot)==RNET_LAN_ROOM_ARGUMENT);
    assert(rnet_lan_room_move(p[0],0,previous_slot)==RNET_LAN_ROOM_OCCUPIED);
    /* Kick creates a hole; stable identities follow actual moves. */
    {int hole=rnet_lan_room_local_slot(p[3]);
        assert(rnet_lan_room_kick(p[0],hole)==0);
        deadline=now()+3000;while(rnet_lan_room_pump(p[3])!=RNET_LAN_ROOM_KICKED){pump_all();assert(now()<deadline);}
        rnet_lan_room_close(&p[3]);settle(3,0);
        assert(rnet_lan_room_move(p[0],0,hole)==0);settle(3,0);
        assert(rnet_lan_room_state(p[0])->host_slot==hole);
        assert(rnet_lan_room_move(p[1],previous_slot,0)==0);
        deadline=now()+3000;while(rnet_lan_room_local_slot(p[1])!=0){pump_all();assert(now()<deadline);}
        settle(3,0);
        assert(rnet_lan_room_local_slot(p[1])==0);
    }
    open_guest(3,4,"private");settle(4,0);
    /* Duplicate READY datagrams must not toggle or increment the roster. */
    assert(rnet_lan_room_set_ready(p[1],1)==0);
    deadline=now()+450;while(now()<deadline){rnet_lan_room_pump(p[1]);sleep_ms();}
    {unsigned bit=1u<<rnet_lan_room_local_slot(p[1]);settle(4,bit);previous_revision=rnet_lan_room_state(p[0])->revision;
        deadline=now()+250;while(now()<deadline)pump_all();assert(rnet_lan_room_state(p[0])->revision==previous_revision);
    }
    for(i=0;i<4;++i)assert(rnet_lan_room_set_ready(p[i],1)==0);
    settle(4,15);assert(rnet_lan_room_start(p[0])==0);
    deadline=now()+4000;
    while(!rnet_lan_room_launch_ready(p[0])){pump_all();assert(now()<deadline);}
    {const rnet_u32 session=rnet_lan_room_state(p[0])->session_id;
        assert(session);
        /* Host hands its socket to the actual game first. The listeners
         * must retain the agreed launch and survive that clean handoff. */
        rnet_lan_room_close(&p[0]);
        for(;;) {
            int ready=1;for(i=1;i<4;++i){assert(rnet_lan_room_pump(p[i])==0);ready&=rnet_lan_room_launch_ready(p[i]);}
            if(ready)break;sleep_ms();assert(now()<deadline);
        }
        for(i=1;i<4;++i){assert(rnet_lan_room_state(p[i])->session_id==session);rnet_lan_room_close(&p[i]);}
    }
    puts("lan_room_test: four sockets, same-name identities, readiness/caps, kick/moves, duplicate requests and start handoff passed");
    return 0;
}
