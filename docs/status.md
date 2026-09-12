# Status

An honest account of what has been exercised and what has not.

## Verified

**Strobe logic** (`build\Release\strobe_test.exe`, passing). Runs the rotation with no keyboard
and no Synthesia and checks the invariants that matter.

- Four keys or fewer never flash, so ordinary playing is untouched.
- Ten keys over four lamps split 4/3/3 and the cycle repeats every three turns.
- Every key gets exactly one turn per cycle, and no key is ever missed.
- Each block is adjacent in pitch, not a scattered set.
- The keyboard never sees more than four keys lit, across a full run including removals.
- A released key is turned off and stops taking a turn.
- Dropping back under the limit ends the rotation and lights the remainder steadily.

**End to end against a real MIDI port** (`tools\test_hook_loopback.py`, passing). Creates a
virtual MIDI port, loads the built DLL so the hook is live, then sends ten key-light messages
through `midiOutLongMsg` exactly the way Synthesia does, prepare then long message then
unprepare with no wait, and records what actually arrives at the port.

- The hook installs on `winmm.dll!midiOutLongMsg` and intercepts real traffic.
- Light messages are swallowed rather than forwarded.
- The port sees 88 light messages where 10 were sent, so the rotation genuinely runs.
- At most four keys are lit at any moment.
- All ten keys get a turn.
- Keepalives are emitted while keys are held and nothing upstream is sending.
- Closing the port turns the remaining block off, leaving no key lit.

**Static facts about Synthesia 10.9**, read from Ghidra output for the shipped build and
recorded in [hooks.md](hooks.md). The Casio message templates and their addresses, the
`lightChannel` values for the proprietary schemes, the 21..108 note clamp, the 180 ms keepalive
interval, the single `midiOutLongMsg` and `midiOutOpen` call sites, the CALLBACK_EVENT open
flags, and the fact that Synthesia never waits for SysEx completion. The installed
`Synthesia.exe` was confirmed byte-identical to the analyzed copy.

**Install mechanics.** `version.dll` is not a KnownDLL, so an application-directory copy wins.
The built DLL is x64 and exports exactly the same seventeen names as the real System32 version,
which the loopback test checks by comparing the two export tables. Both the wide and the ANSI
query paths are called through the proxy, the ANSI one being what the display driver uses.

## Not yet verified

**Against the real keyboard.** The strobe has never driven an actual LK-S250 through this
plugin. The wire protocol, the four-lamp limit and the 45 ms floor on how briefly a light can be
shown are all established, but how the rotation looks on the hardware is unconfirmed.

**Inside Synthesia.** Every test so far drives the hook from a test process. The plugin has not
been loaded by `Synthesia.exe` itself, so the proxy load path, the timing of hook installation
against Synthesia's startup, and the interaction with Synthesia's own keepalive are reasoned
from the disassembly rather than observed.

**The UWP backend.** The `win10-midi.dll` detour is written against the signature confirmed from
that DLL's own prologue, but it has never been exercised, since the backend is off by default
and the loopback test uses WinMM. The lazy attach, which waits for the module to appear, is
likewise untested.

**Long sessions.** No soak test. Nothing suggests drift, since the turn clock restarts its
deadline from the current time rather than accumulating, but this has not been run for hours.

## Known limits

- Only the Casio scheme, `lightChannel -4`, is recognised. The Vangoa scheme at `-5` passes
  through untouched, so those keyboards behave exactly as they do without the plugin.
- Four lamps and a 50 ms turn are constants, deliberately. Changing them is a recompile.
- If Synthesia is sending lights through some third route not visible in the disassembly, the
  plugin would simply do nothing and Synthesia would behave as it always has.
