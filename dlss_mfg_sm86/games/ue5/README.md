# UE5 games

Unreal Engine 5 games integrate DLSS-G through NVIDIA's Streamline plugin, so they
share one layout and one set of steps.

**Where files go.** The rendering EXE is `<Project>\Binaries\Win64\<Project>-Win64-Shipping.exe`
(or a renamed equivalent); dlssg_sm86 (`version.dll` + `dlssg_sm86.ini`), the
dlss_mfg_sm86 proxy and `dlss_mfg_sm86.ini` all go in that `Binaries\Win64` folder.
Pick a proxy name the EXE imports and no other mod uses there.

**Detection.** The core DLL activates in any process that loads Streamline. It
recognises Unreal by `r.DynamicRes.OperationMode` in the EXE (UE4.22+ / UE5) and
then enables the console-variable features (Mode 3 dynamic resolution,
`[ConsoleVariables]`). The check reads every initialized data section regardless
of its name, after Streamline has loaded, so protected executables with renamed
sections are recognised too. Most UE5 games need no folder here: the generic
install in the [project README](../../README.md#install-any-streamline-game) is
enough.

**Streamline runtime.** The plugin's runtime is in
`<...>\Plugins\...\Streamline*\Binaries\ThirdParty\Win64` (`sl.interposer.dll`,
`sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `sl.deepdvc.dll`,
`nvngx_dlssg.dll`, `nvngx_deepdvc.dll`). Check `sl.interposer.dll`'s version:

| Version | Mode 2 (fixed 2x/3x/4x) | Mode 1 (Dynamic) |
|---|---|---|
| below 2.7 | needs an upgrade — no multi-frame code | needs an upgrade |
| 2.7 – 2.13 | works as shipped | needs an upgrade |
| 2.14.1 | works | works (verified) |

To upgrade, replace those eight files with the ones from the Streamline SDK 2.14.1
release (`bin\x64`). The game's own copies are the only rollback, so back them up.

**FG switch.** Games with an in-game FG toggle keep using it. Games without one need
a bootstrap that turns FG on through Engine.ini (see ACE COMBAT 8).

## Games

- [ACE COMBAT 8](ace-combat-8/README.md)
- [Ninja Gaiden 2 Black](ninja-gaiden-2-black/README.md)
- [Halo: Campaign Evolved](halo-campaign-evolved/README.md)
To add a game that needs more than the generic install: create `games/ue5/<game>/` with a README (requirements, install table,
quirks) and a `mod/` folder mirroring the game's paths, add the game's proxy copy
to `build.cmd`, and list it in the project README.
