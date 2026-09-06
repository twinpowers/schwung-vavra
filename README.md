# schwung-vavra

Waldorf microQ for Schwung/Move, on gearmulator's `mqLib`.

**Status: scaffolding.** The build wiring, the gearmulator pin and the module
metadata are real; `src/dsp/vavra_plugin.cpp` is not written yet. Nothing here
has run on a device.

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

## Two things that will bite

**That boot cost is a real risk to this port.** ~16 CPU-s against the Virus A's
3.0. On Move that is module-load latency the user feels, and it should be
measured against the host's load timeouts before much more is built.

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

1. Write `src/dsp/vavra_plugin.cpp`. The Osirus plugin
   (`schwung-virus/src/dsp/virus_plugin.cpp`) is the model to copy, not the
   JP-8000 one -- it is the other DSP56300 device, it already solves the fork,
   the per-model DSP clock and the shm audio ring, and its ring is free of the
   two defects found in `jePipeline`.
2. Measure boot on device before building any UI.
3. Decide a default DSP clock percent the way Osirus does per model.
