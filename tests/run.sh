#!/usr/bin/env bash
# Offline x86 test under ASan/UBSan (no device): the native front end + the real engine and Vital DSP, driven by tests/host_test.cpp.
# Must print OK. OTT_NAME / OTT_UID_HEX: the build's name and uid (beta tests).
set -euo pipefail
cd "$(dirname "$0")/.."
BUILDER="${MPC_BUILDER:?set MPC_BUILDER to the MPC plugin builder checkout}"
python3 tools/gen_params.py >/dev/null
T=build/test; mkdir -p "$T"
UIDDEF=()
[ -n "${OTT_UID_HEX:-}" ] && UIDDEF=(-DOTT_UID="$(echo "$OTT_UID_HEX" | sed -E "s/(..)(..)(..)(..)/0x\1,0x\2,0x\3,0x\4/")")
S="-fsanitize=address,undefined -fno-sanitize-recover=undefined -O1 -g -std=c++17 -pthread"
g++ $S -Wall -Wno-sign-compare -Wno-unused-function -DOTT_TEST -DOTT_EXPECT_NAME="\"${OTT_NAME:-OTTmpc}\"" -DOTT_EXPECT_UID="0x${OTT_UID_HEX:-764f5478}" ${OTT_NAME:+-DOTT_NAME="\"$OTT_NAME\""} "${UIDDEF[@]}" \
  -I"$BUILDER/framework" -Isrc -o "$T/otttest" src/OTTPlugin.cpp src/engine.cpp src/vital_unit.cpp tests/host_test.cpp -lm
pf="$(mktemp -d)"; trap 'rm -rf "$pf"' EXIT
cp plugin.json "$pf/"
MPC_PLUGIN_FOLDER="$pf" ASAN_OPTIONS=detect_leaks=1 "$T/otttest" "$PWD"
