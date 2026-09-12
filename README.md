# Casio LK-S250 Light Cycling Plugin for Synthesia 10.9

[![License: GPLv3](https://img.shields.io/github/license/musicastudio/Synthesia.strobe)](https://github.com/musicastudio/Synthesia.strobe/blob/main/LICENSE)
[![Version](https://img.shields.io/github/v/release/musicastudio/Synthesia.strobe?color=7a39fb)](https://github.com/musicastudio/Synthesia.strobe/releases/latest)

**[Download the latest release](https://github.com/musicastudio/Synthesia.strobe/releases/latest)**

## Background

### Synthesia

[Synthesia](https://www.synthesiagame.com/) is a piano app that shows music as notes falling towards a keyboard on screen. Some keyboards can light their own keys, and Synthesia can drive those lights so the next note to play glows on the instrument itself rather than only on screen. You can then learn a piece while looking at your hands.

I own the Casiotone LK-S250 and purchased Synthesia because it supported driving the lights on this keyboard through the **Proprietary 2** setting in the Key Lights list.

What I didn't realize at the time was that the LK-S250 can only light four keys at once. Synthesia sends a light message for every note that should be glowing, but the keyboard can only display four of them, so once a chord or a passage needs more than four, the ones beyond the fourth simply never light.

### The idea

In 2021 I posted a thread on the Synthesia forum called "Key light cycling - Casio LK-S250" (`https://www.synthesiagame.com/forum/viewtopic.php?f=5&t=10217`, now dead since the forum has closed) suggesting a way around the four-key limit. Rather than letting the extra notes vanish, cycle the lights, showing four, then the next few, then the next, fast enough that all of them read as lit.

The author promised to implement it in a future version, but understandably it was low priority, and development of Synthesia appears to have ceased in 2022.

### The implementation

By hooking into Synthesia I have been able to now create a plugin that delivers these features to help myself and fellow Casio LK-S250 / Synthesia users.

## Install

Run `Synthesia.strobe-Setup.exe` from the [latest release](https://github.com/musicastudio/Synthesia.strobe/releases/latest). It asks for one thing, the folder Synthesia is installed in, and defaults to the usual location. It needs administrator rights, since that folder is under Program Files. Uninstall it from Add or Remove Programs the normal way.

If you would rather not run an installer, `version.dll` is in the release as well. Copy it into Synthesia's program folder yourself, normally

```
C:\Program Files (x86)\Synthesia
```

That is the whole install either way. To uninstall a manual copy, delete the file again.

In Synthesia, open Settings then Music Devices and set the LK-S250's **Key Lights** to **Proprietary 2**, which is the Casio scheme. That is the only Synthesia setting involved, and it is the same one the keyboard needs without the plugin.

There is no window, no tray icon and nothing to configure. Nothing is registered, no service is installed, and no Synthesia file is modified. Synthesia loads `version.dll` from its own folder because Windows searches the application directory before System32, and all seventeen entry points the real DLL exports are passed straight through to it, so Synthesia and everything else in the process behave exactly as before.

## How it works

The keys Synthesia wants lit are sorted by pitch and split into adjacent blocks of up to four, and each block is shown for 50 ms in turn. Ten held keys come out as three turns of 4, 3 and 3, a 150 ms cycle in which each key blinks about 6.7 times a second.

Four keys or fewer is a single block, so nothing cycles and the lights just stay on. The cycling only appears when more keys are wanted than the keyboard can show, which means ordinary playing is untouched.

```
10 keys, 4 lamps    {0,1,2,3} {4,5,6} {7,8,9}     3 turns, 6.7 flashes a second
 4 keys, 4 lamps    {0,1,2,3}                     steady, no cycling
```

The 50 ms is the length of a turn rather than a per-key rate, because the keyboard drops a light shown for much less than about 45 ms. With a fixed turn the per-key rate looks after itself and falls as the music thickens, instead of having to be turned down by hand.

Synthesia drives these lights with Casio SysEx, and the plugin attaches to the two MIDI output entry points those bytes can leave through, `midiOutLongMsg` for the classic backend and `winrt_midi_out_port_send` for the optional Windows 10 UWP one. It also watches `midiOutClose`, so nothing is left lit when Synthesia releases the device.

It matches the 10-byte Casio light pattern and passes everything else through untouched, including Synthesia's own keepalive and all ordinary music. Because it matches the finished message rather than calling into Synthesia, it holds no Synthesia addresses and a Synthesia update cannot break it.

The design and the reverse-engineering notes are in [docs/design.md](docs/design.md) and [docs/hooks.md](docs/hooks.md).

## How it was made

Using Claude, `Synthesia.exe` was disassembled with [Ghidra](https://ghidra-sre.org/) and the decompiled C was read to find where the key lights come from. That turned up the Casio message templates in `.rdata`, the dispatch that rewrites notes into vendor SysEx, the 180 ms keepalive that holds the keyboard's light display open, and the note range Synthesia clamps to.

The analysis also settled the design question. The obvious hook, Synthesia's internal light logic, turned out to be the generic MIDI send for the whole program, reached from about fifty call sites and deciding from device state whether a note becomes a light. Hooking the finished SysEx at the MIDI output boundary is both simpler and version-proof, and the decompilation is what proved there are exactly two such boundaries and that swallowing a message at either one cannot stall Synthesia. Those notes are in [docs/hooks.md](docs/hooks.md).

To be clear about what that means, this repository contains no Synthesia source and no decompiled Synthesia content. The analysis only informed where the plugin attaches at runtime, and Synthesia's own files are never touched on disk.

## Build

Needs Visual Studio 2022 (x64), CMake, and the vendored submodule. Synthesia is a 64-bit program despite installing under `Program Files (x86)`, so the build must be x64; the CMake file refuses anything else.

```bash
git submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

That produces `build\Release\version.dll`, which is the whole plugin at about 34 KB.

To build the installer as well, which needs [Inno Setup 6](https://jrsoftware.org/isinfo.php):

```bash
powershell -ExecutionPolicy Bypass -File installer\build_installer.ps1
```

That writes `installer\Output\Synthesia.strobe-Setup.exe`.

### Tests

`build\Release\strobe_test.exe` runs the cycling logic against its invariants with no keyboard and no Synthesia, and is the check to run after touching the rotation.

```bash
build\Release\strobe_test.exe
```

`tools\test_hook_loopback.py` is the end-to-end check. It creates a virtual MIDI port, loads the built DLL so its hook is live, sends ten key-light messages the same way Synthesia does, and records what actually reaches the port. It also compares the DLL's export table against the real `version.dll`. It needs [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html) installed for the teVirtualMIDI driver.

```bash
python tools\test_hook_loopback.py
```

## Disassembly

The Ghidra project and the decompilation database live outside this repo, beside the binaries, so nothing large lands in git.

```bash
python tools\build_synthesia_ghidra.py        # import, analyze, decompile into decomp.db
python tools\export_ghidra_meta.py            # add symbols and xrefs tables
```

Set `GHIDRA_INSTALL_DIR` and `SYNTHESIA_DISASM` to override the default locations. A full run takes about 16 minutes and yields 10,745 functions, 135,591 symbols and 464,565 cross references. Progress is committed as it goes, so an interrupted run resumes where it stopped.

## Layout

```
synthesia.strobe/
  src/strobe.h                  the cycling logic and its self-check
  src/synthesia_strobe.cpp      the proxy, the detours, and the turn clock
  src/version.def               the forwarded exports
  docs/                         design rationale and reverse-engineering notes
  tools/                        Ghidra helpers and the two tests
  installer/                    Inno Setup script and its build script
  third_party/minhook           vendored hooking library
  ../SYNTHESIA_DISASM/          binaries and the Ghidra project (not in git)
```

## Status

The cycling logic passes its self-check, and the plugin is verified end to end against a real WinMM MIDI port. The hook installs, Synthesia-style light messages are swallowed, the port sees rotated blocks of at most four keys with all ten getting a turn, and closing the port leaves nothing lit. It has not yet driven a real LK-S250; see [docs/status.md](docs/status.md) for exactly what has and has not been exercised.

## Licence

Released under the GNU General Public License v3.0; see [LICENSE](LICENSE).

It is an independent interoperability add-on for software you already own. It ships no Synthesia code or content, never modifies Synthesia on disk, and only forwards the system `version.dll` entry points Synthesia asks for. The loopback test loads `teVirtualMIDI64.dll` at run time if it is present and neither ships nor requires it.
