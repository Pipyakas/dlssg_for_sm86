# ACE COMBAT 8 Dynamic MFG verification history

## Installed

- Official Streamline SDK 2.14.1 x64 production binaries: `sl.interposer`, `sl.common`, `sl.dlss_g`, `sl.reflex`, `sl.pcl`, `sl.deepdvc`, `nvngx_dlssg`, and `nvngx_deepdvc` in the game's Streamline ThirdParty directory.
- The existing `dlssg_sm86` proxy remains installed and selects its bundled 310.9.1.0 runtime/backend.
- The launcher retains the seven-call-site DLSS preset correction and startup refresh-to-INI selection.
- `streamline_dynamic_probe.enable` opts into loading `streamline_dynamic_probe.dll`.
- `streamline_dynamic_probe.ini`, `[Probe] EnableDynamic=1`, enables the Dynamic override after the runtime reports support. Changes take effect on the next game launch.

## Behavior

The adapter hooks Streamline's feature-function resolution, then intercepts the game's existing GetState/SetOptions calls on their original threads. It upgrades the old version-3 state output to version 4 internally, copies only the old allocation's body back, and keeps the game's original frame-counter query behavior.

When Dynamic MFG is supported, game requests for On or legacy Auto are converted to `eDynamic` with a version-5 options object and `dynamicTargetFrameRate=0` (monitor auto-detection). Off requests remain Off. A rejected Dynamic request disables the override for that process and retries the original options.

Dynamic MFG ignores `numFramesToGenerate`. The startup INI mapping is still useful when the override is disabled: below 75 Hz = 2x, 75–99 Hz = 3x, at least 100 Hz = 4x. Unknown refresh leaves the setting untouched. Startup detection uses the primary display; Dynamic MFG uses the display containing the game window.

The mod's existing `MaxGeneratedFrames=3` caps the runtime at 4x. Dynamic MFG chooses among available counts; 60 Hz does not necessarily mean exactly 2x if the base render rate needs more generated frames to reach the target.

## Verified on 2026-10-02

- ABI/canary tests passed: old state allocation not overrun; options input preserved; Off and unsupported modes pass through; rejected Dynamic falls back.
- Refresh-to-INI tests passed for 13 boundary/rounded refresh rates.
- Passive run PID 4428: Dynamic supported, maximum generated frames 3, status 0; fixed-mode generation worked.
- Dynamic run PID 13576: mode 3 accepted with result 0, support true and status 0. Backend evaluate samples varied among generated counts 1, 2, and 3 (2x/3x/4x), with successful results and zero failed kernel launches in those samples.
- A rendered flight scene was inspected; the overlay showed approximately 117 FPS on the 120 Hz display. This is a short functional test, not a long-session stability certification.
- The real 60 Hz virtual-monitor streaming transition has not been tested yet.

## Logs and rollback

- Adapter: `streamline_dynamic_probe.log` beside the launcher.
- Mod: `Game\Binaries\Win64\dlssg_sm86\logs\backend_<PID>.jsonl` and `loader_<PID>.jsonl`.
- Original eight DLLs and launcher source/binary: `streamline-backup-20261002-132824` (SHA-256 manifest included).
- To return to fixed FG without downgrading DLLs: exit the game, set `[Probe] EnableDynamic=0`, and relaunch.
- To restore all pre-experiment files: exit the game, then run `powershell -ExecutionPolicy Bypass -File "D:\steamapps\common\ACE COMBAT 8\Restore-Streamline.ps1"`.

SDK source/release: https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1

The hook implementation uses MinHook v1.3.4 (commit c3fcafdc10146beb5919319d0683e44e3c30d537).

## Project migration and consolidated configuration

The details above record the original experiment's filenames. The maintained project is now `C:\code\ac8-sl-mfg`; sources and working files are no longer stored in the game directory or the temporary work directory.

The shipping adapter and control file are now `streamline_dynamic.dll` and `streamline_dynamic.ini`. `[Launcher] EnableAdapter` replaces the marker file, `[FrameGeneration] EnableDynamic` replaces `[Probe] EnableDynamic`, and the adapter writes `streamline_dynamic.log` at runtime. The old binaries/config/log are archived under the project's `backups/` directory.

All raw CVars can be managed through `[ConsoleVariables]` in the consolidated INI. The launcher syncs these to Engine.ini, and forwards `SM86.*` sections to the mod INI. The primary-display fallback thresholds, generated-frame counts, Dynamic target FPS, launcher switches, hook timing, and adapter logging controls are also configurable there. Both updated test suites pass, including Unicode destination preservation and explicit `@remove` deletion.

Use `tools/Build.cmd`, `tools/Deploy.ps1`, and `tools/Restore-Streamline.ps1` from the project. See the project README for current usage.

Post-migration live smoke test (PID 11632): the renamed DLL loaded the consolidated INI with Dynamic enabled and automatic target FPS. Engine.ini and the mod INI synchronized successfully; mod and adapter logs went to the project's `logs/` directory. The runtime accepted mode 3, and 76 sampled evaluate records included generated counts 1, 2, and 3, all successful, with zero failed kernel launches reported (highest evaluate sequence 7680). A read-only live memory check confirmed the correct cave and all seven preset-call redirects. The smoke-test game was closed cleanly afterward.

The game root contains only the shipping `start_protected_game.exe`, `streamline_dynamic.dll`, and `streamline_dynamic.ini` added by this project, alongside the existing game files.

## Shared Mode-2 extension (current)

The shipping set now also includes `sm86_refresh.dll`, built from
`C:\code\dlssg_for_sm86\companion`. Mode 1 loads the old NVIDIA Dynamic helper;
Mode 2 loads this new shared refresh-based fixed-MFG DLL; Mode 3 loads neither.
Both share the existing INI. Current default is Mode 2.

New tests pass: 60/61 and 90/91 boundaries, historic options v1-v5 against guard
pages, older state-output canaries, Off passthrough, capability clamp and rejection
fallback, all six system proxy export sets, and real Windows API forwarding calls.

Live AC8 validation could not start: the Artifact service tries to load
`Game\Binaries\Win64\Artifact_AMD\SimpleSvm.sys`, which Windows rejects.
SCM events 7045 and 7000 and Code Integrity event 3004 confirm the same failure on
both Mode 2 and the no-injection Mode-3 baseline. VBS and HVCI are currently running.
No OS security/boot settings were changed. New gameplay verification is pending;
earlier official Dynamic runs are not evidence for the new Mode-2 DLL.
