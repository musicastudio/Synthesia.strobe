#!/usr/bin/env python3
"""End-to-end check of the built version.dll against a real WinMM MIDI port.

Creates a virtual MIDI port with teVirtualMIDI (installed by loopMIDI), loads the built
version.dll into this process so its midiOutLongMsg hook is live, then sends Casio key-light
SysEx to that port exactly the way Synthesia does (prepare, long message, unprepare) and
records what actually reaches the port.

Without the DLL every message passes straight through, so all ten keys are "lit" at once.
With it, the light messages are swallowed and the strobe re-emits adjacent blocks of four,
so the port should never hold more than four keys lit and should cycle through all ten.

    python tools/test_hook_loopback.py

Needs loopMIDI installed for the teVirtualMIDI driver, and the Release build present.
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import sys
import time
from pathlib import Path

DLL = Path(__file__).resolve().parents[1] / "build" / "Release" / "version.dll"
PORT_NAME = "synthesia.strobe test"

LIGHT_PREFIX = bytes([0xF0, 0x44, 0x7E, 0x7E, 0x7F, 0x02, 0x00])
KEEPALIVE = bytes([0xF0, 0x44, 0x7E, 0x7E, 0x7F, 0x00, 0x03, 0xF7])
TURN_MS = 50
MAX_LIT = 4

mm = ctypes.WinDLL("winmm")
te = ctypes.WinDLL("teVirtualMIDI64.dll")


class MIDIHDR(ctypes.Structure):
    _fields_ = [
        ("lpData", ctypes.c_char_p),
        ("dwBufferLength", wt.DWORD),
        ("dwBytesRecorded", wt.DWORD),
        ("dwUser", ctypes.c_void_p),
        ("dwFlags", wt.DWORD),
        ("lpNext", ctypes.c_void_p),
        ("reserved", ctypes.c_void_p),
        ("dwOffset", wt.DWORD),
        ("dwReserved", ctypes.c_void_p * 8),
    ]


class CAPS(ctypes.Structure):
    _fields_ = [
        ("wMid", ctypes.c_ushort), ("wPid", ctypes.c_ushort),
        ("vDriverVersion", ctypes.c_uint), ("szPname", ctypes.c_wchar * 32),
        ("wTechnology", ctypes.c_ushort), ("wVoices", ctypes.c_ushort),
        ("wNotes", ctypes.c_ushort), ("wChannelMask", ctypes.c_ushort),
        ("dwSupport", ctypes.c_uint),
    ]


CALLBACK = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, ctypes.POINTER(ctypes.c_ubyte),
                              wt.DWORD, ctypes.c_void_p)

te.virtualMIDICreatePortEx2.restype = ctypes.c_void_p
te.virtualMIDICreatePortEx2.argtypes = [wt.LPCWSTR, CALLBACK, ctypes.c_void_p, wt.DWORD, wt.DWORD]
te.virtualMIDIClosePort.argtypes = [ctypes.c_void_p]


def find_out(name: str) -> int:
    for i in range(mm.midiOutGetNumDevs()):
        caps = CAPS()
        if mm.midiOutGetDevCapsW(i, ctypes.byref(caps), ctypes.sizeof(caps)) == 0:
            if caps.szPname == name or caps.szPname.startswith(name[:31]):
                return i
    return -1


def send_sysex(handle, data: bytes) -> int:
    """Send exactly the way Synthesia does: prepare, long message, unprepare, no wait."""
    buf = ctypes.create_string_buffer(data, len(data))
    hdr = MIDIHDR()
    hdr.lpData = ctypes.cast(buf, ctypes.c_char_p)
    hdr.dwBufferLength = hdr.dwBytesRecorded = len(data)
    if mm.midiOutPrepareHeader(handle, ctypes.byref(hdr), ctypes.sizeof(hdr)) != 0:
        return -1
    err = mm.midiOutLongMsg(handle, ctypes.byref(hdr), ctypes.sizeof(hdr))
    mm.midiOutUnprepareHeader(handle, ctypes.byref(hdr), ctypes.sizeof(hdr))
    return err


def light(note: int, on: bool) -> bytes:
    return LIGHT_PREFIX + bytes([note, 1 if on else 0, 0xF7])


def check_proxy(dll) -> bool:
    """The three forwarded version.dll exports must behave like the real System32 ones.

    Synthesia calls these during startup, so a broken forward would break Synthesia itself.
    """
    real = ctypes.WinDLL("C:\\Windows\\System32\\version.dll")
    target = "C:\\Windows\\System32\\kernel32.dll"
    ok = True
    for mod, label in ((real, "system"), (dll, "proxy")):
        mod.GetFileVersionInfoSizeW.restype = wt.DWORD
        mod.GetFileVersionInfoSizeW.argtypes = [wt.LPCWSTR, ctypes.POINTER(wt.DWORD)]
    handle = wt.DWORD(0)
    want = real.GetFileVersionInfoSizeW(target, ctypes.byref(handle))
    got = dll.GetFileVersionInfoSizeW(target, ctypes.byref(handle))
    if want == 0 or want != got:
        print(f"FAIL: GetFileVersionInfoSizeW returned {got}, the system DLL returns {want}")
        ok = False

    buf = ctypes.create_string_buffer(got)
    if not dll.GetFileVersionInfoW(target, 0, got, buf):
        print("FAIL: GetFileVersionInfoW through the proxy failed")
        ok = False
    else:
        val = ctypes.c_void_p()
        ln = ctypes.c_uint(0)
        if not dll.VerQueryValueW(buf, "\\", ctypes.byref(val), ctypes.byref(ln)) or ln.value == 0:
            print("FAIL: VerQueryValueW through the proxy failed")
            ok = False
    print("version.dll proxy forwarding:", "PASS" if ok else "FAIL")
    return ok


def main() -> int:
    if not DLL.exists():
        print(f"FAIL: {DLL} not built")
        return 1

    received: list[bytes] = []

    def on_data(port, data, length, inst):
        received.append(bytes(data[i] for i in range(length)))

    cb = CALLBACK(on_data)
    port = te.virtualMIDICreatePortEx2(PORT_NAME, cb, None, 65535, 1)
    if not port:
        print("FAIL: could not create the virtual MIDI port (is loopMIDI installed?)")
        return 1

    try:
        # Load the plugin into this process; its hook goes on winmm's midiOutLongMsg.
        plugin = ctypes.WinDLL(str(DLL))
        if not plugin:
            print("FAIL: could not load version.dll")
            return 1
        time.sleep(0.5)  # the DLL installs its hook on a worker thread

        proxy_ok = check_proxy(plugin)

        dev = find_out(PORT_NAME)
        if dev < 0:
            print("FAIL: the virtual port did not appear in the WinMM output list")
            return 1

        handle = ctypes.c_void_p()
        if mm.midiOutOpen(ctypes.byref(handle), dev, None, None, 0) != 0:
            print("FAIL: could not open the virtual port")
            return 1

        notes = [60, 62, 64, 65, 67, 69, 71, 72, 74, 76]   # ten keys, well over the limit
        received.clear()
        for n in notes:
            send_sysex(handle, light(n, True))

        time.sleep(0.6)   # about a dozen turns at 50 ms
        mm.midiOutClose(handle)
        time.sleep(0.1)
    finally:
        te.virtualMIDIClosePort(port)

    # ---- what actually reached the port ----
    lights = [m for m in received if len(m) == 10 and m[:7] == LIGHT_PREFIX]
    keepalives = [m for m in received if m == KEEPALIVE]
    print(f"messages at the port: {len(received)} "
          f"({len(lights)} light, {len(keepalives)} keepalive)")

    if not lights:
        print("FAIL: no light messages reached the port at all")
        return 1

    ok = proxy_ok
    lit: set[int] = set()
    peak = 0
    seen: set[int] = set()
    for m in lights:
        note, on = m[7], m[8] == 1
        if on:
            lit.add(note)
            seen.add(note)
        else:
            lit.discard(note)
        peak = max(peak, len(lit))

    print(f"peak keys lit at once: {peak}")
    print(f"distinct keys that got a turn: {len(seen)} of {len(notes)}")

    if peak > MAX_LIT:
        print(f"FAIL: {peak} keys lit at once, over the LK-S250 limit of {MAX_LIT}")
        ok = False
    if peak <= 1:
        print("FAIL: the lights never came on together, so nothing was really driven")
        ok = False
    if seen != set(notes):
        print(f"FAIL: these keys never lit: {sorted(set(notes) - seen)}")
        ok = False
    if len(lights) <= len(notes):
        print("FAIL: no rotation happened, the messages just passed through")
        ok = False
    # midiOutClose is hooked so the last block is turned off while the port still works,
    # rather than being left lit on the keyboard.
    if lit:
        print(f"FAIL: {sorted(lit)} still lit after the port closed")
        ok = False

    print("hook loopback test:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
