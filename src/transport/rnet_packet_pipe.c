/* SPDX-License-Identifier: MIT */
#include "recomp_net/packet_pipe.h"
#include "protocol/rnet_protocol.h"
#include <stdlib.h>
#include <string.h>

int rnet_packet_source_valid(const rnet_u8 *bytes,size_t size,rnet_u32 magic,
                             rnet_u32 session_id,int source_slot,int slot_count) {
    RNetDecodedPacket packet;
    if(slot_count<2 || slot_count>RNET_MAX_SLOTS || source_slot<0 || source_slot>=slot_count ||
       size>RNET_PACKET_PIPE_MAX || rnet_proto_decode(bytes,size,magic,&packet)!=0 ||
       packet.session_id!=session_id)return 0;
    if(packet.type==RNET_PKT_START || packet.type==RNET_PKT_DELAY_SYNC)return source_slot==0;
    return packet.local_slot==source_slot;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

typedef struct PipePacket {rnet_u16 size;rnet_u8 bytes[RNET_PACKET_PIPE_MAX];} PipePacket;
typedef struct PipeQueue {
    PipePacket rows[RNET_PACKET_PIPE_CAPACITY];
    unsigned first,count;
} PipeQueue;
struct RNetPacketPipe {
    HANDLE input,output,reader,writer,wake;
    CRITICAL_SECTION lock;
    volatile LONG error;
    rnet_u8 path;
    PipeQueue incoming,outgoing;
};
static void pipe_fail(RNetPacketPipe *p,LONG why) {
    InterlockedCompareExchange(&p->error,why,RNET_PIPE_OK);
    SetEvent(p->wake);
}
int rnet_packet_pipe_error(const RNetPacketPipe *p) {
    return p ? (int)InterlockedCompareExchange((volatile LONG*)&p->error,0,0):RNET_PIPE_CLOSED;
}
static int pipe_read(RNetPacketPipe *p,void *bytes,DWORD size) {
    rnet_u8 *cursor=bytes;
    while(size && !rnet_packet_pipe_error(p)) {
        DWORD got=0;
        if(!ReadFile(p->input,cursor,size,&got,NULL) || !got) {
            pipe_fail(p,RNET_PIPE_IO);return 0;
        }
        cursor+=got;size-=got;
    }
    return size==0;
}
static int pipe_write(RNetPacketPipe *p,const void *bytes,DWORD size) {
    const rnet_u8 *cursor=bytes;
    while(size && !rnet_packet_pipe_error(p)) {
        DWORD sent=0;
        if(!WriteFile(p->output,cursor,size,&sent,NULL) || !sent) {
            pipe_fail(p,RNET_PIPE_IO);return 0;
        }
        cursor+=sent;size-=sent;
    }
    return size==0;
}
static unsigned __stdcall pipe_reader(void *user) {
    RNetPacketPipe *p=user;
    while(!rnet_packet_pipe_error(p)) {
        rnet_u8 header[8];PipePacket packet;
        if(!pipe_read(p,header,sizeof(header)))break;
        packet.size=(rnet_u16)(((rnet_u16)header[4]<<8)|header[5]);
        if(memcmp(header,"RNP1",4) || header[6] || header[7] || !packet.size ||
           packet.size>RNET_PACKET_PIPE_MAX) {pipe_fail(p,RNET_PIPE_FRAME);break;}
        if(!pipe_read(p,packet.bytes,packet.size))break;
        EnterCriticalSection(&p->lock);
        if(p->incoming.count==RNET_PACKET_PIPE_CAPACITY)pipe_fail(p,RNET_PIPE_OVERFLOW);
        else {
            PipePacket *to=&p->incoming.rows[(p->incoming.first+p->incoming.count)%RNET_PACKET_PIPE_CAPACITY];
            to->size=packet.size;memcpy(to->bytes,packet.bytes,packet.size);
            ++p->incoming.count;
        }
        LeaveCriticalSection(&p->lock);
    }
    return 0;
}
static unsigned __stdcall pipe_writer(void *user) {
    RNetPacketPipe *p=user;
    while(!rnet_packet_pipe_error(p)) {
        PipePacket packet;int have=0;
        EnterCriticalSection(&p->lock);
        if(p->outgoing.count) {
            const PipePacket *from=&p->outgoing.rows[p->outgoing.first];
            packet.size=from->size;memcpy(packet.bytes,from->bytes,packet.size);have=1;
            p->outgoing.first=(p->outgoing.first+1)%RNET_PACKET_PIPE_CAPACITY;--p->outgoing.count;
        }
        LeaveCriticalSection(&p->lock);
        if(!have) {WaitForSingleObject(p->wake,INFINITE);continue;}
        const rnet_u8 header[8]={'R','N','P','1',(rnet_u8)(packet.size>>8),(rnet_u8)packet.size,0,0};
        if(!pipe_write(p,header,sizeof(header)) || !pipe_write(p,packet.bytes,packet.size))break;
    }
    return 0;
}
static void pipe_join(HANDLE thread) {
    if(!thread)return;
    /* Cancellation may race the start of the next synchronous IO call. Keep
     * requesting cancellation until this owned worker exits; no kill cutoff. */
    while(WaitForSingleObject(thread,0)==WAIT_TIMEOUT) {
        CancelSynchronousIo(thread);WaitForSingleObject(thread,10);
    }
    CloseHandle(thread);
}
static void pipe_destroy(RNetPacketPipe *p,int close_handles) {
    pipe_fail(p,RNET_PIPE_CLOSED);
    pipe_join(p->reader);pipe_join(p->writer);
    if(close_handles) {CloseHandle(p->input);CloseHandle(p->output);}
    CloseHandle(p->wake);DeleteCriticalSection(&p->lock);free(p);
}
int rnet_packet_pipe_available(void) {return 1;}
RNetPacketPipe *rnet_packet_pipe_open(uintptr_t input,uintptr_t output,rnet_u8 path) {
    HANDLE in=(HANDLE)input,out=(HANDLE)output;
    if(!in || !out || in==out || in==INVALID_HANDLE_VALUE || out==INVALID_HANDLE_VALUE || path>3 ||
       GetFileType(in)!=FILE_TYPE_PIPE || GetFileType(out)!=FILE_TYPE_PIPE)return NULL;
    RNetPacketPipe *p=calloc(1,sizeof(*p));if(!p)return NULL;
    p->input=in;p->output=out;p->path=path;InitializeCriticalSection(&p->lock);
    p->wake=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(!p->wake) {DeleteCriticalSection(&p->lock);free(p);return NULL;}
    p->reader=(HANDLE)_beginthreadex(NULL,0,pipe_reader,p,0,NULL);
    if(p->reader)p->writer=(HANDLE)_beginthreadex(NULL,0,pipe_writer,p,0,NULL);
    if(!p->reader || !p->writer) {pipe_destroy(p,0);return NULL;}
    if(!SetHandleInformation(in,HANDLE_FLAG_INHERIT,0) || !SetHandleInformation(out,HANDLE_FLAG_INHERIT,0)) {
        pipe_destroy(p,0);return NULL;
    }
    return p;
}
void rnet_packet_pipe_close(RNetPacketPipe *p) {if(p)pipe_destroy(p,1);}
int rnet_packet_pipe_send(RNetPacketPipe *p,const rnet_u8 *bytes,size_t size) {
    if(!p || rnet_packet_pipe_error(p))return -1;
    if(!bytes || !size || size>RNET_PACKET_PIPE_MAX) {pipe_fail(p,RNET_PIPE_FRAME);return -1;}
    EnterCriticalSection(&p->lock);
    if(p->outgoing.count==RNET_PACKET_PIPE_CAPACITY)pipe_fail(p,RNET_PIPE_OVERFLOW);
    else {
        PipePacket *packet=&p->outgoing.rows[(p->outgoing.first+p->outgoing.count)%RNET_PACKET_PIPE_CAPACITY];
        packet->size=(rnet_u16)size;memcpy(packet->bytes,bytes,size);++p->outgoing.count;
    }
    LeaveCriticalSection(&p->lock);SetEvent(p->wake);
    return rnet_packet_pipe_error(p)?-1:(int)size;
}
int rnet_packet_pipe_recv(RNetPacketPipe *p,rnet_u8 *bytes,size_t capacity) {
    if(!p || rnet_packet_pipe_error(p))return -1;
    int result=0;
    EnterCriticalSection(&p->lock);
    if(p->incoming.count) {
        const PipePacket *packet=&p->incoming.rows[p->incoming.first];
        if(!bytes || capacity<packet->size)pipe_fail(p,RNET_PIPE_FRAME);
        else {
            memcpy(bytes,packet->bytes,packet->size);result=packet->size;
            p->incoming.first=(p->incoming.first+1)%RNET_PACKET_PIPE_CAPACITY;--p->incoming.count;
        }
    }
    LeaveCriticalSection(&p->lock);
    return rnet_packet_pipe_error(p)?-1:result;
}
#else
int rnet_packet_pipe_available(void) {return 0;}
RNetPacketPipe *rnet_packet_pipe_open(uintptr_t in,uintptr_t out,rnet_u8 path) {
    (void)in;(void)out;(void)path;return NULL;
}
void rnet_packet_pipe_close(RNetPacketPipe *p) {(void)p;}
int rnet_packet_pipe_error(const RNetPacketPipe *p) {(void)p;return RNET_PIPE_CLOSED;}
int rnet_packet_pipe_send(RNetPacketPipe *p,const rnet_u8 *b,size_t n) {(void)p;(void)b;(void)n;return -1;}
int rnet_packet_pipe_recv(RNetPacketPipe *p,rnet_u8 *b,size_t n) {(void)p;(void)b;(void)n;return -1;}
#endif
static int send_packet(void *p,const rnet_u8 *b,size_t n) {return rnet_packet_pipe_send(p,b,n);}
static int recv_packet(void *p,rnet_u8 *b,size_t n) {return rnet_packet_pipe_recv(p,b,n);}
static int pump_pipe(void *p) {return rnet_packet_pipe_error(p)?-1:0;}
RNetPacketTransport rnet_packet_pipe_transport(RNetPacketPipe *p) {
    RNetPacketTransport transport={p,send_packet,recv_packet,pump_pipe,0};
#ifdef _WIN32
    if(p)transport.path=p->path;
#endif
    return transport;
}
