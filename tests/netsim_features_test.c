#include "transport/rnet_netsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static rnet_u64 clock_ms=1000;
rnet_u64 rnet_os_monotonic_ms(void){return clock_ms;}
static int failures;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);++failures;}}while(0)
static void env(const char *key,const char *value){
#ifdef _WIN32
    _putenv_s(key,value);
#else
    setenv(key,value,1);
#endif
}
static void reset(void){
    env("RNET_SIM_PROFILE","");env("RNET_SIM_LATENCY_MS","");env("RNET_SIM_JITTER_MS","0");
    env("RNET_SIM_LOSS_PCT","0");env("RNET_SIM_DUPLICATE_PCT","0");env("RNET_SIM_BURST_PCT","0");
    env("RNET_SIM_KBPS","0");env("RNET_SIM_SEND_LATENCY_MS","0");clock_ms=1000;
}
int main(void){
    const char *names[]={"same-region","cross-country","transatlantic","transpacific","wifi"};
    unsigned latency[]={10,40,80,125,12},i;unsigned char bytes[1000]={1},out[1000];RNetNetSim *s;
    for(i=0;i<5;i++){
        reset();env("RNET_SIM_PROFILE",names[i]);s=rnet_netsim_create();CHECK(s);if(!s)return 1;
        CHECK(rnet_netsim_hold(s,bytes,1,NULL));clock_ms+=latency[i]-1;
        CHECK(!rnet_netsim_due(s,out,sizeof(out),NULL));clock_ms++;
        CHECK(rnet_netsim_due(s,out,sizeof(out),NULL)==1);rnet_netsim_destroy(s);
    }
    reset();env("RNET_SIM_LATENCY_MS","10");env("RNET_SIM_DUPLICATE_PCT","100");s=rnet_netsim_create();
    CHECK(s);CHECK(rnet_netsim_hold(s,bytes,1,NULL));clock_ms+=10;
    CHECK(rnet_netsim_due(s,out,sizeof(out),NULL)==1);CHECK(!rnet_netsim_due(s,out,sizeof(out),NULL));
    clock_ms++;CHECK(rnet_netsim_due(s,out,sizeof(out),NULL)==1);rnet_netsim_destroy(s);
    reset();env("RNET_SIM_BURST_PCT","100");env("RNET_SIM_BURST_PACKETS","3");s=rnet_netsim_create();CHECK(s);
    for(i=0;i<6;i++) { CHECK(rnet_netsim_hold(s,bytes,1,NULL)); }
    clock_ms+=100;
    CHECK(!rnet_netsim_due(s,out,sizeof(out),NULL));rnet_netsim_destroy(s);
    reset();env("RNET_SIM_KBPS","8");s=rnet_netsim_create();CHECK(s);
    CHECK(rnet_netsim_hold(s,bytes,1000,NULL));CHECK(rnet_netsim_hold(s,bytes,1000,NULL));
    clock_ms+=999;CHECK(!rnet_netsim_due(s,out,sizeof(out),NULL));clock_ms++;
    CHECK(rnet_netsim_due(s,out,sizeof(out),NULL)==1000);CHECK(!rnet_netsim_due(s,out,sizeof(out),NULL));
    clock_ms+=1000;CHECK(rnet_netsim_due(s,out,sizeof(out),NULL)==1000);rnet_netsim_destroy(s);
    reset();env("RNET_SIM_LATENCY_MS","100");env("RNET_SIM_SEND_LATENCY_MS","5");s=rnet_netsim_create_send();CHECK(s);
    CHECK(rnet_netsim_hold(s,bytes,1,NULL));clock_ms+=4;CHECK(!rnet_netsim_due(s,out,sizeof(out),NULL));clock_ms++;
    CHECK(rnet_netsim_due(s,out,sizeof(out),NULL)==1);rnet_netsim_destroy(s);
    return failures?1:0;
}
