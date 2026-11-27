#include "recomp_net/telemetry.h"
#include <stdio.h>
#include <string.h>
int main(void){
    RNetTelemetry *t=rnet_telemetry_create(RNET_SYNC_LOCAL_V4,16667);
    RNetTickTelemetry row={0},first[2];char report[4096]={0},short_buffer[2]={'x','y'};unsigned i;size_t need;
    if(!t)return 1;
    row.delay=2;row.input_latency_valid=1;row.input_latency_us=33334;
    for(i=0;i<RNET_TELEMETRY_TICKS+5;i++){row.tick=i;rnet_telemetry_record(t,&row);}
    if(rnet_telemetry_read(t,0,first,2)!=2 || first[0].sequence!=5 || first[1].sequence!=6)return 2;
    need=rnet_telemetry_report(t,report,sizeof(report));
    if(!need || need>sizeof(report))return 3;
    if(!strstr(report,"\"dropped_ticks\":5") || !strstr(report,"33.334"))return 4;
    if(rnet_telemetry_report(t,short_buffer,2)<3 || memcmp(short_buffer,"xy",2))return 5;
    rnet_telemetry_destroy(t);puts("telemetry wrap, exact latency and atomic short-buffer report passed");return 0;
}
