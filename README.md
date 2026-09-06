# schwung-vavra

Waldorf microQ for Schwung/Move, on gearmulator's `mqLib`.

**Status: playable, measured on Move.** The module builds, boots the firmware,
plays, and selects any of the 300 factory sounds by name. Four and eight voices
at the default 75% DSP clock each ran 10 s with zero underruns, worst-case
`render_block` 26 us. No state save/restore and no editable patch parameters
yet -- see [Next](#next).

## Presets

`preset` is one enum of 300 options, `A1 LosAngeles2019 T` .. `C100 Init Sound
2.0`, in Program Change order. Three things about it are not obvious:

- **The names are not in the ROM.** Scanning all 524288 bytes for a 16-char
  name table at any plausible stride finds nothing. The only authority is the
  running firmware, which streams its 2x20 front panel as `SysexCommand::EmuLCD`
  -- so `tools/dump_presets.cpp` boots the device once and reads the names off
  the screen, one Program Change at a time, exactly as a person would.
- **Bank select is CC 32, not CC 0.** CC 0 is accepted and ignored: a full dump
  taken with it came back reading `A001-A100` for all three banks, which looks
  like a correct dump of a machine that only has one bank.
- That dump is committed as `docs/presets-os223.tsv` and compiled in by
  `tools/gen_presets_header.py`, so browsing costs no round trip and nothing
  reads a file on the audio callback. `tests/test_presets_generated.sh`
  compares the dump, the header and `module.json`'s options by content --
  a stale copy shows the wrong name on a preset that still loads, which is
  invisible at runtime. The *current* name (`preset_name`) is still read live
  off the LCD, so a different ROM cannot make the module lie.

**The module selects A1 at boot.** Left alone the firmware comes up on A17
`11KHz Solo`, which is arbitrary and sounds like a kick with reverb -- for a
while that was the only sound this port could make.

## Why the microQ and not one of the others

Measured on a Pi 4B @ 1.8 GHz (an A72, like Move's CM4), JIT, four voices, CPU
seconds per second of audio, boot excluded:

| synth | DSPs | CPU-s / audio-s | vs Virus A |
|---|---|---|---|
| Virus A (Osirus, already ships on Move) | 1 | 0.580 | 1.00x |
| **microQ (this)** | **1** | **0.813** | **1.40x** |
| Microwave II | 1 | 0.944 | 1.63x |
| Microwave II + voice expansion | 3 | 4.111 | 7.09x |
| Virus TI2 (OsTIrus) | 2 | 2.435 | 4.20x |
| Nord Lead 2x | 2 | 2.763 | 4.76x |

The microQ is the only DSP56300 synth in the tree besides the Virus A that fits
on Move: one DSP, roughly one core, and it answers `canModifyDspClock()` so it
has the same polyphony-for-CPU dial Osirus already uses. Everything below it in
that table wants two cores or more, which Move does not have spare -- Ableton
pins itself to cores 0-2.

**Rank by CPU-seconds, not by real-time factor.** By wall-clock RT the microQ
(1.88x) looks competitive with the Virus A (2.22x); it gets there by spending
nearly two cores. And boot must be excluded or it reads four times worse still:
the microQ burns ~16 CPU-seconds booting before it renders a sample.

## Measured on Move

`tests/module_smoke.cpp`, 3446 blocks (10 s) of held-and-retriggered chord per
run, two runs a cell, underruns counted only inside the note window:

| dsp_clock | 4 voices | 8 voices |
|---|---|---|
| 100% | 0, 2 | **137, 141, 142, 159** |
| 75% (default) | 0, 0, 0 | 0, 0, 0 |
| 50% | 0, 0, 0 | 0, 0, 0 |

100% cannot sustain eight voices on Move -- 4% of blocks drop -- so the default
is 75%: clean at four voices, and 0.3% at eight. 50% was clean in every run.
The microQ's polyphony is DSP-bound, so this dial buys voices rather than
quality, which is why it exists and why Osirus does not default to 100 either.
**Anything measured at eight voices needs repeats**: MoveOriginal's own load
swings between 50% and 60% of a core and moves this cell with it. The 75%/8
cell read 0-10 before the queue boost below and 0, 0, 0 after.

**A preset change brings its own cold JIT.** The boot warmup can only compile
the patch that is loaded, so the first notes on a newly selected sound stalled
5-8 blocks -- at every DSP clock, which is what rules out capacity. The child
runs the queue four times deep for 4 s after a change, paid for out of the
~20% steady-state headroom, and latency returns to the user's `buffer_ms`
once the JIT is quiet. Measured 0 underruns on four presets, two runs each. Boot is **12.6-20.6 s** wall, during which
`create_instance` has already returned (in ~0.5 ms) and the module renders
silence. Cost is ~1.3 CPU-seconds per second of audio; worst-case
`render_block` on the host callback is 26-77 us.

**Run the harness as root or it measures itself.** It only pins to core 3 at
FIFO 70 -- what the real SPI callback is -- when it can. Run as `ableton` it is
SCHED_OTHER, competing with the emulator's own FIFO 20 threads on cores 0-2; it
then gets descheduled and *bursts* its catch-up `render_block` calls, draining
the queue faster than real time. That reported 13-65 underruns a run, with a
spread so wide (0-52 within one configuration) that a 2x2 scheduler/buffer
matrix looked like pure noise -- because it was measuring the instrument. The
same builds measured correctly report 0-2.

## Two things that will bite

**Boot is long: 12.6-20.6 s.** ~16 CPU-s against the Virus A's 3.0. The host
never blocks on it -- `create_instance` returns immediately and the child boots
on its own thread -- but it is load latency the user waits through.

**ROM images are commonly byte-swapped.** `mqLib::ROM::verifyRom()` requires the
image to begin with the ASCII `2.23`; the widely circulated `microQ223.BIN`
begins `2e 32 33 32`, i.e. the same bytes swapped within each 16-bit word. Swap
pairwise and it boots. The loader reports only "no ROM found", which is a long
way from "your dump has the wrong endianness".

## Layout

Modelled on `schwung-jp8000`: `scripts/build.sh` cross-compiles for ARM64 in
Docker, `scripts/install.sh` deploys to `move.local`, `libs/gearmulator` is a
submodule pinned to the commit these measurements were taken with.

## Next

1. Play it through the Schwung chain on hardware. Everything above was measured
   by a standalone harness against the same `dsp.so` the host loads; the module
   has never been driven by the chain host itself.
2. State save/restore, so a slot remembers its patch. `mqLib::Device`
   implements `getState`/`setState`; nothing is wired up, so a reload returns
   to A1. Osirus (`schwung-virus/src/dsp/virus_plugin.cpp`) is the model.
3. Editable patch parameters and a Remote UI. Today the module publishes four
   knobs: preset, gain, DSP clock and buffer. The front panel arrives over
   sysex already (`EmuLCD`, `EmuLEDs`, `EmuButtons`), which is a route to a
   real editor.
