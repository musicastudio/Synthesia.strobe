# Design

## The problem

The Casio LK-S250 lights at most four keys at once. Synthesia happily asks for more, and the
keyboard drops everything past the fourth, so in any passage thicker than a four-note chord some
notes simply never light. Which four survive is an accident of arrival order, not musical
sense.

## The fix

Never ask for more than four. Take the set of keys Synthesia wants lit, sort it by pitch, split
it into adjacent blocks of up to four, and show one block per turn at 50 ms a turn. Every
requested key then blinks instead of the extras vanishing.

Ten keys become three turns of 4, 3 and 3, so the cycle is 150 ms and each key blinks about 6.7
times a second. Four keys or fewer is one block, nothing rotates, and the lights simply stay on,
so ordinary playing is untouched.

The block sizes never differ by more than one, and blocks are adjacent in pitch rather than
scattered, so a turn reads as a chunk of the chord rather than a random handful.

### Why 50 ms

The keyboard drops a light shown for much less than about 45 ms, so the setting is the length of
a *turn*, not a per-key rate. With a fixed turn the per-key rate takes care of itself, falling as
more keys share the cycle, which is the behaviour you want: a key blinks `1000 / (50 x turns)`
times a second. A per-key rate would instead have to be turned down by hand as the music
thickened.

## Where it attaches

Synthesia drives the LK-S250 with Casio SysEx, `F0 44 7E 7E 7F 02 00 <note> <on> F7`. Those
bytes can leave the process by exactly two routes, both single exported functions.

| Hook | Module | When it is used |
|---|---|---|
| `midiOutLongMsg` | `winmm.dll` | the classic backend, which is the default |
| `winrt_midi_out_port_send` | `win10-midi.dll` | the Windows 10 UWP backend, off by default |

The plugin detours both and matches the 10-byte Casio pattern. Anything else, including
Synthesia's own keepalive and all ordinary music, passes straight through untouched.

A light message is swallowed rather than forwarded, because the strobe re-emits a rotated subset
in its place. The remaining detail is that the strobe's own sends go out through the saved
original function, never the detour, so they cannot be intercepted again.

This boundary was chosen over hooking Synthesia's internal light logic, which
[docs/hooks.md](hooks.md) locates at `0x14011f850`. That function is the generic MIDI send for
the whole program and decides from device state whether a note becomes a light, so detouring it
would mean reproducing that decision from outside and pinning the plugin to one build. Matching
the finished SysEx needs no Synthesia addresses at all, which is why the plugin survives a
Synthesia update that the address-based approach would not.

### Which backend answers

The plugin does not detect or configure a backend. It records which one the intercepted light
message arrived on, along with the port handle, and answers on that same one. So whichever way
Synthesia is set up, the strobe replies down the route already proven to work.

## How it loads

Synthesia imports `version.dll`, and Windows searches the application directory before System32.
A `version.dll` in Synthesia's own folder therefore loads in place of the system one, which makes
the install a single file with nothing to register and nothing to run.

Synthesia is the only module in the process that imports `version.dll`, and it imports exactly
three entry points, so the proxy forwards those three to the real DLL loaded by full path from
System32. Loading by full path is what stops the proxy from finding itself.

`version.dll` is not in the KnownDLLs list, which is what makes this work at all; a KnownDLL
would be resolved from the system copy regardless of what sits next to the executable.

## Threading

Three threads matter.

- **Synthesia's MIDI thread** runs the detours. It takes the lock only to hand a note to the
  strobe, and non-light traffic never takes it at all.
- **The turn clock** advances the rotation every 50 ms, paced by a high-resolution waitable
  timer where the OS has one and plain sleeping otherwise. It costs no measurable CPU and it is
  where the strobe's own sends happen.
- **The loader thread** does nothing but spawn the setup thread, since installing hooks from
  `DllMain` under the loader lock is not safe.

One mutex guards the strobe state, the recorded backend, and the outgoing sends. Critical
sections are a few MIDI writes long; a 10-byte SysEx is on the wire in well under a millisecond.

## Keepalive

The keyboard closes its light display roughly 180 ms after the last keepalive and then ignores
light messages entirely. Synthesia sends its own while it plays, and those pass through, so the
common case needs nothing.

The gap the plugin has to cover is keys held lit while Synthesia has stopped sending, where the
display would close mid-strobe. So if no keepalive has passed through in 150 ms and any key is
lit, the turn clock sends one.

## What was deliberately left out

- **No settings.** Blocks at 50 ms over four lamps is the whole behaviour, and the three numbers
  are constants at the top of one file. A settings file earns its place when someone wants a
  second pattern, not before.
- **No UI.** The plugin has no window, no tray icon and no configuration. It is meant to be
  invisible.
- **No other strobe patterns.** Blocks is the brightest option and the one asked for.
- **No device detection.** The plugin never enumerates MIDI devices or cares which keyboard is
  attached. It reacts to Casio light bytes, so it is active exactly when Synthesia is driving a
  Casio-scheme device and dormant otherwise.
