#!/usr/bin/env bash
# Build the released OTTmpc (tag + the mpc-vst-plugins wrapper it was built with) for x86 and ask it for its state.
# 1.0.1 answers 0 bytes: its engine has no "state" key, so projects saved with it hold no OTTmpc settings.
set -euo pipefail
TAG="${1:-ottmpc-vst-v1.0.1}"; TOOLS_REF="${TOOLS_REF:-8c7be1c}"
MPC_VST="${MPC_VST:-$HOME/mpc-vst-plugins}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
mkdir -p "$W/ott" "$W/tools"
git -C "$HERE" archive "$TAG" | tar -x -C "$W/ott"
git -C "$MPC_VST" archive "$TOOLS_REF" | tar -x -C "$W/tools"
(cd "$W/ott/vst" && python3 "$W/tools/tools/gen_vst.py" vst.json --params-h)
cd "$W/ott"
g++ -O1 -shared -fPIC -std=gnu++17 -Isrc -Isrc/vital_dsp -Ivst/build -I"$W/tools/wrapper" \
    -DPLUG_EFFECT -DHAS_DISPLAY_REV=1 -DDISPLAY_REV_NO_UPDATE=1 -o "$W/legacy.so" \
    -x c "$W/tools/wrapper/vst2_wrap.c" -x c++ src/engine.cpp src/vital_dsp/compressor.cpp src/vital_dsp/linkwitz_riley_filter.cpp \
    src/vital_dsp/framework/{processor,value,utils,operators}.cpp src/vital_dsp/utilities/smooth_value.cpp -lm -lpthread
gcc -o "$W/probe" "$HERE/tests/legacy_probe.c" -ldl
"$W/probe" "$W/legacy.so"
