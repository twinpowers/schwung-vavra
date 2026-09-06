# schwung-vavra

Waldorf microQ for Schwung/Move, on gearmulator's `mqLib`.

**Status: first audio, measured on Move.** It builds, boots the firmware in a
forked child and plays: four voices at the default 75% DSP clock, 10 s, zero
underruns. Not yet driven by the Schwung chain host on hardware, and it has no
presets, no state save/restore and no patch parameters. Numbers and the next
steps are in `README.md`; the plan is `docs/plans/2026-09-06-microq-first-audio.md`.

## The harness measures ITSELF unless it runs as root

This cost most of a day. `tests/module_smoke.cpp` pins to core 3 at FIFO 70 --
what Schwung's real SPI callback is -- but only when `geteuid()==0`. Run as
`ableton` it is SCHED_OTHER on cores 0-2 against the emulator's own FIFO 20
threads, so it gets descheduled and then **bursts** its catch-up `render_block`
calls, draining the queue faster than real time. A late consumer is a BURST, not
a gap -- the same shape Schwung's own SPI notes warn about.

It reported 13-65 underruns a run and, worse, it reported them *plausibly*:
they clustered in the first second after the first note, which is exactly where
cold-JIT dropouts would sit. Three separate fixes were built and measured
against that ghost -- 64-sample emulator steps (worse: 251 underruns, more CPU),
a child at SCHED_OTHER, and a queue up to 100 ms deep (still 5) -- and a 2x2
matrix of the last two came back pure noise, 0-52 *within* one cell. That
uniform noise was the tell. Run as root, the same builds report 0-2, and the
emulator itself gets faster (proc_ms 7950 vs 9100) because the harness had been
stealing its cores.

**Always run it as root, and distrust any cell whose repeats disagree.**

## Two real findings that survived

**The JIT warmup must run until the JIT goes QUIET, not for N blocks.** A cold
path compiles for up to 86 ms against a queue holding 17 ms. A fixed warmup
cannot cover it: what must compile is not "a note" but note-on, voice
re-allocation and note-off arriving as separate events over seconds. The child
plays a retriggering chord and exits when no step has exceeded 8 ms for two
seconds. Do not shorten this into a constant.

**100% DSP clock cannot sustain eight voices** -- 137-159 underruns a run, 4% of
blocks, against 0-10 at 75% and 0 at 50%. The default is 75: clean at four
voices. Polyphony on a microQ is DSP-bound, so this dial buys voices, not
quality -- the same trade Osirus makes per model. **Repeat any eight-voice
measurement three times**; MoveOriginal's own load swings 50-60% of a core and
the first two-rep reading of this cell came back 0-1, which is not what it
does.

## Copy the Osirus plugin, not the JP-8000 one

`schwung-virus/src/dsp/virus_plugin.cpp` is still the model for what is NOT
built yet (presets, banks, state, parameters). It is the other DSP56300 device,
and it already solves:

- the fork into a child process, with a shm audio ring
- per-model DSP clock scaling (`setDspClockPercent`)
- MIDI FIFO, state save/restore, preset/bank browsing

Its ring is also already free of the two defects found in `jePipeline` (a
producer with no space check; blocking waits reachable from the audio thread) --
audited 2026-09-06, both absent. The JP-8000 module is a worse model: different
chip, different engine, and its pipeline machinery has no counterpart here.

## Why the microQ, and only the microQ

Measured on a Pi 4B @ 1.8 GHz (A72, like Move's CM4), JIT, four voices,
CPU-seconds per second of audio, boot excluded via a 2 s/10 s slope:

| synth | DSPs | CPU-s / audio-s | vs Virus A |
|---|---|---|---|
| Virus A (Osirus, ships on Move) | 1 | 0.580 | 1.00x |
| **microQ (this)** | **1** | **0.813** | **1.40x** |
| Microwave II | 1 | 0.944 | 1.63x |
| Microwave II + voice expansion | 3 | 4.111 | 7.09x |
| Virus TI2 (OsTIrus) | 2 | 2.435 | 4.20x |
| Nord Lead 2x | 2 | 2.763 | 4.76x |

One DSP, about one core on Move, and it answers `canModifyDspClock()` so it has
the same polyphony-for-CPU dial Osirus uses. Everything below it wants two cores
or more, and Move does not have them: Ableton pins itself to cores 0-2.

**Rank by CPU-seconds, never by real-time factor.** By wall-clock the microQ
(1.88x) looks competitive with the Virus A (2.22x); it gets there by spending
nearly two cores. `dsp56kBench` in the gearmulator tree is the tool
(`dsp56kBench_mq mq "" <secs> <voices> <clock%> <repeats>`); it takes best-of-N
and prints peak/rms with an AUDIO/SILENT verdict, both of which exist because a
single timing and a real-time factor each produced a confidently wrong table.

## Two things that will bite

**Boot costs ~16 CPU-seconds** against the Virus A's 3.0. On Move that is
module-load latency the user feels. Measure it on device against the host's load
timeouts BEFORE building a UI -- it is the most likely reason this port fails.

**ROM dumps are commonly byte-swapped.** `mqLib::ROM::verifyRom()` requires the
image to start with ASCII `2.23`; the widely circulated `microQ223.BIN` starts
`2e 32 33 32` -- the same bytes swapped within each 16-bit word. Swap pairwise
and it boots. The loader only ever says "no ROM found".

A working image is at `~/Documents/_Songs/Move ROMs/` if one was left there, or
swap a fresh dump with:
`d=bytearray(open(f,'rb').read()); d[0::2],d[1::2]=d[1::2],d[0::2]`

## Build

`scripts/build.sh` cross-compiles ARM64 in Docker (image `schwung-vavra-builder`,
built from `scripts/Dockerfile` on first run); `scripts/install.sh` deploys to
`move.local`. `libs/gearmulator` is a submodule pinned to `d7c692c1`, the commit
every measurement above was taken with.

`tests/runtime_test` (ring + ROM normalization, no device needed) and
`tests/module_smoke` (the real ABI, on device) are built by the same script and
both are ARM64 -- they run on Move, not on the Mac.

Note this repo has **no git remote yet** -- create one before relying on it.
