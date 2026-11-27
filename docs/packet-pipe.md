# Private child-process packet transport

`recomp_net/packet_pipe.h` adapts explicitly inherited Windows anonymous pipe
handles to `RNetPacketTransport`. The launcher retains completed ICE agents;
the child retains its simulation session. No ICE pointer, account secret,
new network listener or server change is involved. This component does not
implement a launcher's process creation or its ICE routing policy.

Create two pipes and inherit only the child's read and write endpoints using
an explicit process handle list. Close the child's endpoints in the parent
after process creation. Pass their numeric values through the trusted launch
arguments. Each process opens its own adapter, which takes ownership on
success and clears local inheritance. An unsuccessful open leaves handles
owned by the caller. Destroy the session before closing its adapter.

The versioned stream frame is eight bytes followed by one datagram:
`RNP1`, a big-endian 16-bit payload length, and two zero reserved bytes.
Payloads contain 1–1200 bytes. Partial reads and writes are assembled by IO
workers; the session callbacks only copy datagrams through two queues, each
bounded at 256 packets. Bad framing, capacity overflow, EOF, short receive
capacity and IO errors latch a terminal error. They never silently truncate,
drop or fall back to another transport. Closing cancels and joins only the
adapter's workers, including when a peer stops reading.

A hub must bind each ICE agent to its authenticated player and negotiated
session slot. Before forwarding an incoming datagram, use
`rnet_packet_source_valid` with that slot, the actual protocol magic, session
ID and player count. It runs the normal decoder/checksum and checks the
claimed sender; host-only START and DELAY_SYNC cannot originate at a guest.
The child session continues to perform its normal build, profile, content,
input and simulation checks. Never derive the agent's identity from the
packet's claimed sender.

Use the actual selected direct/hub/TURN path in `open`, retaining per-agent
metadata in the launcher. Packet access to an agent has one owner: no other
session or lobby helper may concurrently consume that same agent.

The adapter currently supports Windows. Other platforms report unavailable
and preserve the existing portable external-transport callback interface;
they must not advertise this Windows process handoff as available.

`packet_pipe_test` checks actual private-handle subprocess round trips,
maximum datagrams, byte-at-a-time framing, malformed headers, truncated EOF,
both queue limits, cancellation of blocked writers and authenticated source
validation. `external_transport_test` independently checks protocol sessions
over the host-supplied transport. These component results do not qualify a
game launcher, deployed ICE/TURN service or online game match.
