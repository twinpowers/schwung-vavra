#!/usr/bin/env bash
# Does the module actually output the right audio, in Single and in Multi?
#
# Captures what render_block hands the host, for a set of presets and part
# layouts, alongside an OFFLINE render of the same engine driven directly (no
# fork, no ring, no gain). tools/battery_check.py then requires every capture
# to match its OWN reference and beat every other by 2x.
#
# Spectral, not sample-exact, and that is not a shortcut: mqLib runs the 68k
# and the DSP in separate threads, so the emulator is not reproducible even
# against itself. Two renders of one preset differ by 1.4-5.0 on this metric.
#
# Runs as ROOT on the device: unprivileged, the harness is SCHED_OTHER against
# the emulator's FIFO 20 threads, and it drains the queue itself.
set -euo pipefail
HOST=${VAVRA_HOST:-root@move.local}
DEVICE_DIR=${VAVRA_DEVICE_DIR:-/data/UserData/vavra-tests}
MODULE=${VAVRA_MODULE_DIR:-/data/UserData/schwung/modules/sound_generators/vavra}
LOCAL=${VAVRA_BATTERY_DIR:-battery}
PRESETS="0 16 100 200 299"

echo "=== capturing on $HOST (about 6 minutes: every run boots the firmware) ==="
ssh "$HOST" "mkdir -p $DEVICE_DIR/battery && cd $DEVICE_DIR && \
  for p in $PRESETS; do \
    taskset 0x7 ./dump_presets $MODULE/roms/*.[bB][iI][nN] render 50 \$p battery/R\$p.wav 2 6 >/dev/null 2>&1; \
    VAVRA_HOLD=1 VAVRA_MODE=single VAVRA_PRESET=\$p VAVRA_PLAY_CH=1 \
      VAVRA_WAV=battery/single_\$p.wav ./module_smoke $MODULE - 2 >/dev/null 2>&1; \
  done; \
  run() { VAVRA_HOLD=1 VAVRA_MODE=multi VAVRA_PART1=\$1 VAVRA_PART2=\$2 VAVRA_PART2_CH=\$3 \
            VAVRA_PART2_VOL=\$4 VAVRA_PLAY_CH=\$5 VAVRA_WAV=battery/\$6.wav \
            ./module_smoke $MODULE - 2 >/dev/null 2>&1; }; \
  run 0 100 2 127 1 multi_p1; run 0 100 2 127 2 multi_p2; \
  run 200 299 2 127 1 multi_p3; run 200 299 2 127 2 multi_p4; \
  run 0 100 2 0   2 multi_muted; run 0 100 1 127 1 multi_layered"

echo "=== fetching ==="
mkdir -p "$LOCAL"
for f in $(ssh "$HOST" "ls $DEVICE_DIR/battery"); do
    scp -q "${HOST%%@*}@${HOST##*@}:$DEVICE_DIR/battery/$f" "$LOCAL/" 2>/dev/null || \
    scp -q "$HOST:$DEVICE_DIR/battery/$f" "$LOCAL/"
done

VAVRA_BATTERY_DIR="$LOCAL" python3 "$(dirname "$0")/../tools/battery_check.py"
