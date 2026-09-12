// synthesia.strobe: key-light strobing for the Casio LK-S250, as a single drop-in DLL.
//
// Synthesia's "Proprietary 2" key-light scheme drives the LK-S250 with Casio SysEx, but the
// keyboard shows at most four lights at once and silently drops the rest. This DLL intercepts
// those light messages and time-multiplexes them: the requested keys are sorted by pitch,
// split into adjacent blocks of four, and each block is shown for 50 ms in turn, so every
// requested key blinks instead of the extras never appearing.
//
// It loads because Synthesia imports version.dll and the application directory is searched
// ahead of System32, so dropping this one file beside Synthesia.exe is the entire install.
// The three version.dll entry points Synthesia uses are forwarded to the real System32 copy.
//
// The intercept sits on the MIDI output calls rather than inside Synthesia, so it needs no
// hard-coded addresses in Synthesia.exe and keeps working across both of Synthesia's MIDI
// backends. See docs/hooks.md for the analysis behind that choice.
#include <windows.h>
#include <mmsystem.h>

#include <MinHook.h>

#include <atomic>
#include <mutex>

#include "strobe.h"

#pragma comment(lib, "winmm.lib")

// Present only in Windows 10 1803 and later SDKs; the flag is simply ignored by older kernels,
// where CreateWaitableTimerExW falls back to a normal timer.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

// ---- Casio LK-S250 key-light protocol, as Synthesia's "Proprietary 2" scheme sends it ----
// F0 44 7E 7E 7F 02 00 <note> <on> F7   light on (on=1) or off (on=0)
// F0 44 7E 7E 7F 00 03 F7               keepalive, which holds the light display open
constexpr BYTE kLightPrefix[] = {0xF0, 0x44, 0x7E, 0x7E, 0x7F, 0x02, 0x00};
constexpr DWORD kLightLen = 10;
constexpr BYTE kKeepalive[] = {0xF0, 0x44, 0x7E, 0x7E, 0x7F, 0x00, 0x03, 0xF7};

constexpr int kTurnMs = 50;        // how long each turn shows its block of keys
constexpr int kMaxLit = 4;         // the LK-S250 hardware limit
constexpr int kKeepaliveMs = 150;  // only used when Synthesia is not sending its own

bool IsLight(const BYTE* d, DWORD n) {
    return n == kLightLen && memcmp(d, kLightPrefix, sizeof kLightPrefix) == 0 && d[9] == 0xF7;
}

bool IsKeepalive(const BYTE* d, DWORD n) {
    return n == sizeof kKeepalive && memcmp(d, kKeepalive, sizeof kKeepalive) == 0;
}

// ---- state ----------------------------------------------------------------------------
// Synthesia can send through either classic WinMM or the optional win10-midi.dll (UWP)
// backend. We learn which one is carrying the lights from the traffic itself and answer on
// the same one, so neither backend has to be configured or detected up front.
enum class Sink { None, WinMM, Uwp };

using MidiOutLongMsgFn = MMRESULT(WINAPI*)(HMIDIOUT, LPMIDIHDR, UINT);
using MidiOutCloseFn = MMRESULT(WINAPI*)(HMIDIOUT);
using UwpSendFn = int (*)(void*, const BYTE*, unsigned);

std::mutex g_lock;                 // guards the strobe, the sink, and our own sends
Strobe* g_strobe = nullptr;
Sink g_sink = Sink::None;
HMIDIOUT g_winmmOut = nullptr;
void* g_uwpPort = nullptr;
MidiOutLongMsgFn g_origLongMsg = nullptr;
MidiOutCloseFn g_origClose = nullptr;
UwpSendFn g_origUwpSend = nullptr;
std::atomic<DWORD> g_lastKeepalive{0};
std::atomic<bool> g_uwpHooked{false};

// Send raw bytes out whichever backend the intercepted lights arrived on, always through the
// original function so we never re-enter our own detour. Called with g_lock held.
void SendRaw(const BYTE* data, DWORD len) {
    if (g_sink == Sink::Uwp) {
        if (g_origUwpSend && g_uwpPort) g_origUwpSend(g_uwpPort, data, len);
        return;
    }
    if (g_sink != Sink::WinMM || !g_winmmOut || !g_origLongMsg) return;

    BYTE buf[16];
    if (len > sizeof buf) return;
    memcpy(buf, data, len);

    MIDIHDR hdr{};
    hdr.lpData = reinterpret_cast<LPSTR>(buf);
    hdr.dwBufferLength = hdr.dwBytesRecorded = len;
    if (midiOutPrepareHeader(g_winmmOut, &hdr, sizeof hdr) != MMSYSERR_NOERROR) return;
    if (g_origLongMsg(g_winmmOut, &hdr, sizeof hdr) == MMSYSERR_NOERROR) {
        // ponytail: spin-wait for the driver, since the buffer is a stack local. A 10-byte
        // SysEx is gone in well under a millisecond; the cap keeps a wedged driver from
        // taking the strobe thread down with it.
        for (int i = 0; i < 200 && !(hdr.dwFlags & MHDR_DONE); ++i) Sleep(1);
    }
    midiOutUnprepareHeader(g_winmmOut, &hdr, sizeof hdr);
}

// The strobe's output: one key on or off, in the Casio light format.
void SendLight(int note, bool on) {
    BYTE m[kLightLen] = {0xF0, 0x44, 0x7E, 0x7E, 0x7F, 0x02, 0x00,
                         static_cast<BYTE>(note), static_cast<BYTE>(on ? 1 : 0), 0xF7};
    SendRaw(m, kLightLen);
}

// Feed one intercepted light message to the strobe. Returns true if we swallowed it.
bool TakeLight(const BYTE* d, DWORD n, Sink sink, HMIDIOUT winmm, void* uwpPort) {
    if (IsKeepalive(d, n)) {
        g_lastKeepalive.store(GetTickCount());
        return false;  // Synthesia's own keepalive still goes out; it holds the display open
    }
    if (!IsLight(d, n)) return false;

    std::lock_guard<std::mutex> lk(g_lock);
    g_sink = sink;
    g_winmmOut = winmm;
    g_uwpPort = uwpPort;
    g_strobe->Light(d[7], d[8] == 1);
    return true;
}

// ---- detours --------------------------------------------------------------------------
MMRESULT WINAPI MidiOutLongMsg_Detour(HMIDIOUT hmo, LPMIDIHDR pmh, UINT cbmh) {
    if (pmh && pmh->lpData) {
        const BYTE* d = reinterpret_cast<const BYTE*>(pmh->lpData);
        DWORD n = pmh->dwBytesRecorded ? pmh->dwBytesRecorded : pmh->dwBufferLength;
        if (TakeLight(d, n, Sink::WinMM, hmo, nullptr)) {
            // Swallowed, because the strobe re-emits a rotated subset instead. Report the
            // completion a driver would have, so Synthesia's immediate unprepare is clean.
            pmh->dwFlags |= MHDR_DONE;
            pmh->dwFlags &= ~MHDR_INQUEUE;
            return MMSYSERR_NOERROR;
        }
    }
    return g_origLongMsg(hmo, pmh, cbmh);
}

// Synthesia resets and closes the port together when it lets a device go. Clear our lights
// while the handle is still usable, so nothing is left lit and the turn clock stops driving a
// handle that is about to become invalid.
MMRESULT WINAPI MidiOutClose_Detour(HMIDIOUT hmo) {
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_sink == Sink::WinMM && g_winmmOut == hmo) {
            g_strobe->Clear();
            g_sink = Sink::None;
            g_winmmOut = nullptr;
        }
    }
    return g_origClose(hmo);
}

int UwpSend_Detour(void* port, const BYTE* data, unsigned len) {
    if (data && TakeLight(data, len, Sink::Uwp, nullptr, port)) return 0;
    return g_origUwpSend(port, data, len);
}

// Synthesia loads win10-midi.dll only when the UWP backend is switched on, which can happen
// after we are already running, so the tick thread keeps an eye out for it.
void TryHookUwp() {
    if (g_uwpHooked.load()) return;
    HMODULE m = GetModuleHandleW(L"win10-midi.dll");
    if (!m) return;
    void* target = reinterpret_cast<void*>(GetProcAddress(m, "winrt_midi_out_port_send"));
    if (!target) {
        g_uwpHooked.store(true);  // nothing to hook; stop looking
        return;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&UwpSend_Detour),
                      reinterpret_cast<void**>(&g_origUwpSend)) == MH_OK &&
        MH_EnableHook(target) == MH_OK) {
        g_uwpHooked.store(true);
    }
}

// ---- the turn clock -------------------------------------------------------------------
void WaitTurn(HANDLE timer, int ms) {
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(ms) * 10000;  // negative is relative, 100 ns units
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep(ms);
}

DWORD WINAPI TickThread(LPVOID) {
    timeBeginPeriod(1);
    // High-resolution timer where it exists (Windows 10 1803+); older ones fall back to Sleep.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

    for (;;) {
        TryHookUwp();
        {
            std::lock_guard<std::mutex> lk(g_lock);
            if (g_strobe->HasLit()) {
                // Synthesia sends its own keepalive while it plays. This only covers the gap
                // when it has stopped sending but keys are still held, which would otherwise
                // let the keyboard close its light display mid-strobe.
                DWORD now = GetTickCount();
                if (now - g_lastKeepalive.load() >= kKeepaliveMs) {
                    SendRaw(kKeepalive, sizeof kKeepalive);
                    g_lastKeepalive.store(now);
                }
            }
            if (g_strobe->Strobing()) g_strobe->Refresh(true);
        }
        WaitTurn(timer, kTurnMs);
    }
}

DWORD WINAPI InitThread(LPVOID) {
    // Off the loader lock: MinHook allocates and suspends threads, neither of which is safe
    // to do from DllMain.
    static Strobe strobe(&SendLight, kMaxLit);
    g_strobe = &strobe;

    if (MH_Initialize() != MH_OK) return 0;

    HMODULE winmm = GetModuleHandleW(L"winmm.dll");
    if (!winmm) winmm = LoadLibraryW(L"winmm.dll");
    if (winmm) {
        auto hook = [winmm](const char* name, void* detour, void** orig) {
            void* target = reinterpret_cast<void*>(GetProcAddress(winmm, name));
            if (target && MH_CreateHook(target, detour, orig) == MH_OK) MH_EnableHook(target);
        };
        hook("midiOutLongMsg", reinterpret_cast<void*>(&MidiOutLongMsg_Detour),
             reinterpret_cast<void**>(&g_origLongMsg));
        hook("midiOutClose", reinterpret_cast<void*>(&MidiOutClose_Detour),
             reinterpret_cast<void**>(&g_origClose));
    }
    if (!g_origLongMsg) return 0;  // nothing to intercept; leave Synthesia completely stock

    CloseHandle(CreateThread(nullptr, 0, &TickThread, nullptr, 0, nullptr));
    return 0;
}

// ---- version.dll proxy ----------------------------------------------------------------
// Synthesia is the only module in the process that imports version.dll, and it imports
// exactly these three entry points. They are forwarded to the real DLL in System32, which is
// loaded by full path so we can never find ourselves.
HMODULE g_realVersion = nullptr;

FARPROC RealVersion(const char* name) {
    if (!g_realVersion) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (!n || n > MAX_PATH - 16) return nullptr;
        wcscpy_s(path + n, MAX_PATH - n, L"\\version.dll");
        g_realVersion = LoadLibraryW(path);
    }
    return g_realVersion ? GetProcAddress(g_realVersion, name) : nullptr;
}

}  // namespace

extern "C" {

DWORD WINAPI ProxyGetFileVersionInfoSizeW(LPCWSTR file, LPDWORD handle) {
    using Fn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    auto fn = reinterpret_cast<Fn>(RealVersion("GetFileVersionInfoSizeW"));
    return fn ? fn(file, handle) : 0;
}

BOOL WINAPI ProxyGetFileVersionInfoW(LPCWSTR file, DWORD handle, DWORD len,
                                                           LPVOID data) {
    using Fn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    auto fn = reinterpret_cast<Fn>(RealVersion("GetFileVersionInfoW"));
    return fn ? fn(file, handle, len, data) : FALSE;
}

BOOL WINAPI ProxyVerQueryValueW(LPCVOID block, LPCWSTR sub, LPVOID* buf,
                                                      PUINT len) {
    using Fn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    auto fn = reinterpret_cast<Fn>(RealVersion("VerQueryValueW"));
    return fn ? fn(block, sub, buf, len) : FALSE;
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        CloseHandle(CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
