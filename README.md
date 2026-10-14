# EchoNET

Client-only plugin for Echo VR (PC build `631547.rad15_live`) that sends your updates
to the server as soon as the game makes them, instead of a frame later.

Echo has no network thread: once per frame, at the very start, it reads the socket and
sends whatever was queued. Your player state is queued ~1.3 ms into the frame, so stock it
waits ~10 ms (the rest of a 90 Hz frame) before it is sent. EchoNET flushes the send
queue at the end of each frame phase.

Measured against a server on the same PC (game API `/session` ping, 45 samples each):

| | median | mean |
|---|---|---|
| stock | 11-12 ms | 10.7-11.7 ms |
| EchoNET | 8-9 ms | 8.4-9.0 ms |

Your movement reaches the server ~10 ms sooner on every frame that produces new state
(the game does so every other frame). The ~8 ms left on localhost is frame timing on the
server and the client's receive side, which a client-only plugin can't usefully change.
The game's built-in network simulator (fake delay / jitter / loss) was checked and is off.

## Your ping number goes down too

The ping the game shows is the server's estimate of your round trip, and part of it is your
client's reply to the server's ping, which stock Echo also holds until the start of the next
frame (up to ~11 ms at 90 Hz). `flush=1` sends that reply in the frame it was made, so the
number drops, as in the table above. Over the internet `qos=1` can add to that: on Wi-Fi or a
busy home network, tagging the game's packets as voice traffic gets them past other traffic
queued in the router, which can be tens of milliseconds. Reported on a remote (Pennsylvania)
server: about 20 ms lower with EchoNET. How much you gain depends on your frame rate and your
network.

## Every-frame state (`send_every_frame=1`)

The client sends its player state (`R15NetPlayerStateMsg`) and the physics it owns
(`R15NetPhysicsSimStateEvent`) on a timer: the interval at component system +0x150 / +0x158
is 22222 µs, so 45 Hz (every other frame at 90 fps). `send_every_frame=1` sets it to 1, so
both go out every frame: the server's copy of you is on average ~5.5 ms fresher. Costs about
twice the upload for those two messages. This setting on its own doesn't move the ping
number, because ping is measured with its own messages, not these; `flush` and `qos` are what
lower it (see above). Tested on our own servers: no warnings in their logs.

## Wi-Fi priority (`qos=1`)

The game socket's traffic to each server is added to a qWAVE flow as voice traffic, so
Windows tags it (DSCP / 802.1p) and Wi-Fi (WMM) / QoS-aware routers send it first. No admin
rights needed; it only helps your upload, and only on networks that honour it. Windows
refuses flows to LAN addresses (error 1168), which is harmless.

## Experimental: reading the socket sooner (`drain=1`, `idle_pump=1`)

Stock Echo reads the socket once per frame, at its start, so a packet from the server can
sit there for most of a frame before the game sees it. The server's ping message is answered
inside that read (by `CR15NetGame::OnR15NetServerFrameMsg`), so a sooner read means a sooner
answer.

- `drain=1` runs the whole broadcaster update (read, handle, send) after every frame phase
  instead of only the send half. Packets are then handled between phases as well as at frame
  start.
- `idle_pump=1` keeps reading while the main thread waits between frames (for the headset or
  the frame pacer): those waits are cut into 1 ms slices with the socket checked between them.
  A wait still ends the moment it would have, so frame timing doesn't change.

Both are off by default, and **leave them off**: in a real match they break how other players
are drawn. Their updates are then applied part-way through your frame, after the frame has
already placed their cosmetics and nametag, so bodies, arms, discs, cosmetics and nametags end
up drawn from different updates and drift apart (players look like they're coming apart).
Against a server on the same PC they changed the ping by only 1-2 ms anyway. A version that
reads early but holds everything except the server's ping message until frame start would
avoid this; it hasn't been written.

## The F9 "Avg Ping"

On a client it is always 0.00: it is filled from the client's ping reply, which only the
server receives. The ping the game shows you (scoreboard, `/session`) is the server's
estimate, sent back to you.

## Build / install

```
build.bat
```

Copy `out/EchoNET.dll` and `EchoNET.ini` to `<install>/bin/win10/plugins/` (needs the
EchoCombat plugin loader, `dbgcore.dll`). `EchoNET.ini` is re-read every 5 s:
`flush`, `send_every_frame` and `qos` (0/1) switch each fix live, `trace=1` logs which code queues which message on which frames, `zero_sim=1` clears the network simulator if it
is ever on. `EchoNET.log` reports frame time and where in the frame packets were sent.
