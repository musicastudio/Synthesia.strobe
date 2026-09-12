# Synthesia key-light reference

Reverse-engineering notes for Synthesia 10.9 (Windows x64, released 2022-12-21; the shipped
`Synthesia.exe` is dated 2023-02-27, PE TimeDateStamp `0x63fccac1`, image base `0x140000000`,
20,153,848 bytes). All calls are Microsoft x64 (RCX, RDX, R8, R9). Addresses are static virtual
addresses, so subtract the image base for an RVA and add the runtime ASLR delta.

Everything below was read out of Ghidra's decompiler output for that build. It is recorded
because it explains where the key lights come from and why the plugin attaches where it does.
**None of these addresses are used at runtime**, so a Synthesia update cannot break the plugin.
The reason is in "Why the hook is not here" at the end.

## 1. The key-light path

Synthesia's Key Lights setting picks a *light channel*, stored as `lightChannel` in
`%APPDATA%\Synthesia\multiDevice.xml`. Negative values are the vendor-specific schemes shown in
the settings list as "Proprietary N".

| lightChannel | Settings list | Device family |
|---|---|---|
| `-4` | Proprietary 2 | Casio LK-S250 |
| `-5` | Proprietary 3 | Vangoa |

Light output is not a separate subsystem. Every MIDI message Synthesia sends passes through one
dispatch function, and that function rewrites note messages into the vendor SysEx when the
destination device is configured for a proprietary scheme.

| Target | Role | Address | Signature |
|---|---|---|---|
| MIDI send dispatch | Every outbound message; rewrites notes into vendor key-light SysEx | `0x14011f850` | `void f(MidiOut* this, MidiEvent* ev, uint dest)` |
| Device init | Sends the scheme's enable SysEx when a device is opened | `0x14011e630` | `void f(Device* this)` |
| Device tick | Per-frame update; emits the Casio keepalive every 180 ms | `0x140120280` | `u8 f(Device* this, Renderer*, char)` |
| SysEx message builder | Wraps a `{bytes, length}` span into a MIDI message record | `0x140246e80` | `u32 f(u32, Span*)` |
| MIDI message kind | Classifies an event; 3 = note off, 4 = note on | `0x1402481e0` | `int f(MidiEvent*)` |
| WinMM SysEx send | The only `midiOutLongMsg` call site in the binary | `0x1402d35fa` | inside `f(WinMidiOut*, ...)` |
| WinMM port open | The only `midiOutOpen` call site | `0x1402d3489` | `fdwOpen = 0x50000` (CALLBACK_EVENT) |

The dispatch at `0x14011f850` is reached from roughly fifty call sites across the binary, which
is what identifies it as the common MIDI send rather than a light-specific routine.

## 2. The Casio LK-S250 wire protocol

Message templates live in `.rdata` in 16-byte slots.

| Address | Bytes | Meaning |
|---|---|---|
| `0x14043c7d8` | `F0 44 7E 7E 7F 00 03 F7` | keepalive, holds the light display open |
| `0x14043c7e8` | `F0 44 7E 7E 7F 02 00 00 00 F7` | key light, note at byte 7, on/off at byte 8 |
| `0x14043c7f8` | `F0 44 7E 7E 7F 00 06 00 F7` | device init |
| `0x14043c808` | `F0 00 20 2B 69 00 00 55 79 F7` | Vangoa init |
| `0x14043c818` | `F0 00 20 2B 69 16 02 00 F7` | Vangoa key light |

The Casio branch of `0x14011f850` copies the 10-byte template into a stack buffer, patches two
bytes, and hands it to the message builder.

```c
if (lightChannel == -4) {                       // Proprietary 2
    if (note < 0x15 || note > 0x6c) goto skip;  // 21..108, the 88-key piano range
    buf[0..6] = DAT_14043c7e8;                  // F0 44 7E 7E 7F 02 00
    buf[7]    = note;
    buf[8]    = (kind == 4 && velocity != 0) ? 1 : 0;   // note-on with velocity lights it
    buf[9]    = 0xF7;
    send(buf, 10);
}
```

So a light on is `F0 44 7E 7E 7F 02 00 <note> 01 F7` and a light off is the same with `00`.
Notes outside 21..108 are dropped by Synthesia before they reach the keyboard.

The keepalive comes from the device tick at `0x140120280`, which walks the device list and, for
every device whose `lightChannel` is `-4`, sends the 8-byte keepalive, then schedules the next
one 180,000,000 ns later.

```c
for (dev : devices)
    if (dev->lightChannel == -4)
        send(DAT_14043c7d8, 8);
next_keepalive = now_ns + 180000000;            // 180 ms
```

Without that keepalive the keyboard closes its light display and ignores light messages that
arrive on their own, which is why anything driving these lights has to keep it going.

## 3. The two MIDI backends

Synthesia can send through either of two backends, which is the single most important fact for
deciding where to attach.

**Classic WinMM.** Statically imported. `midiOutOpen` is called once, at `0x1402d3489`, with
`fdwOpen = 0x50000` (CALLBACK_EVENT). SysEx goes out through the only `midiOutLongMsg` call site
in the binary, at `0x1402d35fa`, in this sequence.

```
midiOutPrepareHeader(hmo, &hdr, 0x70)
ResetEvent(dev->event)
midiOutLongMsg(hmo, &hdr, 0x70)
midiOutUnprepareHeader(hmo, &hdr, 0x70)
```

Synthesia never waits for the buffer to complete. It calls `midiOutUnprepareHeader` immediately
and keeps the return code. That is what makes it safe for the plugin to swallow a message
without the driver ever signalling completion.

**Windows 10 UWP.** `win10-midi.dll` ships beside `Synthesia.exe` and is loaded on demand with
`LoadLibrary`, so it has no import entry. Synthesia's own release notes record that this backend
is disabled by default. It exports one send entry point.

```c
int winrt_midi_out_port_send(void* port, const unsigned char* data, unsigned length);
```

Confirmed from its prologue at RVA `0x17c0`, which takes `port` in RCX, `data` in RDX and
`length` in R8D, brackets the send with a lock, and returns 0.

## 4. Why the hook is not here

Everything above says the light logic lives at `0x14011f850`, and hooking it would be the
obvious move. The plugin does not do that.

That function is the generic MIDI send. It receives an already-built event and decides, from
device state, whether to rewrite it as a light. Detouring it means reproducing that decision
from outside, against a 2 KB function with heavily duplicated branch structure, and pinning the
plugin to one build's addresses.

The vendor SysEx is a far better boundary. By the time the bytes exist they are unambiguous, and
both backends expose a single exported send entry point carrying them. So the plugin detours
those two exports instead.

| Hook | Module | Carries |
|---|---|---|
| `midiOutLongMsg` | `winmm.dll` | every SysEx on the classic backend |
| `winrt_midi_out_port_send` | `win10-midi.dll` | every message on the UWP backend |

Both are documented, exported, and stable, so the plugin holds no Synthesia addresses at all and
identifies light traffic purely by matching the 10-byte Casio pattern. The analysis above is
what established that those two points are genuinely the only ways the light bytes can leave,
and that swallowing a message at the WinMM one cannot stall Synthesia.
