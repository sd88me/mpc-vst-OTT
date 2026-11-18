#!/usr/bin/env bash
# The catalog-shaped release zips, from the release build (build/"sd88me - VST - OTTmpc", made by ./build.sh):
#   dist/OTTmpc-<ver>-mpc-armv7.zip, dist/OTTmpc-<ver>-mpc-aarch64.zip   one per CPU, made by mpc-vst-plugins' tools/release.py
# As 1.0.x, the .so is ottmpc.so at the folder root, so the installer upgrades a 1.0.x install in place; ottmpc-presets.txt (the
# user presets) is user data, kept on upgrade and uninstall. MPC_VST: an mpc-vst-plugins checkout with tools/release.py.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
MPC_VST="${MPC_VST:-$HOME/mpc-vst-plugins-postlaunch}"
[ -f "$MPC_VST/tools/release.py" ] || { echo "need mpc-vst-plugins with tools/release.py (MPC_VST)" >&2; exit 1; }
DIR="sd88me - VST - OTTmpc"; SRC="$here/build/$DIR"
[ -f "$SRC/VST/ottmpc-arm32.so" ] || { echo "missing $SRC: run ./build.sh first" >&2; exit 1; }
VER=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$SRC/plugin.json")
python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); sys.exit(0 if m["uid"]=="vOTx" and "Beta" not in m["name"] else "not the release identity")' "$SRC/plugin.json"
out="$here/dist"; mkdir -p "$out"
for pair in armv7:arm32:file32 aarch64:arm64:file64; do
  IFS=: read -r cpu so key <<<"$pair"
  st="$here/build/catalog/$cpu"; rm -rf "$st"; mkdir -p "$st/$DIR"
  cp -r "$SRC/Plugin Skins" "$SRC/version.xml" "$st/$DIR/"
  python3 - "$SRC/plugin.json" "$st/$DIR/plugin.json" "$key" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
m.pop("file32", None); m.pop("file64", None)
m[sys.argv[3]] = "ottmpc.so"
json.dump(m, open(sys.argv[2], "w"), indent=2)
PY
  cp "$SRC/VST/ottmpc-$so.so" "$st/ottmpc.so"
  printf '<PLUGIN name="OTTmpc" descriptiveName="OTTmpc" format="VST" category="Effect" manufacturer="sd88me" version="%s" file="ottmpc.so" uid="764f5478" isInstrument="0" fileTime="0" infoUpdateTime="0" numInputs="2" numOutputs="2" isShell="0"/>
' "$VER" > "$st/entry.xml"
  python3 "$MPC_VST/tools/release.py" --so "$st/ottmpc.so" --skin "$st/$DIR" --entry "$st/entry.xml" --version "$VER" \
    --user-data ottmpc-presets.txt \
    --repo sd88me/mpc-vst-OTT --license GPL-3.0-only --id ottmpc \
    --about "Vital's OTT multiband up/down compressor as an insert effect: smooth band meters with peak hold, 12 Q-Link controls, 10 presets and your own in the PRESET menu." \
    -o "$st/dist"
  z=$(ls "$st/dist"/*.zip)
  # Public text: the installer template names an unreleased OS version in one comment; reword it and refresh SHA256SUMS
  python3 - "$z" "$out/OTTmpc-$VER-mpc-$cpu.zip" <<'PY'
import hashlib, re, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
zin = zipfile.ZipFile(src)
files = {i.filename: (i, zin.read(i.filename)) for i in zin.infolist()}
for n, (i, data) in list(files.items()):
    if n.endswith("/install.sh"):
        t = data.decode()
        t = re.sub(r"# MPC OS [0-9.]+\+ loads plugins \(skins included\) from <drive>/VST2 Plugins, which is not a content location: no warning there\.",
                   "# Plugins (skins included) loaded from <drive>/VST2 Plugins are not in a content location: no warning there.", t)
        assert "+ loads plugins (skins included)" not in t, "install.sh comment not reworded"
        files[n] = (i, t.encode())
sums = [n for n in files if n.endswith("/SHA256SUMS")]
for sn in sums:
    root = sn[: -len("SHA256SUMS")]
    old = files[sn][1].decode().splitlines()
    lines = []
    for l in old:
        h, _, rel = l.partition("  ")
        rel = rel.strip()
        if root + rel in files:
            h = hashlib.sha256(files[root + rel][1]).hexdigest()
        lines.append("%s  %s" % (h, rel))
    files[sn] = (files[sn][0], ("\n".join(lines) + "\n").encode())
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zo:
    for n, (i, data) in files.items():
        zo.writestr(i, data)
PY
done
python3 "$here/tools/scrub_zip.py" "$out"/OTTmpc-"$VER"-mpc-*.zip
ls -la "$out"/OTTmpc-"$VER"-mpc-*.zip
