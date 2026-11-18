# OTTmpc 2.0: plan and status

From tag `ottmpc-vst-v1.0.1` (6412c19). Catalog id: `ottmpc`.

## Frozen identity (from the 1.0.1 build)
- Name `OTTmpc`, vendor `sd88me`, uid `vOTx` (0x764f5478), effect, 1.x file `ottmpc.so`.
- Parameters: the 18 keys of `vst/params.lock`, in that order. `gen_vst.py vst.json --params-h` at the release's tools
  commit (mpc-vst-plugins 8c7be1c) gives exactly 18: no hidden popup flags. New parameters are appended after `l_out`.
- PRESET menu: the 10 factory presets of `vst/presets.json`, programs 0..9. They keep those numbers; favourite slots follow.

## What a 1.0.1 project holds
Nothing of OTTmpc's settings. The wrapper sets `effFlagsProgramChunks` and answers `effGetChunk` with the engine's
`state` key, which this engine never had, so the chunk is 0 bytes (`tests/check_legacy.sh` builds the tag and shows it).
The only thing that comes back is the program the host re-picks after a load (when it re-picks it): 1.0.1 then applied
that factory preset, because a fresh instance starts at program 0.

This is the reported "parameters are not saved when I close the project".

2.x therefore:
- saves every parameter (the plugin framework's chunk), plus the favourites' program number;
- on an empty or missing chunk (a 1.0.1 project) keeps the defaults and honours the host's program re-pick, so the
  1.0.1 project comes back with the factory preset it had, as close as 1.0.1 ever got;
- after a real 2.x state, ignores the program re-pick for ~2 s, so saved edits win.

## Front end (phase 3)
- `src/OTTPlugin.cpp` (mpc::Plugin), `src/engine.cpp` unchanged for 1.x (bit-identical int16 output, checked against the 1.0.1
  engine), plus `sample_rate`, `ott_process_float` (unclipped float audio) and `ott_meters` for 2.x. The engine runs only on
  the audio thread; no locks.
- PRESET menu: 10 factory presets, then 32 user presets `U01 ..`. SAVE PRESET / DELETE PRESET and the naming keyboard are
  appended parameters (64 in all, `params.lock`). User presets: `<plugin folder>/ottmpc-presets.txt` (plain text).
- `./build.sh test`: native build (arm32 glibc 2.27, arm64), `tests/run.sh` (ASan/UBSan: identity, audio, state, re-pick, 1.0.1
  project, user presets, parameter 0) and host_sim (18 passed, 0 failures; warnings: output above 0 dBFS, expected).
- `./build.sh beta`: "OTTmpc 2 Beta", uid `vOTB`.

## Status log
- 2026-11-18: phase 1 (repo survey) and phase 2 (identity freeze, state finding) done.
- 2026-11-18: phase 3 (front end, offline tests) done.
- 2026-11-18: phase 4: native skin (tools/make_tui.py, previews in docs/skin-preview/); not yet seen on a device.
- 2026-11-18: Live III beta (OTTmpc 2 Beta): skin, SAVE PRESET (file written as expected), native Meter. The skin's Hold/ held
  the maximum for good and the Meter draws its images unscaled: own h/m/l_hold parameters, images at bar size. Owner: looks good.
- 2026-11-19: release zips (tools/package_catalog.sh, scrubbed, catalog_check OK); README and release notes. Not tagged or
  published.
