# microQ first audio implementation plan

**Goal:** Build and install a playable API v2 microQ module and measure boot and sustained audio on Move.

**Architecture:** Follow the Osirus module's forked emulator and shared audio/MIDI ring design. Construct mqLib::Device only in the child, return immediately from create_instance, and render silence until boot completes. microQ already runs at 44100 Hz. Use acquire/release atomic queue publication and bounded parent callbacks. Keep the initial UI to host-provided chain parameters until the boot/performance gate passes.

**Tech stack:** C++17, mqLib/DSP56300 JIT, Linux mmap/fork, CMake/Ninja, ARM64 Docker cross compiler, SSH.

1. Add a standalone API test harness in tests/module_smoke.cpp. Check missing-library failure first, then asynchronous create, missing-ROM error, silence while loading, bounded render time, boot completion, MIDI-generated audio, and teardown on Move.
2. Add independently testable shared ring and ROM normalization helpers in src/dsp/runtime.h; tests/runtime_test.cpp covers wraparound, saturation, ordered MIDI messages, and swapped ROM normalization without modifying source files.
3. Implement src/dsp/vavra_plugin.cpp with child lifecycle, timeout/error reporting, native-rate audio, MIDI and DSP clock/gain parameters. Avoid advertising unsupported state/preset functionality.
4. Fix scripts/build.sh and scripts/install.sh to build the actual dsp target and package only existing module files. Add harness build targets to CMakeLists.txt.
5. Cross-build, install atomically, stage the user's ROM outside release artifacts, and run the harness on Move. Measure boot, callback timing, peak/RMS, underruns, and child CPU time at useful clock settings.
6. Once first audio is established, add state/preset integration and minimal host UI as justified by the measurements. Update CLAUDE.md and README.md with observed results and remaining limits.

Work stays in the user's requested checkout. No commit or remote creation is needed for device testing.
