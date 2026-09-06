# schwung-vavra

Waldorf microQ for Schwung/Move, on gearmulator's `mqLib`.

**Status: first audio, measured on Move.** The module builds, loads, boots the
firmware and plays. Four voices at the default 75% DSP clock ran 10 s with zero
underruns and a worst-case `render_block` of 26 us. It has not yet been played
through the Schwung chain UI on hardware, and it has no preset browser, no state
save/restore and no editable patch parameters -- see [Next](#next).

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
| 75% (default) | 0, 0, 0 | 0, 5, 10 |
| 50% | 0, 0, 0 | 0, 0, 0 |

100% cannot sustain eight voices on Move -- 4% of blocks drop -- so the default
is 75%: clean at four voices, and 0.3% at eight. 50% was clean in every run.
The microQ's polyphony is DSP-bound, so this dial buys voices rather than
quality, which is why it exists and why Osirus does not default to 100 either.
**Anything measured at eight voices needs repeats**: MoveOriginal's own load
swings between 50% and 60% of a core and moves this cell with it. Boot is **12.6-20.6 s** wall, during which
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
2. Preset browsing and state save/restore. `mqLib::Device` implements
   `getState`/`setState` and the ROM carries banks; none of it is wired up, so
   the slot currently has no patch to persist. Osirus
   (`schwung-virus/src/dsp/virus_plugin.cpp`) is the model.
3. Editable patch parameters and a Remote UI. Today the module publishes three
   knobs: gain, DSP clock and buffer.
