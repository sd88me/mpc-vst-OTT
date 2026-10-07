#!/usr/bin/env bash
# Build the plugin with mpc-vst-plugins' tools: MPC_VST, else a checkout next to this repo.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MPC_VST="${MPC_VST:-$HERE/../../mpc-vst-plugins}"
exec "$MPC_VST/tools/build_port.sh" "$HERE/vst.json"
