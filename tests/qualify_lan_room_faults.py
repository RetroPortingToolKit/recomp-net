"""Exercise the real room socket contract through a lossy UDP router.

Each guest retains a distinct host-side source endpoint, preserving the actual
ownership checks. Impair control packets, including a forced first START_ACK
drop for every guest. Best-effort chat is forwarded intact. This certifies the
room protocol, not emulation or Internet NAT diversity.
"""
import argparse
import heapq
import json
import os
from pathlib import Path
import random
import selectors
import socket
import struct
import subprocess
import time
import traceback
import hashlib

def case(exe,out,guest_first,seed):
    out.mkdir();rng=random.Random(seed);selector=selectors.DefaultSelector();links={};pending=[];serial=0
    front=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);front.bind(('127.0.0.1',0));front.setblocking(False)
    selector.register(front,selectors.EVENT_READ,None);host=('127.0.0.1',19971)
    stats={'received':0,'dropped':0,'duplicated':0,'reordered':0,'forced_start_ack_drops':0,'forced_kick_drops':0,'forged_wrong_source_requests':0,'forwarded':0}
    forced=set();last={};counter={};
    env=dict(os.environ,RNET_LAN_ROOM_TEST_ENDPOINT=f'127.0.0.1:{front.getsockname()[1]}')
    if guest_first:env['RNET_LAN_ROOM_TEST_GUEST_FIRST']='1'
    else:env.pop('RNET_LAN_ROOM_TEST_GUEST_FIRST',None)
    stdout=(out/'stdout.log').open('wb');stderr=(out/'stderr.log').open('wb')
    process=subprocess.Popen([str(exe.resolve())],cwd=out,stdout=stdout,stderr=stderr,env=env,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
    def schedule(channel,dest,data,direction,key):
        nonlocal serial
        stats['received']+=1;op=struct.unpack_from('<I',data,8)[0] if len(data)>=36 else 0
        if direction=='up' and op==4 and len(data)==48 and not stats['forged_wrong_source_requests'] and len(links)>=2:
            # Correct identity/epoch/revision/checksum, wrong source endpoint.
            # If accepted it poisons that member's sequence and readiness; the
            # rest of the contract cannot pass on the legitimate later rows.
            forged=bytearray(data);struct.pack_into('<I',forged,20,0x7ffffffe);struct.pack_into('<I',forged,40,0)
            struct.pack_into('<I',forged,32,0);checksum=0x811c9dc5
            for byte in forged:checksum=((checksum^byte)*0x01000193)&0xffffffff
            struct.pack_into('<I',forged,32,checksum)
            next(back for source,back in links.items() if source!=key).sendto(forged,host)
            stats['forged_wrong_source_requests']+=1
        impaired=op not in (12,13)
        if direction=='up' and op==7 and key not in forced:
            forced.add(key);stats['dropped']+=1;stats['forced_start_ack_drops']+=1;return
        if direction=='down' and op==9 and struct.unpack_from('<i',data,28)[0]==-10 and ('kick',key) not in forced:
            forced.add(('kick',key));stats['dropped']+=1;stats['forced_kick_drops']+=1;return
        if impaired and rng.random()<.18:stats['dropped']+=1;return
        copies=2 if impaired and rng.random()<.18 else 1
        stats['duplicated']+=copies-1
        order_key=(key,direction);counter[order_key]=counter.get(order_key,0)+1;seq=counter[order_key]
        for _ in range(copies):
            serial+=1;delay=max(0,.025+rng.uniform(-.025,.045)) if impaired else 0
            heapq.heappush(pending,(time.monotonic()+delay,serial,channel,dest,data,order_key,seq))
    try:
        deadline=time.monotonic()+45
        while process.poll() is None:
            if time.monotonic()>deadline:raise TimeoutError('Room fault contract timed out')
            now=time.monotonic()
            while pending and pending[0][0]<=now:
                _,_,channel,dest,data,key,seq=heapq.heappop(pending)
                channel.sendto(data,dest);stats['forwarded']+=1
                if seq<last.get(key,0):stats['reordered']+=1
                last[key]=max(seq,last.get(key,0))
            for event,_ in selector.select(.001):
                channel=event.fileobj
                try:data,source=channel.recvfrom(4096)
                except (BlockingIOError,ConnectionResetError):continue # Windows reports ICMP after a closed guest
                if channel is front:
                    if source not in links:
                        back=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);back.bind(('127.0.0.1',0));back.setblocking(False)
                        links[source]=back;selector.register(back,selectors.EVENT_READ,source)
                    schedule(links[source],host,data,'up',source)
                else:
                    assert source==host
                    schedule(front,event.data,data,'down',event.data)
        assert process.returncode==0,('Room contract failed',process.returncode,(out/'stderr.log').read_text())
        assert stats['dropped'] and stats['duplicated'] and stats['reordered'] and stats['forced_start_ack_drops']>=3 and stats['forced_kick_drops']>=1 and stats['forged_wrong_source_requests'],stats
        return {'passed':True,'guest_first':guest_first,'seed':seed,'stats':stats}
    finally:
        if process.poll() is None:process.terminate();process.wait(timeout=5)
        stdout.close();stderr.close();selector.close();front.close()
        for channel in links.values():channel.close()

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--exe',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=False)
    report={'passed':False,'scope':__doc__,'binary_sha256':hashlib.sha256(a.exe.read_bytes()).hexdigest(),'cases':[]}
    try:
        for mode,seed in ((False,91321),(True,31719)):report['cases'].append(case(a.exe,a.out/('guest-first' if mode else 'host-first'),mode,seed))
        report['passed']=True
    except Exception as error:report['error']=repr(error);report['traceback']=traceback.format_exc()
    (a.out/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
    return 0 if report['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
