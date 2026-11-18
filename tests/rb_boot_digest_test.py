"""Real per-process rollback drivers behind deterministic UDP packet loss.

Drop only selected FRAME_COMMITs, preserving INPUT/HELLO/identity. This works
on Windows as well as POSIX; no fork, game ROM or test hook in the library.
"""
import argparse
import json
import os
from pathlib import Path
import selectors
import socket
import struct
import subprocess
import tempfile
import time


def udp():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if hasattr(socket, 'SIO_UDP_CONNRESET'):
        # A peer may not have bound yet, or may already have exited. Windows
        # otherwise converts its ICMP port-unreachable into recvfrom failure.
        sock.ioctl(socket.SIO_UDP_CONNRESET, False)
    sock.bind(('127.0.0.1', 0))
    return sock


def run(peer, name, slots=2, drops=0, mismatch=False, late=False, permanent=False):
    with tempfile.TemporaryDirectory(prefix='rnet-boot-') as temporary:
        folder = Path(temporary)
        selector = selectors.DefaultSelector()
        reserves = [udp() for _ in range(slots)]
        addresses = [s.getsockname() for s in reserves]
        links = []
        processes, files = [], []
        events, requests, dropped = [], 0, 0
        highest_commit = [-1] * slots
        late_request, late_replies = False, []
        try:
            # Each hub/guest link has an endpoint on each side. Forwarding
            # through the opposite socket preserves the configured source.
            for remote in range(1, slots):
                host_side, guest_side = udp(), udp()
                links += [host_side, guest_side]
                selector.register(host_side, selectors.EVENT_READ, (guest_side, addresses[remote], remote))
                selector.register(guest_side, selectors.EVENT_READ, (host_side, addresses[0], 0))
            env = {k: v for k, v in os.environ.items() if not k.startswith(('RNET_', 'RBE_RB_'))}
            for s in reserves:
                s.close()
            for slot in range(slots):
                endpoint = links[0 if slot == 0 else (slot-1)*2+1].getsockname()
                bind = '%s:%d' % addresses[slot]
                remote = '%s:%d' % endpoint
                refusal = 'boot_digest_timeout' if permanent else 'boot_digest_mismatch' if mismatch else '-'
                log = (folder / ('peer%d.log' % slot)).open('wb')
                files.append(log)
                processes.append(subprocess.Popen([str(peer), str(slot), str(slots), bind, remote,
                                                    str(addresses[0][1]), refusal, str(int(mismatch)),
                                                    str(int(late)), str(folder)], env=env, stdout=subprocess.PIPE, stderr=log))
            deadline = time.monotonic() + 20
            stop_written = False
            while any(p.poll() is None for p in processes) and time.monotonic() < deadline:
                if not stop_written and all((folder / ('%d.ready' % n)).exists() for n in range(slots)):
                    (folder / 'stop').touch()
                    stop_written = True
                for key, _ in selector.select(.002):
                    try:
                        packet, _ = key.fileobj.recvfrom(2048)
                    except ConnectionResetError:
                        # Some Windows Python builds omit SIO_UDP_CONNRESET.
                        continue
                    forward, destination, receiver = key.data
                    if len(packet) >= 24:
                        kind, = struct.unpack_from('<H', packet, 4)
                        sender = packet[10]
                        if kind == 20 and packet[12] == 6:  # FRAME_COMMIT_REQUEST
                            requests += 1
                            if (late and sender == 1 and receiver == 0 and
                                    struct.unpack_from('<I', packet, 18)[0] == 0 and
                                    highest_commit[1] >= 179):
                                assert highest_commit[0] >= 128, (name, highest_commit)
                                late_request = True
                        if kind == 24:
                            tick, digest = struct.unpack_from('<II', packet, 12)
                            highest_commit[sender] = max(highest_commit[sender], tick)
                            if tick == 0:
                                events.append((sender, receiver, digest))
                                if late_request and sender == 0 and receiver == 1:
                                    late_replies.append(digest)
                                # Positive one-way loss: receiver 1 waits while
                                # sender 0 has already accepted peer 1's hash.
                                eligible = permanent or (sender == 0 and receiver == slots-1)
                                if eligible and (permanent or dropped < drops):
                                    dropped += 1
                                    continue
                    forward.sendto(packet, destination)
            assert all(p.poll() is not None for p in processes), name + ': child timeout'
            reports = []
            for slot, p in enumerate(processes):
                output = p.communicate(timeout=1)[0].decode()
                assert p.returncode == 0, (name, slot, p.returncode, output)
                reports.append(json.loads(output))
            for f in files:
                f.flush()
            assert all(not r['unverified'] and not r['failed'] for r in reports), (name, reports)
            if permanent or mismatch:
                reason = 'boot_digest_timeout' if permanent else 'boot_digest_mismatch'
                assert all(r['reason'] == reason and r['refused'] == 1 and r['sim_at_refusal'] == 1 and
                           r['live_after_refusal'] == 0 for r in reports), (name, reports)
            else:
                assert all(r['agreed'] == 1 and not r['refused'] and r['sim'] >= 220 and
                           r['confirmed'] >= 215 and not r['desync'] for r in reports), (name, reports)
            if drops:
                assert dropped == drops and requests > 0, (name, dropped, requests)
            if late:
                original = [digest for sender, receiver, digest in events if sender == 0 and receiver == 1]
                assert (reports[1]['late_sent'] and late_request and late_replies and
                        all(digest == original[0] for digest in late_replies)), (name, original, late_replies)
            print(json.dumps(dict(case=name, dropped=dropped, requests=requests, peers=reports)), flush=True)
        except BaseException:
            for slot, f in enumerate(files):
                f.flush()
                print((folder / ('peer%d.log' % slot)).read_text(errors='replace')[-6000:], flush=True)
            raise
        finally:
            for p in processes:
                if p.poll() is None:
                    p.kill()
                p.wait()
                if p.stdout:
                    p.stdout.close()
            for f in files:
                f.close()
            selector.close()
            for s in links + reserves:
                s.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--peer', type=Path, required=True)
    parser.add_argument('--case', default='')
    args = parser.parse_args()
    cases = [('clean', {}), ('lost-first', dict(drops=1)), ('lost-replies', dict(drops=3)),
             ('mismatch-after-loss', dict(drops=1, mismatch=True)), ('permanent-loss', dict(permanent=True)),
             ('three-seat-loss', dict(slots=3, drops=2)), ('four-seat-loss', dict(slots=4, drops=2)),
             ('retained-boot-digest', dict(late=True))]
    for name, options in cases:
        if not args.case or name == args.case:
            run(args.peer.resolve(), name, **options)


if __name__ == '__main__':
    main()
