/* SPDX-License-Identifier: MIT */
#include "recomp_net/packet_pipe.h"
#include "protocol/rnet_protocol.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
static int receive(RNetPacketPipe *pipe,rnet_u8 *bytes,size_t size,HANDLE child) {
    int n;
    while((n=rnet_packet_pipe_recv(pipe,bytes,size))==0) {
        if(child)assert(WaitForSingleObject(child,0)==WAIT_TIMEOUT);
        Sleep(1);
    }
    assert(n>0);return n;
}
static void make_pipes(HANDLE *read,HANDLE *write,HANDLE *other_read,HANDLE *other_write) {
    SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE};
    assert(CreatePipe(read,other_write,&security,4096));
    assert(CreatePipe(other_read,write,&security,4096));
    assert(SetHandleInformation(*read,HANDLE_FLAG_INHERIT,0));
    assert(SetHandleInformation(*write,HANDLE_FLAG_INHERIT,0));
}
static RNetPacketPipe *new_pipe(HANDLE *other_read,HANDLE *other_write) {
    HANDLE read,write;make_pipes(&read,&write,other_read,other_write);
    RNetPacketPipe *pipe=rnet_packet_pipe_open((uintptr_t)read,(uintptr_t)write,3);assert(pipe);
    assert(rnet_packet_pipe_transport(pipe).path==3);return pipe;
}
static void framing(void) {
    HANDLE raw_read,raw_write;RNetPacketPipe *pipe=new_pipe(&raw_read,&raw_write);
    rnet_u8 frame[40]={'R','N','P','1',0,32,0,0},bytes[1200];
    for(unsigned i=8;i<sizeof(frame);++i)frame[i]=(rnet_u8)(i^123);
    for(unsigned i=0;i<sizeof(frame);++i) {DWORD written;assert(WriteFile(raw_write,frame+i,1,&written,NULL)&&written==1);}
    assert(receive(pipe,bytes,sizeof(bytes),NULL)==32 && !memcmp(bytes,frame+8,32));
    /* EOF in a partial header must fail instead of delivering partial data. */
    DWORD written;assert(WriteFile(raw_write,"RNP",3,&written,NULL));CloseHandle(raw_write);
    while(!rnet_packet_pipe_error(pipe))Sleep(1);
    assert(rnet_packet_pipe_recv(pipe,bytes,sizeof(bytes))==-1);
    rnet_packet_pipe_close(pipe);CloseHandle(raw_read);
    for(unsigned kind=0;kind<3;++kind) {
        pipe=new_pipe(&raw_read,&raw_write);
        rnet_u8 bad[8]={'R','N','P','1',0,1,0,0};
        if(kind==0)bad[0]='X';
        if(kind==1)bad[4]=0xff;
        if(kind==2)bad[7]=1;
        assert(WriteFile(raw_write,bad,sizeof(bad),&written,NULL));
        while(!rnet_packet_pipe_error(pipe))Sleep(1);
        assert(rnet_packet_pipe_error(pipe)==RNET_PIPE_FRAME);
        rnet_packet_pipe_close(pipe);CloseHandle(raw_read);CloseHandle(raw_write);
    }
    /* An unconsumed stream cannot grow an unbounded sender queue. */
    pipe=new_pipe(&raw_read,&raw_write);memset(bytes,0x5a,sizeof(bytes));
    for(unsigned i=0;i<RNET_PACKET_PIPE_CAPACITY+32 && !rnet_packet_pipe_error(pipe);++i)
        rnet_packet_pipe_send(pipe,bytes,sizeof(bytes));
    assert(rnet_packet_pipe_error(pipe)==RNET_PIPE_OVERFLOW);
    rnet_packet_pipe_close(pipe);CloseHandle(raw_read);CloseHandle(raw_write);
    /* Likewise a slow simulation is a visible failure, never silent loss. */
    pipe=new_pipe(&raw_read,&raw_write);
    rnet_u8 one[9]={'R','N','P','1',0,1,0,0,0x42};
    for(unsigned i=0;i<RNET_PACKET_PIPE_CAPACITY+1;++i)
        assert(WriteFile(raw_write,one,sizeof(one),&written,NULL)&&written==sizeof(one));
    while(!rnet_packet_pipe_error(pipe))Sleep(1);
    assert(rnet_packet_pipe_error(pipe)==RNET_PIPE_OVERFLOW);
    rnet_packet_pipe_close(pipe);CloseHandle(raw_read);CloseHandle(raw_write);
}
static void process_roundtrip(void) {
    HANDLE read,write,child_read,child_write;make_pipes(&read,&write,&child_read,&child_write);
    wchar_t exe[32768],command[33024];assert(GetModuleFileNameW(NULL,exe,32768));
    swprintf(command,33024,L"\"%ls\" child %llu %llu",exe,
        (unsigned long long)(uintptr_t)child_read,(unsigned long long)(uintptr_t)child_write);
    STARTUPINFOEXW startup;PROCESS_INFORMATION process;SIZE_T attribute_size=0;
    memset(&startup,0,sizeof(startup));memset(&process,0,sizeof(process));startup.StartupInfo.cb=sizeof(startup);
    InitializeProcThreadAttributeList(NULL,1,0,&attribute_size);
    startup.lpAttributeList=malloc(attribute_size);assert(startup.lpAttributeList);
    assert(InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&attribute_size));
    HANDLE inherited[2]={child_read,child_write};
    assert(UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    inherited,sizeof(inherited),NULL,NULL));
    assert(CreateProcessW(exe,command,NULL,NULL,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,
                          NULL,NULL,&startup.StartupInfo,&process));
    DeleteProcThreadAttributeList(startup.lpAttributeList);free(startup.lpAttributeList);
    CloseHandle(child_read);CloseHandle(child_write);CloseHandle(process.hThread);
    RNetPacketPipe *pipe=rnet_packet_pipe_open((uintptr_t)read,(uintptr_t)write,1);assert(pipe);
    RNetPacketTransport transport=rnet_packet_pipe_transport(pipe);
    const unsigned sizes[3]={32,1,1200};rnet_u8 bytes[1200],echo[1200];
    for(unsigned i=0;i<3;++i) {
        for(unsigned j=0;j<sizes[i];++j)bytes[j]=(rnet_u8)(i*53+j);
        assert(transport.send(transport.ctx,bytes,sizes[i])==(int)sizes[i]);
        assert(receive(pipe,echo,sizeof(echo),process.hProcess)==(int)sizes[i]);
        assert(!memcmp(bytes,echo,sizes[i]));
    }
    /* Explicit closing message keeps the EOF after the final delivered row. */
    assert(transport.send(transport.ctx,(const rnet_u8*)"done",4)==4);
    assert(WaitForSingleObject(process.hProcess,INFINITE)==WAIT_OBJECT_0);
    DWORD code=1;assert(GetExitCodeProcess(process.hProcess,&code)&&code==0);
    rnet_packet_pipe_close(pipe);CloseHandle(process.hProcess);
}
#endif
int main(int argc,char **argv) {
#ifdef _WIN32
    if(argc==4 && !strcmp(argv[1],"child")) {
        RNetPacketPipe *pipe=rnet_packet_pipe_open((uintptr_t)strtoull(argv[2],NULL,10),
                                                   (uintptr_t)strtoull(argv[3],NULL,10),1);assert(pipe);
        rnet_u8 bytes[1200];
        for(unsigned i=0;i<3;++i) {int n=receive(pipe,bytes,sizeof(bytes),NULL);assert(rnet_packet_pipe_send(pipe,bytes,n)==n);}
        assert(receive(pipe,bytes,sizeof(bytes),NULL)==4 && !memcmp(bytes,"done",4));
        rnet_packet_pipe_close(pipe);return 0;
    }
    assert(rnet_packet_pipe_available());framing();process_roundtrip();
#else
    (void)argc;(void)argv;assert(!rnet_packet_pipe_available());
#endif
    rnet_u8 packet[1200];const rnet_u32 magic=0x12345678,session=54321;
    int n=rnet_proto_encode_hello(packet,sizeof(packet),magic,session,2,3,2);assert(n>0);
    assert(rnet_packet_source_valid(packet,n,magic,session,2,3));
    assert(!rnet_packet_source_valid(packet,n,magic,session,1,3));
    assert(!rnet_packet_source_valid(packet,n,magic,session+1,2,3));
    assert(!rnet_packet_source_valid(packet,n-1,magic,session,2,3));
    packet[n-1]^=1;assert(!rnet_packet_source_valid(packet,n,magic,session,2,3));
    n=rnet_proto_encode_start(packet,sizeof(packet),magic,session,0);assert(n>0);
    assert(rnet_packet_source_valid(packet,n,magic,session,0,3));
    assert(!rnet_packet_source_valid(packet,n,magic,session,2,3));
    puts("private packet pipe: framing, bounds, subprocess and authenticated source contracts passed");return 0;
}
