# schwung-vavra

Waldorf microQ for Schwung/Move, on gearmulator's `mqLib`.

**Status: scaffolding only.** `CMakeLists.txt`, `scripts/`, `src/module.json` and
the `libs/gearmulator` pin are real and correct. **`src/dsp/vavra_plugin.cpp`
does not exist**, so nothing here builds into a module yet. Nothing has run on a
device.

## Copy the Osirus plugin, not the JP-8000 one

`schwung-virus/src/dsp/virus_plugin.cpp` is the model. It is the other DSP56300
device, and it already solves everything this port needs:

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

Note this repo has **no git remote yet** -- create one before relying on it.
