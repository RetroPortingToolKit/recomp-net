#include "recomp_net/lan_room.h"
#include "platform/rnet_platform.h"
#include "protocol/rnet_protocol.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>

enum { PKT_MAX=2048, HEADER=36, RETRY_MS=200, TIMEOUT_MS=10000, HANDOFF_MS=800 };
enum { JOIN=1,ROOM,ACK,OP,LEAVE,START,START_ACK,NAK,CLOSE,PING,PONG,CHAT_REQUEST,CHAT_ECHO };
enum { READY=1,MOVE=2 };
static const rnet_u8 magic[8]={'R','N','E','T','L','R','4',1};
typedef struct Buffer {rnet_u8 bytes[PKT_MAX];size_t at,end;int valid;} Buffer;
typedef struct MemberLink {
    struct sockaddr_in address;
    rnet_u32 id,sequence,room_ack,start_ack;
    rnet_u32 chat_sequence;
    rnet_u64 seen,ping;
    int rtt;
} MemberLink;
struct RNetLanRoomPeer {
    rnet_socket socket;
    struct sockaddr_in host_address;
    int host,status,joined,max_supported,want_start;
    rnet_u32 id,sequence,pending_op,pending_a,pending_b,pending_revision;
    rnet_u32 retired[16];unsigned retired_at;
    rnet_u64 opened,last_send,last_host,start_seen,start_sent;
    char game[RNET_LAN_LOBBY_GAME_MAX],version[RNET_LAN_LOBBY_VERSION_MAX];
    char password[RNET_LAN_LOBBY_PASSWORD_MAX],name[RNET_LAN_LOBBY_PLAYER_MAX];
    RNetLanRoom room;
    MemberLink links[RNET_LAN_ROOM_MAX];
    rnet_u32 chat_sequence,chat_seen;
    RNetLanChatLine chat[RNET_LAN_CHAT_QUEUE];int chat_head,chat_count;
};
static void put32(rnet_u8* p,rnet_u32 v){unsigned i;for(i=0;i<4;++i)p[i]=(rnet_u8)(v>>(8*i));}
static rnet_u32 get32(const rnet_u8* p){return p[0]|(rnet_u32)p[1]<<8|(rnet_u32)p[2]<<16|(rnet_u32)p[3]<<24;}
static void number(Buffer* b,rnet_u32 v) {
    if(!b->valid || b->at+4>b->end){b->valid=0;return;}put32(b->bytes+b->at,v);b->at+=4;
}
static rnet_u32 read_number(Buffer* b) {
    rnet_u32 v;if(!b->valid || b->at+4>b->end){b->valid=0;return 0;}
    v=get32(b->bytes+b->at);b->at+=4;return v;
}
static int copy_string(char* out,size_t cap,const char* in) {
    const char* end;size_t i,n;if(!out || !cap || !in)return 0;
    end=(const char*)memchr(in,0,cap);if(!end)return 0;n=(size_t)(end-in);
    for(i=0;i<n;++i)if((unsigned char)in[i]<32 || in[i]==127)return 0;
    memmove(out,in,n+1);memset(out+n+1,0,cap-n-1);return 1;
}
static void string(Buffer* b,const char* in,size_t cap) {
    const char* end=(const char*)memchr(in,0,cap);size_t n;
    if(!end){b->valid=0;return;}n=(size_t)(end-in);number(b,(rnet_u32)n);
    if(!b->valid || b->at+n>b->end){b->valid=0;return;}
    memcpy(b->bytes+b->at,in,n);b->at+=n;
}
static void read_string(Buffer* b,char* out,size_t cap) {
    rnet_u32 n=read_number(b);unsigned i;
    if(!b->valid || n>=cap || n>b->end-b->at){b->valid=0;return;}
    for(i=0;i<n;++i)if(b->bytes[b->at+i]<32 || b->bytes[b->at+i]==127){b->valid=0;return;}
    memcpy(out,b->bytes+b->at,n);out[n]=0;b->at+=n;
}
static rnet_u32 occupied(const RNetLanRoom* r) {
    rnet_u32 bits=0;int i;for(i=0;i<r->max_slots;++i)if(r->member[i].id)bits|=1u<<i;return bits;
}
static int member_count(const RNetLanRoom* r) {
    int i,n=0;for(i=0;i<r->max_slots;++i)n+=r->member[i].id!=0;return n;
}
static int slot_of(const RNetLanRoomPeer* p,rnet_u32 id) {
    int i;for(i=0;i<p->room.max_slots;++i)if(p->room.member[i].id==id)return i;return -1;
}
static int valid_room(const RNetLanRoom* r) {
    int i,j;
    if(r->max_slots<2 || r->max_slots>4 || r->host_slot<0 || r->host_slot>=r->max_slots
       || !r->member[r->host_slot].id || !r->epoch || !r->revision || !r->configuration || r->configuration>r->revision
       || r->ready_mask&~occupied(r) || r->input_delay<2 || r->input_delay>20
       || r->rollback<0 || r->rollback>1 || r->input_prediction<2 || r->input_prediction>16
       || r->session_variant<0 || r->starting<0 || r->starting>1 || (r->starting && !r->session_id))return 0;
    for(i=0;i<4;++i) {
        if((i>=r->max_slots && r->member[i].id) || (r->member[i].id && !r->member[i].name[0]))return 0;
        if(r->member[i].id)for(j=i+1;j<4;++j)if(r->member[i].id==r->member[j].id)return 0;
    }
    return r->game[0] && r->game_version[0] && r->endpoint[0];
}
static void write_room(Buffer* b,const RNetLanRoom* r) {
    int i;
#define STR(field) string(b,r->field,sizeof(r->field))
    STR(name);STR(game);STR(game_version);STR(endpoint);STR(mod_set);
#undef STR
    number(b,r->max_slots);number(b,r->host_slot);number(b,r->ready_mask);number(b,r->configuration);number(b,r->session_id);
    number(b,r->input_delay);number(b,r->rollback);number(b,r->input_prediction);number(b,r->session_variant);number(b,r->starting);
    for(i=0;i<4;++i){number(b,r->member[i].id);string(b,r->member[i].name,sizeof(r->member[i].name));}
}
static void read_room(Buffer* b,RNetLanRoom* r) {
    int i;
#define STR(field) read_string(b,r->field,sizeof(r->field))
    STR(name);STR(game);STR(game_version);STR(endpoint);STR(mod_set);
#undef STR
    r->max_slots=(int)read_number(b);r->host_slot=(int)read_number(b);r->ready_mask=read_number(b);r->configuration=read_number(b);r->session_id=read_number(b);
    r->input_delay=(int)read_number(b);r->rollback=(int)read_number(b);r->input_prediction=(int)read_number(b);
    r->session_variant=(int)read_number(b);r->starting=(int)read_number(b);
    for(i=0;i<4;++i){r->member[i].id=read_number(b);read_string(b,r->member[i].name,sizeof(r->member[i].name));}
}
static void packet(Buffer* b,rnet_u32 op,rnet_u32 epoch,rnet_u32 id,rnet_u32 sequence,rnet_u32 revision,int status) {
    memset(b,0,sizeof(*b));b->valid=1;b->end=PKT_MAX;b->at=8;memcpy(b->bytes,magic,8);
    number(b,op);number(b,epoch);number(b,id);number(b,sequence);number(b,revision);number(b,(rnet_u32)status);number(b,0);
}
static int send_packet(RNetLanRoomPeer* p,const struct sockaddr_in* address,Buffer* b) {
    if(!b->valid)return RNET_LAN_ROOM_ARGUMENT;
    put32(b->bytes+32,rnet_proto_checksum(b->bytes,b->at));
    return rnet_os_sendto(p->socket,b->bytes,b->at,address)<0?RNET_LAN_ROOM_IO:0;
}
static void send_simple(RNetLanRoomPeer* p,const struct sockaddr_in* address,int op,rnet_u32 id,int status) {
    Buffer b;packet(&b,op,p->room.epoch,id,p->sequence,p->room.revision,status);send_packet(p,address,&b);
}
static void send_state(RNetLanRoomPeer* p,int slot,int status) {
    Buffer b;MemberLink* link=&p->links[slot];
    packet(&b,p->room.starting?START:ROOM,p->room.epoch,link->id,link->sequence,p->room.revision,status);
    write_room(&b,&p->room);send_packet(p,&link->address,&b);
}
static void changed(RNetLanRoomPeer* p) {
    if(p->room.revision==UINT_MAX){p->status=RNET_LAN_ROOM_CLOSED;return;}
    ++p->room.revision;p->last_send=0;p->want_start=0;
}
static void configuration_changed(RNetLanRoomPeer* p) {
    changed(p);p->room.configuration=p->room.revision;p->room.ready_mask=0;
}
static int same_address(const struct sockaddr_in* a,const struct sockaddr_in* b) {
    return a->sin_addr.s_addr==b->sin_addr.s_addr && a->sin_port==b->sin_port;
}
static int resolve(const char* text,struct sockaddr_in* address) {
    char host[128];rnet_u16 port;
    return rnet_os_parse_hostport(text,host,sizeof(host),&port)==0
        && rnet_os_resolve_sockaddr(host,port,address)==0;
}
static int allocate(RNetLanRoomPeer** out,const char* bind) {
    RNetLanRoomPeer* p;struct sockaddr_in address;
    rnet_os_startup();
    if(!out || *out || !bind || !resolve(bind,&address))return RNET_LAN_ROOM_ARGUMENT;
    p=(RNetLanRoomPeer*)calloc(1,sizeof(*p));if(!p)return RNET_LAN_ROOM_IO;
    p->socket=rnet_os_socket_create_dgram();p->opened=p->last_host=rnet_os_monotonic_ms();
    if(!rnet_os_socket_valid(p->socket) || rnet_os_bind(p->socket,&address)
       || rnet_os_set_nonblocking(p->socket) || rnet_os_random_bytes(&p->id,sizeof(p->id)) || !p->id) {
        rnet_os_socket_destroy(&p->socket);free(p);return RNET_LAN_ROOM_IO;
    }
    p->status=RNET_LAN_ROOM_PENDING;*out=p;return 0;
}
int rnet_lan_room_host_open(RNetLanRoomPeer** out,const char* bind,const RNetLanRoom* config,const char* password) {
    RNetLanRoomPeer* p;int status,i;
    if(!config || config->max_slots<2 || config->max_slots>4 || config->host_slot<0
       || config->host_slot>=config->max_slots)return RNET_LAN_ROOM_ARGUMENT;
    status=allocate(out,bind);if(status)return status;p=*out;p->host=1;p->room=*config;
    memset(p->room.member,0,sizeof(p->room.member));
    if(!copy_string(p->password,sizeof(p->password),password?password:"")
       || !copy_string(p->room.member[config->host_slot].name,sizeof(p->name),config->member[config->host_slot].name))goto bad;
    p->room.member[config->host_slot].id=p->id;p->room.epoch=p->id;p->room.revision=p->room.configuration=1;
    p->room.ready_mask=p->room.session_id=0;p->room.starting=0;
    if(!valid_room(&p->room))goto bad;
    {Buffer b;packet(&b,ROOM,p->id,p->id,0,1,0);write_room(&b,&p->room);if(!b.valid)goto bad;}
    for(i=0;i<4;++i)p->links[i].rtt=-1;
    p->joined=1;p->status=0;return 0;
bad:
    rnet_lan_room_close(out);return RNET_LAN_ROOM_ARGUMENT;
}
int rnet_lan_room_guest_open(RNetLanRoomPeer** out,const char* bind,const char* host,const char* game,
                             const char* version,const char* password,const char* name,int slots) {
    RNetLanRoomPeer* p;int status;if(slots<2 || slots>4)return RNET_LAN_ROOM_ARGUMENT;
    status=allocate(out,bind);if(status)return status;p=*out;p->max_supported=slots;
    if(!resolve(host,&p->host_address) || !copy_string(p->game,sizeof(p->game),game)
       || !copy_string(p->version,sizeof(p->version),version) || !copy_string(p->password,sizeof(p->password),password?password:"")
       || !copy_string(p->name,sizeof(p->name),name) || !p->game[0] || !p->version[0] || !p->name[0]) {
        rnet_lan_room_close(out);return RNET_LAN_ROOM_ARGUMENT;
    }
    return 0;
}
static void remove_member(RNetLanRoomPeer* p,int slot,int reason) {
    send_simple(p,&p->links[slot].address,CLOSE,p->links[slot].id,reason);
    p->retired[p->retired_at++%16]=p->links[slot].id;
    memset(&p->links[slot],0,sizeof(p->links[slot]));p->links[slot].rtt=-1;
    memset(&p->room.member[slot],0,sizeof(p->room.member[slot]));p->room.ready_mask&=~(1u<<slot);
    p->room.starting=0;p->room.session_id=0;configuration_changed(p);
}
static int move_member(RNetLanRoomPeer* p,int from,int to) {
    if(p->room.starting)return RNET_LAN_ROOM_STARTED;
    if(from<0 || from>=p->room.max_slots || to<0 || to>=p->room.max_slots || !p->room.member[from].id)return RNET_LAN_ROOM_ARGUMENT;
    if(from==to)return 0;
    if(p->room.member[to].id)return RNET_LAN_ROOM_OCCUPIED;
    p->room.member[to]=p->room.member[from];memset(&p->room.member[from],0,sizeof(p->room.member[from]));
    p->links[to]=p->links[from];memset(&p->links[from],0,sizeof(p->links[from]));p->links[from].rtt=-1;
    if(p->room.host_slot==from)p->room.host_slot=to;
    configuration_changed(p);return 0;
}
static void queue_chat(RNetLanRoomPeer* p,rnet_u32 sender,const char* name,const char* text) {
    int index;RNetLanChatLine* line;
    if(p->chat_count<RNET_LAN_CHAT_QUEUE){index=(p->chat_head+p->chat_count)%RNET_LAN_CHAT_QUEUE;++p->chat_count;}
    else {index=p->chat_head;p->chat_head=(p->chat_head+1)%RNET_LAN_CHAT_QUEUE;}
    line=&p->chat[index];memset(line,0,sizeof(*line));
    snprintf(line->player_id,sizeof(line->player_id),"lan:%08x",(unsigned)sender);
    snprintf(line->from,sizeof(line->from),"%s",name);snprintf(line->text,sizeof(line->text),"%s",text);
}
static int broadcast_chat(RNetLanRoomPeer* p,int sender,const char* text) {
    int i;const RNetLanRoomMember* member=&p->room.member[sender];
    if(p->chat_sequence==UINT_MAX)return RNET_LAN_ROOM_ARGUMENT;
    ++p->chat_sequence;queue_chat(p,member->id,member->name,text);
    for(i=0;i<p->room.max_slots;++i)if(p->links[i].id) {
        Buffer b;packet(&b,CHAT_ECHO,p->room.epoch,p->links[i].id,p->chat_sequence,p->room.revision,0);
        number(&b,member->id);string(&b,member->name,sizeof(member->name));string(&b,text,RNET_LAN_CHAT_TEXT_LEN);
        send_packet(p,&p->links[i].address,&b);
    }
    return 0;
}
static void handle_host(RNetLanRoomPeer* p,Buffer* b,const struct sockaddr_in* source,int type,
                        rnet_u32 epoch,rnet_u32 id,rnet_u32 sequence,rnet_u32 revision) {
    int slot=slot_of(p,id),i,status=0;const rnet_u64 now=rnet_os_monotonic_ms();
    if(type==JOIN) {
        char game[64],version[32],password[RNET_LAN_LOBBY_PASSWORD_MAX],name[RNET_LAN_LOBBY_PLAYER_MAX];
        int supported;
        read_string(b,game,sizeof(game));read_string(b,version,sizeof(version));read_string(b,password,sizeof(password));
        read_string(b,name,sizeof(name));supported=(int)read_number(b);
        if(!b->valid || b->at!=b->end || !id || !name[0])return;
        if(strcmp(game,p->room.game) || strcmp(version,p->room.game_version) || supported<p->room.max_slots)status=RNET_LAN_ROOM_IDENTITY;
        else if(strcmp(password,p->password))status=RNET_LAN_ROOM_PASSWORD;
        else if(p->room.starting)status=RNET_LAN_ROOM_STARTED;
        for(i=0;i<16;++i)if(p->retired[i]==id)status=RNET_LAN_ROOM_STALE;
        if(!status && slot>=0) {
            if(slot==p->room.host_slot || !same_address(source,&p->links[slot].address))status=RNET_LAN_ROOM_IDENTITY;
            else {p->links[slot].seen=now;send_state(p,slot,0);return;}
        }
        if(!status)for(i=0;i<p->room.max_slots;++i)
            if(p->links[i].id && same_address(source,&p->links[i].address))status=RNET_LAN_ROOM_IDENTITY;
        if(!status) {
            for(slot=0;slot<p->room.max_slots && p->room.member[slot].id;++slot){}
            if(slot==p->room.max_slots)status=RNET_LAN_ROOM_FULL;
        }
        if(status){send_simple(p,source,NAK,id,status);return;}
        p->room.member[slot].id=id;memcpy(p->room.member[slot].name,name,strlen(name)+1);
        p->links[slot].id=id;p->links[slot].address=*source;p->links[slot].seen=now;p->links[slot].rtt=-1;
        configuration_changed(p);send_state(p,slot,0);return;
    }
    if(epoch!=p->room.epoch || slot<0 || slot==p->room.host_slot
       || !same_address(source,&p->links[slot].address))return;
    p->links[slot].seen=now;
    if(type==ACK || type==START_ACK) {
        if(b->at!=b->end)return;
        if(revision==p->room.revision) {
            p->links[slot].room_ack=revision;
            if(type==START_ACK && p->room.starting)p->links[slot].start_ack=revision;
        }
    } else if(type==LEAVE) {
        if(b->at==b->end)remove_member(p,slot,RNET_LAN_ROOM_CLOSED);
    } else if(type==OP) {
        rnet_u32 op=read_number(b),a=read_number(b),c=read_number(b);
        if(!b->valid || b->at!=b->end || !sequence)return;
        if(sequence<=p->links[slot].sequence){send_state(p,slot,0);return;}
        p->links[slot].sequence=sequence;
        if(p->room.starting)status=RNET_LAN_ROOM_STARTED;
        else if(op==READY && a<=1 && c==p->room.configuration) {
            const rnet_u32 old=p->room.ready_mask;
            if(a)p->room.ready_mask|=1u<<slot;else p->room.ready_mask&=~(1u<<slot);
            if(old!=p->room.ready_mask)changed(p);
        } else if(revision!=p->room.revision || (op==READY && c!=p->room.configuration))status=RNET_LAN_ROOM_STALE;
        else if(op==MOVE && a==(rnet_u32)slot && c<4)status=move_member(p,slot,(int)c);
        else status=RNET_LAN_ROOM_ARGUMENT;
        slot=slot_of(p,id);if(slot>=0)send_state(p,slot,status);
    } else if(type==CHAT_REQUEST) {
        char text[RNET_LAN_CHAT_TEXT_LEN];read_string(b,text,sizeof(text));
        if(!b->valid || b->at!=b->end || !text[0] || !sequence || sequence<=p->links[slot].chat_sequence)return;
        p->links[slot].chat_sequence=sequence;broadcast_chat(p,slot,text);
    } else if(type==PONG && b->at==b->end && p->links[slot].ping) {
        rnet_u64 age=now-p->links[slot].ping;p->links[slot].rtt=(int)(age>60000?60000:age);p->links[slot].ping=0;
    }
}
static void handle_guest(RNetLanRoomPeer* p,Buffer* b,const struct sockaddr_in* source,int type,
                         rnet_u32 epoch,rnet_u32 id,rnet_u32 sequence,rnet_u32 revision,int status) {
    RNetLanRoom next;int local;
    if(id!=p->id || !same_address(source,&p->host_address))return;
    if(type==NAK && !p->joined){p->status=status<0?status:RNET_LAN_ROOM_IDENTITY;return;}
    if(p->joined && epoch!=p->room.epoch)return;
    if(type==CLOSE && p->joined){p->status=status<0?status:RNET_LAN_ROOM_CLOSED;return;}
    if(type==PING && p->joined){send_simple(p,&p->host_address,PONG,p->id,0);return;}
    if(type==CHAT_ECHO && p->joined) {
        rnet_u32 sender=read_number(b);char name[RNET_LAN_LOBBY_PLAYER_MAX],text[RNET_LAN_CHAT_TEXT_LEN];
        read_string(b,name,sizeof(name));read_string(b,text,sizeof(text));
        if(!b->valid || b->at!=b->end || !sender || !text[0] || sequence<=p->chat_seen)return;
        p->chat_seen=sequence;queue_chat(p,sender,name,text);return;
    }
    if(type!=ROOM && type!=START)return;
    memset(&next,0,sizeof(next));next.epoch=epoch;next.revision=revision;read_room(b,&next);
    if(!b->valid || b->at!=b->end || !valid_room(&next) || next.max_slots>p->max_supported
       || strcmp(next.game,p->game) || strcmp(next.game_version,p->version))return;
    if(p->joined && revision<p->room.revision)return;
    if(p->joined && revision==p->room.revision && memcmp(&next,&p->room,sizeof(next))) {
        p->status=RNET_LAN_ROOM_IDENTITY;return;
    }
    local=-1;{int i;for(i=0;i<next.max_slots;++i)if(next.member[i].id==p->id)local=i;}
    if(local<0){p->status=RNET_LAN_ROOM_KICKED;return;}
    if(type==START && next.starting && (!p->room.starting || p->room.session_id!=next.session_id))p->start_seen=rnet_os_monotonic_ms();
    if(!next.starting)p->start_seen=0;
    p->room=next;p->joined=1;p->status=0;p->last_host=rnet_os_monotonic_ms();
    if(sequence>=p->sequence)p->pending_op=0;
    /* A stale request is acknowledged without mutation. The caller sees the
     * new authoritative roster and may submit its desired operation again. */
    (void)status;
    if(type==START && next.starting) {
        if(!p->start_seen)p->start_seen=p->last_host;
        send_simple(p,&p->host_address,START_ACK,p->id,0);
    } else send_simple(p,&p->host_address,ACK,p->id,0);
}
static void send_guest(RNetLanRoomPeer* p) {
    Buffer b;
    if(!p->joined) {
        packet(&b,JOIN,0,p->id,0,0,0);string(&b,p->game,sizeof(p->game));string(&b,p->version,sizeof(p->version));
        string(&b,p->password,sizeof(p->password));string(&b,p->name,sizeof(p->name));number(&b,p->max_supported);
    } else if(p->pending_op) {
        packet(&b,OP,p->room.epoch,p->id,p->sequence,p->pending_revision,0);
        number(&b,p->pending_op);number(&b,p->pending_a);number(&b,p->pending_b);
    } else packet(&b,p->room.starting?START_ACK:ACK,p->room.epoch,p->id,p->sequence,p->room.revision,0);
    send_packet(p,&p->host_address,&b);
}
int rnet_lan_room_pump(RNetLanRoomPeer* p) {
    rnet_u64 now;int i,blocked,n;
    if(!p)return RNET_LAN_ROOM_ARGUMENT;
    if(p->status<0)return p->status;
    now=rnet_os_monotonic_ms();
    for(i=0;i<256;++i) {
        Buffer b;struct sockaddr_in source;rnet_u32 type,epoch,id,sequence,revision,checksum;int status;
        n=rnet_os_recvfrom(p->socket,b.bytes,PKT_MAX,&source,&blocked);if(n<0)break;
        if(n<HEADER || n>=PKT_MAX || memcmp(b.bytes,magic,8))continue;
        checksum=get32(b.bytes+32);put32(b.bytes+32,0);
        if(checksum!=rnet_proto_checksum(b.bytes,(size_t)n))continue;
        b.at=8;b.end=(size_t)n;b.valid=1;type=read_number(&b);epoch=read_number(&b);id=read_number(&b);
        sequence=read_number(&b);revision=read_number(&b);status=(int)read_number(&b);read_number(&b);
        if(p->host)handle_host(p,&b,&source,(int)type,epoch,id,sequence,revision);
        else handle_guest(p,&b,&source,(int)type,epoch,id,sequence,revision,status);
    }
    if(p->status<0)return p->status;
    /* Receive handlers stamp liveness while draining. Read the clock again:
     * subtracting their newer timestamp from the pre-drain unsigned time
     * would manufacture a timeout when the millisecond counter advances. */
    now=rnet_os_monotonic_ms();
    if(!p->host && now-p->last_host>TIMEOUT_MS){p->status=RNET_LAN_ROOM_TIMEOUT;return p->status;}
    if(p->host && !p->room.starting)for(i=0;i<p->room.max_slots;++i)
        if(p->links[i].id && now-p->links[i].seen>TIMEOUT_MS)remove_member(p,i,RNET_LAN_ROOM_TIMEOUT);
    if(p->host && p->want_start) {
        int acked=1;for(i=0;i<p->room.max_slots;++i)if(p->links[i].id && p->links[i].room_ack!=p->room.revision)acked=0;
        if(acked && rnet_lan_room_all_ready(p)) {
            rnet_u32 session=0;
            if(rnet_os_random_bytes(&session,sizeof(session)) || !session || session==p->room.session_id){p->status=RNET_LAN_ROOM_IO;return p->status;}
            p->room.session_id=session;changed(p);p->room.starting=1;p->start_sent=now;
        }
    }
    if(p->host && p->room.starting && now-p->start_sent>TIMEOUT_MS && !rnet_lan_room_launch_ready(p)) {
        p->status=RNET_LAN_ROOM_TIMEOUT;return p->status;
    }
    if(!p->last_send || now-p->last_send>=RETRY_MS) {
        if(p->host)for(i=0;i<p->room.max_slots;++i)if(p->links[i].id) {
            send_state(p,i,0);
            if(!p->links[i].ping){p->links[i].ping=now;send_simple(p,&p->links[i].address,PING,p->links[i].id,0);}
        }
        if(!p->host)send_guest(p);
        p->last_send=now;
    }
    return p->joined?0:RNET_LAN_ROOM_PENDING;
}
void rnet_lan_room_close(RNetLanRoomPeer** out) {
    RNetLanRoomPeer* p;int i;if(!out || !*out)return;p=*out;
    if(!rnet_lan_room_launch_ready(p)) {
        if(p->host)for(i=0;i<p->room.max_slots;++i)if(p->links[i].id)send_simple(p,&p->links[i].address,CLOSE,p->links[i].id,RNET_LAN_ROOM_CLOSED);
        if(!p->host && p->joined)send_simple(p,&p->host_address,LEAVE,p->id,0);
    }
    rnet_os_socket_destroy(&p->socket);memset(p->password,0,sizeof(p->password));free(p);*out=NULL;
}
const RNetLanRoom* rnet_lan_room_state(const RNetLanRoomPeer* p){return p?&p->room:NULL;}
int rnet_lan_room_local_slot(const RNetLanRoomPeer* p){return p&&p->joined?slot_of(p,p->id):-1;}
int rnet_lan_room_is_host(const RNetLanRoomPeer* p){return p&&p->host;}
int rnet_lan_room_all_ready(const RNetLanRoomPeer* p) {
    return p && p->joined && member_count(&p->room)>=2 && (p->room.ready_mask&occupied(&p->room))==occupied(&p->room);
}
static int request(RNetLanRoomPeer* p,rnet_u32 op,rnet_u32 a,rnet_u32 b) {
    if(!p || !p->joined || p->status<0)return RNET_LAN_ROOM_ARGUMENT;
    if(p->room.starting)return RNET_LAN_ROOM_STARTED;
    if(p->sequence==UINT_MAX)return RNET_LAN_ROOM_ARGUMENT;
    ++p->sequence;p->pending_op=op;p->pending_a=a;p->pending_b=b;p->pending_revision=p->room.revision;p->last_send=0;return 0;
}
int rnet_lan_room_set_ready(RNetLanRoomPeer* p,int ready) {
    int slot;if(!p || !p->joined || p->room.starting)return RNET_LAN_ROOM_ARGUMENT;
    slot=slot_of(p,p->id);
    if(p->host) {
        rnet_u32 previous=p->room.ready_mask;
        if(ready)p->room.ready_mask|=1u<<slot;else p->room.ready_mask&=~(1u<<slot);
        if(previous!=p->room.ready_mask)changed(p);
        return 0;
    }
    return request(p,READY,ready!=0,p->room.configuration);
}
int rnet_lan_room_move(RNetLanRoomPeer* p,int from,int to) {
    if(!p || !p->joined)return RNET_LAN_ROOM_ARGUMENT;
    if(p->host)return move_member(p,from,to);
    if(from!=slot_of(p,p->id) || to<0 || to>=p->room.max_slots)return RNET_LAN_ROOM_ARGUMENT;
    if(p->room.member[to].id)return RNET_LAN_ROOM_OCCUPIED;
    return request(p,MOVE,(rnet_u32)from,(rnet_u32)to);
}
int rnet_lan_room_kick(RNetLanRoomPeer* p,int slot) {
    if(!p || !p->host || slot<0 || slot>=p->room.max_slots || slot==p->room.host_slot || !p->links[slot].id || p->room.starting)return RNET_LAN_ROOM_ARGUMENT;
    remove_member(p,slot,RNET_LAN_ROOM_KICKED);return 0;
}
int rnet_lan_room_set_caps(RNetLanRoomPeer* p,int delay,int rollback,int prediction,int variant,const char* mods) {
    RNetLanRoom next;
    if(!p || !p->host || p->room.starting)return RNET_LAN_ROOM_ARGUMENT;
    next=p->room;next.input_delay=delay;next.rollback=rollback;next.input_prediction=prediction;next.session_variant=variant;
    if(!copy_string(next.mod_set,sizeof(next.mod_set),mods?mods:"") || !valid_room(&next))return RNET_LAN_ROOM_ARGUMENT;
    if(memcmp(&next,&p->room,sizeof(next))){p->room=next;configuration_changed(p);}return 0;
}
int rnet_lan_room_start(RNetLanRoomPeer* p) {
    if(!p || !p->host || p->room.starting || !rnet_lan_room_all_ready(p))return RNET_LAN_ROOM_NOT_READY;
    p->want_start=1;return 0;
}
int rnet_lan_room_launch_ready(const RNetLanRoomPeer* p) {
    int i;if(!p || !p->joined || p->status<0 || !p->room.starting)return 0;
    if(!p->host)return p->start_seen && rnet_os_monotonic_ms()-p->start_seen>=HANDOFF_MS;
    for(i=0;i<p->room.max_slots;++i)if(p->links[i].id && p->links[i].start_ack!=p->room.revision)return 0;
    return 1;
}
int rnet_lan_room_member_rtt(const RNetLanRoomPeer* p,int slot) {
    return p && p->host && slot>=0 && slot<p->room.max_slots && p->links[slot].id?p->links[slot].rtt:-1;
}
int rnet_lan_room_send_chat(RNetLanRoomPeer* p,const char* text) {
    char copy[RNET_LAN_CHAT_TEXT_LEN];Buffer b;
    if(!p || !p->joined || p->status<0 || !copy_string(copy,sizeof(copy),text) || !copy[0])return RNET_LAN_ROOM_ARGUMENT;
    if(p->host)return broadcast_chat(p,slot_of(p,p->id),copy);
    if(p->chat_sequence==UINT_MAX)return RNET_LAN_ROOM_ARGUMENT;
    ++p->chat_sequence;packet(&b,CHAT_REQUEST,p->room.epoch,p->id,p->chat_sequence,p->room.revision,0);
    string(&b,copy,sizeof(copy));return send_packet(p,&p->host_address,&b);
}
int rnet_lan_room_take_chat(RNetLanRoomPeer* p,RNetLanChatLine* out) {
    if(!p || !out || !p->chat_count)return 0;
    *out=p->chat[p->chat_head];p->chat_head=(p->chat_head+1)%RNET_LAN_CHAT_QUEUE;--p->chat_count;return 1;
}
