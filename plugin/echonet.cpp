// EchoNET: measure and cut the latency Echo VR (PC build 631547.rad15_live) adds on top
// of the network, client side only.
//
// Echo has no network thread. Once per frame CR15NetGame::Update (+0x1bf610), from phase 0
// of CR15Game's frame function (+0x127420, (game, phase)), calls CBroadcaster::Update
// (+0xf8ea10, (broadcaster handle)), which
//
//   1. SendRecvDrain (+0xf8e310): sends packets that were held back, then drains the
//      socket (non-blocking recvfrom, 10 ms budget) and hands each packet on;
//   2. SendFlush (+0xf8a280, (handle, 1)): encodes everything the game queued for each
//      peer and sends it. Rate gated by the broadcaster itself to once per 1 ms.
//
// So everything the game queues while it simulates frame N (hand / head poses, throws,
// grabs) is only sent at the start of frame N+1: up to a whole frame late (11 ms at
// 90 Hz). flush=1 calls SendFlush again at the end of every frame phase, on the same
// thread, so it goes out as soon as it is queued.
//
// The port (handle -> +0x10) also carries a network simulator: outgoing delay / jitter
// at +0x18 / +0x28 (timer ticks), incoming delay / jitter at +0x20 / +0x30, outgoing /
// incoming loss at +0x38 / +0x3c (floats). Non-zero values hold packets in a queue until
// due. The log reports them; zero_sim=1 clears them.
//
// EchoNET.ini next to the DLL, re-read every 5 s (so the fix can be A/B tested live):
//   [net]
//   flush=1
//   zero_sim=1
// EchoNET.log: every 5 s, frame time, where in the frame packets were sent, and how
// many went out early thanks to flush.

#include <winsock2.h>
#include <windows.h>
#include <qos2.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "detours.h"
#pragma comment(lib, "winmm.lib")

namespace {

constexpr uintptr_t BC_UPDATE_RVA = 0xf8ea10;
constexpr uint8_t BC_UPDATE_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf9 };
constexpr uintptr_t FRAME_PHASE_RVA = 0x127420;
constexpr uint8_t FRAME_PHASE_BYTES[] = { 0x40, 0x55, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xb9, 0x48, 0x81, 0xec, 0xf0, 0x00, 0x00 };
constexpr uintptr_t SEND_FLUSH_RVA = 0xf8a280;
constexpr uint8_t SEND_FLUSH_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56 };

// CBroadcaster::Send(CMatSym, ...): every game message is queued through here.
constexpr uintptr_t BC_SEND_RVA = 0xf89af0;
constexpr uint8_t BC_SEND_BYTES[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x4c, 0x89, 0x48, 0x20, 0x44, 0x89, 0x40, 0x18, 0x55 };

// The two senders of the client's state, each (component system): they send when
// (now - last) + carry + 1000 >= the interval at cs+0x150 (under 6 players) or cs+0x158,
// now = netgame (cs+0xB8) +0x2DA8. A ~60 Hz interval at 90 fps comes out as every other
// frame (45 Hz). send_every_frame=1 sets both intervals to 1.
constexpr uintptr_t PHYS_SEND_RVA = 0xd6e6b0;     // -> ProcessGameFrame: R15NetPhysicsSimStateEvent
constexpr uint8_t PHYS_SEND_BYTES[] = { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48 };
constexpr uintptr_t PLAYER_SEND_RVA = 0xd6e980;   // -> ProcessSyncResponse: R15NetPlayerStateMsg
constexpr uint8_t PLAYER_SEND_BYTES[] = { 0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48 };

using bc_update_fn = void (*)(int64_t *handle);
using cs_send_fn = uint64_t (*)(void *cs);
using frame_phase_fn = void (*)(void *game, int64_t phase);
using send_flush_fn = void (*)(int64_t *handle, int now);
using bc_send_fn = void (*)(void *, uint64_t, uint32_t, void *, int64_t, void *, int64_t, void *, int64_t, uint32_t, int64_t);
using sendto_fn = int (WSAAPI *)(SOCKET, const char *, int, int, const sockaddr *, int);

bc_update_fn Real_BcUpdate;
cs_send_fn Real_PhysSend, Real_PlayerSend;
frame_phase_fn Real_FramePhase;
send_flush_fn SendFlush;
bc_send_fn Real_BcSend;
sendto_fn Real_sendto = sendto;

int64_t *volatile g_handle = nullptr;   // the game's broadcaster (the one with a socket)
DWORD g_main_thread = 0;
volatile LONG g_flush = 1, g_zero_sim = 1, g_qos = 1, g_trace = 0, g_every_frame = 0, g_drain = 0;
uint64_t g_drains = 0;   // drain=1: mid-frame socket reads this report window
// idle_pump=1: while the main thread waits between frames (for the headset or the frame pacer)
// it still reads the socket. g_idle is true from the end of the last frame phase until the
// next frame's broadcaster update.
volatile LONG g_idle_pump = 0;
bool g_idle = false, g_pumping = false;
uint64_t g_idle_pumps = 0;
thread_local int t_in_wait = 0;

// send_every_frame: the intervals each state sender had, so 0 puts them back
struct SenderIntervals { void *cs; uint64_t under6, six_up; };
SenderIntervals g_senders[4];
int g_nsenders = 0;
char g_dir[MAX_PATH];
uint8_t *g_base;
uint64_t g_frame = 0;

// trace=1: which code queues which message, and on which frames
struct SendSite { uintptr_t caller; uint64_t sym; uint32_t total, even, odd; uint64_t last_frame; uint32_t frames; };
SendSite g_sites[96];
int g_nsites = 0;
SRWLOCK g_sites_lock = SRWLOCK_INIT;
thread_local int t_send_depth = 0;

// qos=1: the game socket's flows to each server, as voice traffic
HANDLE g_qos_handle = nullptr;
SOCKET g_qos_socket = INVALID_SOCKET;
sockaddr_storage g_qos_dest[8];
int g_qos_ndest = 0;
SRWLOCK g_qos_lock = SRWLOCK_INIT;

LARGE_INTEGER g_freq;
int64_t Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
double Ms(int64_t ticks) { return ticks * 1000.0 / g_freq.QuadPart; }

// stats over one report window
struct Stats {
    int64_t window_start = 0, frame_start = 0, last_update = 0;
    uint64_t frames = 0, frame_ticks = 0;
    uint64_t sends_in_update = 0, sends_early = 0, sends_other = 0;
    uint64_t early_offset_ticks = 0;   // frame-start -> early send, summed
    int phase_sends[4] = {};
} g_s;
bool g_in_update = false, g_in_flush = false;
int g_flush_phase = -1;

void Log(const char *fmt, ...) {
    char path[MAX_PATH];
    sprintf_s(path, "%sEchoNET.log", g_dir);
    FILE *f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

bool Check(const uint8_t *at, const uint8_t *bytes, size_t n, const char *what) {
    if (memcmp(at, bytes, n) == 0) return true;
    Log("%s: unexpected bytes, not hooking (wrong game build?)", what);
    return false;
}

void ReadIni() {
    char path[MAX_PATH];
    sprintf_s(path, "%sEchoNET.ini", g_dir);
    LONG flush = GetPrivateProfileIntA("net", "flush", 1, path);
    LONG zero = GetPrivateProfileIntA("net", "zero_sim", 1, path);
    LONG qos = GetPrivateProfileIntA("net", "qos", 1, path);
    LONG trace = GetPrivateProfileIntA("net", "trace", 0, path);
    LONG every = GetPrivateProfileIntA("net", "send_every_frame", 0, path);
    LONG drain = GetPrivateProfileIntA("net", "drain", 0, path);
    LONG idle_pump = GetPrivateProfileIntA("net", "idle_pump", 0, path);
    if (idle_pump != g_idle_pump) {
        Log("ini: idle_pump=%ld", idle_pump);
        if (idle_pump) timeBeginPeriod(1);   // 1 ms wait slices
        else if (g_idle_pump) timeEndPeriod(1);
    }
    g_idle_pump = idle_pump;
    if (flush != g_flush || zero != g_zero_sim || qos != g_qos || trace != g_trace || every != g_every_frame || drain != g_drain)
        Log("ini: flush=%ld zero_sim=%ld qos=%ld trace=%ld send_every_frame=%ld drain=%ld", flush, zero, qos, trace, every, drain);
    g_drain = drain;
    g_flush = flush;
    g_zero_sim = zero;
    g_qos = qos;
    g_trace = trace;
    g_every_frame = every;
}

uint8_t *Port(int64_t *handle) { return (uint8_t *)*handle + 0x10; }

void CheckSim(uint8_t *port) {
    static int64_t last[4] = { -1, -1, -1, -1 };
    static uint32_t last_loss[2] = { 0xffffffff, 0xffffffff };
    int64_t *ticks = (int64_t *)(port + 0x18);   // +18 out delay, +20 in delay, +28 out jitter, +30 in jitter
    int64_t v[4] = { ticks[0], ticks[1], ticks[2], ticks[3] };
    uint32_t loss[2] = { *(uint32_t *)(port + 0x38), *(uint32_t *)(port + 0x3c) };
    if (memcmp(v, last, sizeof v) || memcmp(loss, last_loss, sizeof loss)) {
        float lo, li;
        memcpy(&lo, &loss[0], 4);
        memcpy(&li, &loss[1], 4);
        Log("network simulator: out delay %lld jitter %lld, in delay %lld jitter %lld (ticks), loss out %.3f in %.3f",
            v[0], v[2], v[1], v[3], lo, li);
        memcpy(last, v, sizeof v);
        memcpy(last_loss, loss, sizeof loss);
    }
    if (g_zero_sim && (v[0] | v[1] | v[2] | v[3] | loss[0] | loss[1])) {
        memset(port + 0x18, 0, 0x20);
        memset(port + 0x38, 0, 8);
        Log("network simulator cleared");
    }
}

void Report(int64_t now) {
    Stats &s = g_s;
    if (s.frames) {
        Log("%llu frames, %.2f ms each | sends: %llu at frame start, %llu early (avg %.2f ms into the frame; by phase %d/%d/%d/%d), %llu other",
            s.frames, Ms(s.frame_ticks) / s.frames, s.sends_in_update, s.sends_early,
            s.sends_early ? Ms(s.early_offset_ticks) / s.sends_early : 0.0,
            s.phase_sends[0], s.phase_sends[1], s.phase_sends[2], s.phase_sends[3], s.sends_other);
        if (g_drain)
            Log("  drain: %llu mid-frame socket reads", (unsigned long long)g_drains);
    }
    g_drains = 0;
    if (g_trace) {
        AcquireSRWLockExclusive(&g_sites_lock);
        for (int pass = 0; pass < 16; ++pass) {   // the 16 busiest, busiest first
            int best = -1;
            for (int i = 0; i < g_nsites; ++i)
                if (g_sites[i].total && (best < 0 || g_sites[i].total > g_sites[best].total)) best = i;
            if (best < 0) break;
            SendSite &x = g_sites[best];
            Log("  send from +0x%llx msg %016llx: %u in %u frames (even %u, odd %u)",
                (unsigned long long)x.caller, (unsigned long long)x.sym, x.total, x.frames, x.even, x.odd);
            x.total = 0;
        }
        g_nsites = 0;
        ReleaseSRWLockExclusive(&g_sites_lock);
    }
    int64_t last = s.last_update, start = s.frame_start;
    s = Stats();
    s.window_start = now;
    s.last_update = last;
    s.frame_start = start;
}

void Hooked_BcUpdate(int64_t *handle) {
    int64_t now = Now();
    if (!g_handle && handle && *handle) {
        g_handle = handle;
        g_main_thread = GetCurrentThreadId();
        Log("broadcaster %p, thread %lu", (void *)handle, g_main_thread);
    }
    if (handle == g_handle) {
        g_frame++;
        if (g_s.last_update) { g_s.frames++; g_s.frame_ticks += now - g_s.last_update; }
        g_s.last_update = now;
        g_s.frame_start = now;
        if (!g_s.window_start) g_s.window_start = now;
        if (Ms(now - g_s.window_start) >= 5000) {
            Report(now);
            ReadIni();
            CheckSim(Port(handle));
        }
        g_in_update = true;
    }
    Real_BcUpdate(handle);
    g_in_update = false;
}

// drain=1: after each frame phase, run the whole broadcaster update -- read the socket, hand
// what arrived to the game and send what is queued -- instead of only the send half. The
// client then answers the server (and sees its updates) up to a frame sooner. Packets are
// handled between phases rather than only at frame start.
void Hooked_FramePhase(void *game, int64_t phase) {
    Real_FramePhase(game, phase);
    int64_t *h = g_handle;
    if (!h || GetCurrentThreadId() != g_main_thread) return;
    if (g_drain) {
        g_in_flush = true;
        g_flush_phase = (int)(phase & 3);
        float *gate = (float *)((uint8_t *)*h + 0x450);
        float old_gate = *gate;
        *gate = 0.0f;
        __try {
            Real_BcUpdate(h);
            g_drains++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_drain = 0;
            Log("broadcaster update threw mid-frame; drain disabled");
        }
        *gate = old_gate;
        g_in_flush = false;
        return;
    }
    if (!g_flush) return;
    g_in_flush = true;
    g_flush_phase = (int)(phase & 3);
    // SendFlush skips itself if the last flush was under +0x450 seconds (1 ms) ago; the
    // frame's own flush is often just that close, so lift the gate for this call.
    float *gate = (float *)((uint8_t *)*h + 0x450);
    float old_gate = *gate;
    *gate = 0.0f;
    __try {
        SendFlush(h, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_flush = 0;
        Log("SendFlush threw; flush disabled");
    }
    *gate = old_gate;
    g_in_flush = false;
}

// Before a state sender runs: remember its own intervals the first time, then either
// force sending every frame or put the game's intervals back.
void SetInterval(void *cs, const char *what) {
    uint64_t *iv = (uint64_t *)((uint8_t *)cs + 0x150);
    int i = 0;
    while (i < g_nsenders && g_senders[i].cs != cs) ++i;
    if (i == g_nsenders) {
        if (g_nsenders == 4 || iv[0] <= 1 || iv[1] <= 1) return;   // full, or not a value we know
        g_senders[g_nsenders++] = { cs, iv[0], iv[1] };
        Log("%s sender %p: interval %llu (under 6 players) / %llu (6+)", what, cs,
            (unsigned long long)iv[0], (unsigned long long)iv[1]);
    }
    if (g_every_frame) { iv[0] = 1; iv[1] = 1; }
    else { iv[0] = g_senders[i].under6; iv[1] = g_senders[i].six_up; }
}

uint64_t Hooked_PhysSend(void *cs) {
    SetInterval(cs, "physics state");
    return Real_PhysSend(cs);
}

uint64_t Hooked_PlayerSend(void *cs) {
    SetInterval(cs, "player state");
    return Real_PlayerSend(cs);
}

void Hooked_BcSend(void *bc, uint64_t sym, uint32_t a3, void *a4, int64_t a5, void *a6, int64_t a7,
                   void *a8, int64_t a9, uint32_t a10, int64_t a11) {
    if (g_trace && t_send_depth == 0) {   // the outermost call: Send recurses per peer
        uintptr_t caller = (uintptr_t)_ReturnAddress() - (uintptr_t)g_base;
        uint64_t frame = g_frame;
        AcquireSRWLockExclusive(&g_sites_lock);
        int i = 0;
        while (i < g_nsites && !(g_sites[i].caller == caller && g_sites[i].sym == sym)) ++i;
        if (i == g_nsites && g_nsites < (int)(sizeof g_sites / sizeof g_sites[0]))
            g_sites[g_nsites++] = { caller, sym, 0, 0, 0, ~0ull, 0 };
        if (i < g_nsites) {
            SendSite &x = g_sites[i];
            x.total++;
            (frame & 1 ? x.odd : x.even)++;
            if (x.last_frame != frame) { x.frames++; x.last_frame = frame; }
        }
        ReleaseSRWLockExclusive(&g_sites_lock);
    }
    t_send_depth++;
    Real_BcSend(bc, sym, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    t_send_depth--;
}

// Tag the game socket's traffic to `to` as voice (qWAVE): Windows then marks it with
// DSCP / 802.1p priority, which Wi-Fi (WMM) and QoS-aware routers send ahead of bulk
// traffic. No admin rights needed; nothing changes if the network ignores it.
void QosAdd(SOCKET s, const sockaddr *to, int tolen) {
    if (!to || tolen <= 0 || tolen > (int)sizeof(sockaddr_storage)) return;
    AcquireSRWLockExclusive(&g_qos_lock);
    if (s != g_qos_socket) { g_qos_socket = s; g_qos_ndest = 0; }   // the game reopened its socket
    bool known = false;
    for (int i = 0; i < g_qos_ndest && !known; ++i) known = memcmp(&g_qos_dest[i], to, tolen) == 0;
    if (!known && g_qos_ndest < 8) {
        memset(&g_qos_dest[g_qos_ndest], 0, sizeof(sockaddr_storage));
        memcpy(&g_qos_dest[g_qos_ndest++], to, tolen);
        if (!g_qos_handle) {
            QOS_VERSION v = { 1, 0 };
            if (!QOSCreateHandle(&v, &g_qos_handle)) { Log("qos: QOSCreateHandle failed (%lu)", GetLastError()); g_qos_handle = nullptr; }
        }
        if (g_qos_handle) {
            QOS_FLOWID flow = 0;
            char addr[64] = "?";
            DWORD alen = sizeof addr;
            WSAAddressToStringA((LPSOCKADDR)to, tolen, nullptr, addr, &alen);
            if (QOSAddSocketToFlow(g_qos_handle, s, (PSOCKADDR)to, QOSTrafficTypeVoice, QOS_NON_ADAPTIVE_FLOW, &flow))
                Log("qos: voice priority for traffic to %s (flow %lu)", addr, flow);
            else
                Log("qos: QOSAddSocketToFlow to %s failed (%lu)", addr, GetLastError());
        }
    }
    ReleaseSRWLockExclusive(&g_qos_lock);
}

int WSAAPI Hooked_sendto(SOCKET s, const char *buf, int len, int flags, const sockaddr *to, int tolen) {
    int64_t *h = g_handle;
    if (h && s == *(SOCKET *)(Port(h) + 8) && g_qos) QosAdd(s, to, tolen);
    if (h && GetCurrentThreadId() == g_main_thread && s == *(SOCKET *)(Port(h) + 8)) {
        if (g_in_update) g_s.sends_in_update++;
        else if (g_in_flush) {
            g_s.sends_early++;
            g_s.early_offset_ticks += Now() - g_s.frame_start;
            g_s.phase_sends[g_flush_phase]++;
        } else g_s.sends_other++;
    }
    return Real_sendto(s, buf, len, flags, to, tolen);
}

void Install() {
    QueryPerformanceFrequency(&g_freq);
    ReadIni();
    auto base = (uint8_t *)GetModuleHandleW(nullptr);
    g_base = base;
    if (strstr(GetCommandLineA(), "-server")) { Log("server: nothing to do"); return; }
    if (!Check(base + BC_UPDATE_RVA, BC_UPDATE_BYTES, sizeof(BC_UPDATE_BYTES), "CBroadcaster::Update") ||
        !Check(base + FRAME_PHASE_RVA, FRAME_PHASE_BYTES, sizeof(FRAME_PHASE_BYTES), "CR15Game frame phase") ||
        !Check(base + SEND_FLUSH_RVA, SEND_FLUSH_BYTES, sizeof(SEND_FLUSH_BYTES), "SendFlush") ||
        !Check(base + BC_SEND_RVA, BC_SEND_BYTES, sizeof(BC_SEND_BYTES), "CBroadcaster::Send"))
        return;
    Real_BcUpdate = (bc_update_fn)(base + BC_UPDATE_RVA);
    Real_FramePhase = (frame_phase_fn)(base + FRAME_PHASE_RVA);
    SendFlush = (send_flush_fn)(base + SEND_FLUSH_RVA);
    Real_BcSend = (bc_send_fn)(base + BC_SEND_RVA);
    bool senders = Check(base + PHYS_SEND_RVA, PHYS_SEND_BYTES, sizeof(PHYS_SEND_BYTES), "physics state sender") &&
                   Check(base + PLAYER_SEND_RVA, PLAYER_SEND_BYTES, sizeof(PLAYER_SEND_BYTES), "player state sender");
    Real_PhysSend = (cs_send_fn)(base + PHYS_SEND_RVA);
    Real_PlayerSend = (cs_send_fn)(base + PLAYER_SEND_RVA);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (senders) {
        DetourAttach(&(PVOID &)Real_PhysSend, Hooked_PhysSend);
        DetourAttach(&(PVOID &)Real_PlayerSend, Hooked_PlayerSend);
    }
    DetourAttach(&(PVOID &)Real_BcUpdate, Hooked_BcUpdate);
    DetourAttach(&(PVOID &)Real_FramePhase, Hooked_FramePhase);
    DetourAttach(&(PVOID &)Real_BcSend, Hooked_BcSend);
    DetourAttach(&(PVOID &)Real_sendto, Hooked_sendto);
    LONG rc = DetourTransactionCommit();
    Log(rc == NO_ERROR ? "hooked (qpc %lld Hz)" : "hook failed (%lld)", rc == NO_ERROR ? g_freq.QuadPart : (long long)rc);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        GetModuleFileNameA(self, g_dir, MAX_PATH);
        if (char *slash = strrchr(g_dir, '\\')) slash[1] = 0;
        Install();
    }
    return TRUE;
}
