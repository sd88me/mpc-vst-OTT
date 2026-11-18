# OTTmpc 2.0.0

A rebuilt OTTmpc with its own native skin, your own presets and a fix for settings lost with a project.

## Fixed
- **Settings are now saved with the project.** OTTmpc 1.0 saved nothing of its own in a project, so closing and reopening a
  project brought back the defaults (or the factory preset that was picked). 2.0 saves every control and the picked preset.

## New
- **Your own presets in the PRESET menu**: SAVE PRESET names the current settings on an on-screen keyboard and stores them as
  `U01 <name>` (32 slots, shared by every project); DELETE PRESET removes the picked one.
- **Smooth band meters with peak hold** in place of the stepped bars.
- Runs at the project's sample rate; its output is no longer clipped at 0 dBFS inside the plugin.

## Changed
- The skin keeps the 1.0 look. Q-Links are now three blocks of four: Depth / Time / In / Out, High / Mid / Low / Mix, and
  Upward / Downward / Low-Mid / Mid-High, so **Mix moved** next to the band gains.

## Install
- `OTTmpc-2.0.0-mpc-aarch64.zip`: 64-bit MPCs (Live III and others).
- `OTTmpc-2.0.0-mpc-armv7.zip`: the Force and the 32-bit MPCs.
- `OTTmpc-2.0.0-mpc-sdk.zip`: one plugin folder for every MPC (MPC OS 3.10.1 and later), to copy into `VST2 Plugins` by hand.

Install through the Plugin Manager or run the zip's `install.sh` as root (it stops MPC: save first). It upgrades OTTmpc 1.0 in
place. Projects, automation and Q-Link assignments from 1.0 load unchanged; a 1.0 project opens with its factory preset, since
1.0 never saved more.

**Checked:** on an MPC Live III (skin, meters, user presets).
