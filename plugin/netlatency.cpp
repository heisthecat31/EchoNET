// NetLatency: measure and cut the latency Echo VR (PC build 631547.rad15_live) adds on top
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
// NetLatency.ini next to the DLL, re-read every 2 s (so the fix can be A/B tested live):
//   [net]
//   flush=1
//   zero_sim=1
// NetLatency.log: every 5 s, frame time, where in the frame packets were sent, and how
// many went out early thanks to flush.

#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "detours.h"

namespace {

constexpr uintptr_t BC_UPDATE_RVA = 0xf8ea10;
constexpr uint8_t BC_UPDATE_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf9 };
constexpr uintptr_t FRAME_PHASE_RVA = 0x127420;
constexpr uint8_t FRAME_PHASE_BYTES[] = { 0x40, 0x55, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xb9, 0x48, 0x81, 0xec, 0xf0, 0x00, 0x00 };
constexpr uintptr_t SEND_FLUSH_RVA = 0xf8a280;
constexpr uint8_t SEND_FLUSH_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56 };

using bc_update_fn = void (*)(int64_t *handle);
using frame_phase_fn = void (*)(void *game, int64_t phase);
using send_flush_fn = void (*)(int64_t *handle, int now);
using sendto_fn = int (WSAAPI *)(SOCKET, const char *, int, int, const sockaddr *, int);

bc_update_fn Real_BcUpdate;
frame_phase_fn Real_FramePhase;
send_flush_fn SendFlush;
sendto_fn Real_sendto = sendto;

int64_t *volatile g_handle = nullptr;   // the game's broadcaster (the one with a socket)
DWORD g_main_thread = 0;
volatile LONG g_flush = 1, g_zero_sim = 1;
char g_dir[MAX_PATH];

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
    sprintf_s(path, "%sNetLatency.log", g_dir);
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
    sprintf_s(path, "%sNetLatency.ini", g_dir);
    LONG flush = GetPrivateProfileIntA("net", "flush", 1, path);
    LONG zero = GetPrivateProfileIntA("net", "zero_sim", 1, path);
    if (flush != g_flush || zero != g_zero_sim) Log("ini: flush=%ld zero_sim=%ld", flush, zero);
    g_flush = flush;
    g_zero_sim = zero;
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

void Hooked_FramePhase(void *game, int64_t phase) {
    Real_FramePhase(game, phase);
    int64_t *h = g_handle;
    if (!g_flush || !h || GetCurrentThreadId() != g_main_thread) return;
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

int WSAAPI Hooked_sendto(SOCKET s, const char *buf, int len, int flags, const sockaddr *to, int tolen) {
    int64_t *h = g_handle;
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
    if (strstr(GetCommandLineA(), "-server")) { Log("server: nothing to do"); return; }
    if (!Check(base + BC_UPDATE_RVA, BC_UPDATE_BYTES, sizeof(BC_UPDATE_BYTES), "CBroadcaster::Update") ||
        !Check(base + FRAME_PHASE_RVA, FRAME_PHASE_BYTES, sizeof(FRAME_PHASE_BYTES), "CR15Game frame phase") ||
        !Check(base + SEND_FLUSH_RVA, SEND_FLUSH_BYTES, sizeof(SEND_FLUSH_BYTES), "SendFlush"))
        return;
    Real_BcUpdate = (bc_update_fn)(base + BC_UPDATE_RVA);
    Real_FramePhase = (frame_phase_fn)(base + FRAME_PHASE_RVA);
    SendFlush = (send_flush_fn)(base + SEND_FLUSH_RVA);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)Real_BcUpdate, Hooked_BcUpdate);
    DetourAttach(&(PVOID &)Real_FramePhase, Hooked_FramePhase);
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
