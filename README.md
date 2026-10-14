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

## Every-frame state (`send_every_frame=1`)

The client sends its player state (`R15NetPlayerStateMsg`) and the physics it owns
(`R15NetPhysicsSimStateEvent`) on a timer: the interval at component system +0x150 / +0x158
is 22222 µs, so 45 Hz (every other frame at 90 fps). `send_every_frame=1` sets it to 1, so
both go out every frame: the server's copy of you is on average ~5.5 ms fresher. Costs about
twice the upload for those two messages. The ping number doesn't change (ping uses its own
messages). Tested on our own servers: no warnings in their logs.

## Wi-Fi priority (`qos=1`)

The game socket's traffic to each server is added to a qWAVE flow as voice traffic, so
Windows tags it (DSCP / 802.1p) and Wi-Fi (WMM) / QoS-aware routers send it first. No admin
rights needed; it only helps your upload, and only on networks that honour it. Windows
refuses flows to LAN addresses (error 1168), which is harmless.

## Build / install

```
build.bat
```

Copy `out/EchoNET.dll` and `EchoNET.ini` to `<install>/bin/win10/plugins/` (needs the
EchoCombat plugin loader, `dbgcore.dll`). `EchoNET.ini` is re-read every 5 s:
`flush`, `send_every_frame` and `qos` (0/1) switch each fix live, `trace=1` logs which code queues which message on which frames, `zero_sim=1` clears the network simulator if it
is ever on. `EchoNET.log` reports frame time and where in the frame packets were sent.
