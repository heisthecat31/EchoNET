# NetLatency

Client-only plugin for Echo VR (PC build `631547.rad15_live`) that sends your updates
to the server as soon as the game makes them, instead of a frame later.

Echo has no network thread: once per frame, at the very start, it reads the socket and
sends whatever was queued. Your player state is queued ~1.3 ms into the frame, so stock it
waits ~10 ms (the rest of a 90 Hz frame) before it is sent. NetLatency flushes the send
queue at the end of each frame phase.

Measured against a server on the same PC (game API `/session` ping, 45 samples each):

| | median | mean |
|---|---|---|
| stock | 11-12 ms | 10.7-11.7 ms |
| NetLatency | 8-9 ms | 8.4-9.0 ms |

Your movement reaches the server ~10 ms sooner on every frame that produces new state
(the game does so every other frame). The ~8 ms left on localhost is frame timing on the
server and the client's receive side, which a client-only plugin can't usefully change.
The game's built-in network simulator (fake delay / jitter / loss) was checked and is off.

## Build / install

```
build.bat
```

Copy `out/NetLatency.dll` and `NetLatency.ini` to `<install>/bin/win10/plugins/` (needs the
EchoCombat plugin loader, `dbgcore.dll`). `NetLatency.ini` is re-read every 5 s:
`flush=0/1` turns the fix off / on live, `zero_sim=1` clears the network simulator if it
is ever on. `NetLatency.log` reports frame time and where in the frame packets were sent.
