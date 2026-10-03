# ACE COMBAT 8

AC8 has no in-game frame generation toggle, its DLSS quality dropdown is off by
one, and its renderer never enables UE dynamic resolution. Its bootstrap fixes all
three; MFG itself comes from the core DLL like any game.

## Install (manual)

Paths are relative to the game folder, e.g. `D:\steamapps\common\ACE COMBAT 8`.
Back up every file you replace first.

| Copy | To | What |
|---|---|---|
| `mod\start_protected_game.exe` | `start_protected_game.exe` | bootstrap — replaces the game's launcher stub |
| `mod\ac8_bootstrap.ini` | `ac8_bootstrap.ini` | bootstrap settings |
| `mod\Game\Binaries\Win64\dinput8.dll` | `Game\Binaries\Win64\` | dlss_mfg_sm86 proxy |
| `mod\Game\Binaries\Win64\dlss_mfg_sm86.ini` | `Game\Binaries\Win64\` | MFG mode |
| `version.dll` from the [fork root](../../../../version.dll) | `Game\Binaries\Win64\` | dlssg_sm86 itself; the bootstrap writes its `dlssg_sm86.ini` from `[SM86.*]` |

Requirements:

- **Streamline.** AC8 ships 2.7.30 in
  `Game\Plugins\Marketplace\nvidia\StreamlineCore\Binaries\ThirdParty\Win64`: enough
  for Mode 2. Mode 1 (Dynamic) needs the 2.14.1 runtime there
  ([UE5 notes](../README.md)).
- **Proxy name.** `dinput8.dll` is a static import of `AceCombat8.exe`. Don't use
  `winmm.dll` (taken by the Artifact loader on this setup) or `version.dll`
  (dlssg_sm86).

## What the bootstrap does

`start_protected_game.exe` reads `ac8_bootstrap.ini`, then starts
`Game\Binaries\Win64\AceCombat8.exe` suspended:

- `FixDLSSPresets` — patches the 7 `PerfQualityValue` call sites in memory so every
  DLSS dropdown entry maps to its real render scale. A game update that moves the
  code disables the patch instead of corrupting it.
- `[ConsoleVariables]` → Engine.ini: `r.Streamline.DLSSG.Enable` (the only FG
  switch AC8 has), the fallback `FramesToGenerate`, Reflex mode, any other CVar.
  `key=@remove` deletes a key. `RemoveScreenPercentage` drops fixed render-scale
  overrides so the DLSS preset controls it.
- `[SM86.*]` → `Game\Binaries\Win64\dlssg_sm86.ini` (section name without the prefix).
- `LockEngineIni` — keeps Engine.ini read-only while the game runs.
- `EnableDynamicResolution` — AC8's D3D12 renderer never sets
  `GRHISupportsDynamicResolution`, so UE treats dynamic resolution as unsupported and
  `r.DynamicRes.*` does nothing (the state is enabled and unpaused, but the viewport
  never applies it). The bootstrap finds the flag through
  `FDefaultDynamicResolutionState::IsSupported`, which returns it — verifying that
  function's bytes first, so a game update skips the fix — and sets it once the game
  runs. A data write, not a code patch.

It injects no code beyond the preset patch and has no frame generation logic;
`dlss_mfg_sm86.ini` decides the mode. With Mode 0, the game uses `FramesToGenerate`
from the bootstrap INI.

## Mode 3 with dynamic resolution

Set DLSS to **Performance or higher** in game. DLSS reports the range dynamic
resolution may use per mode: Performance, Balanced and Quality allow 50–100%;
Ultra Performance is created at exactly 33% and reports 33–33%, so resolution
can't move there. The shipped INI uses Mode 1 (Dynamic MFG); for Mode 3 it is set
up with `DynamicResolution=1` and a 50–95% range. The maximum stays
well below 100%: with dynamic resolution on, the DLSS plugin picks its mode from the
current resolution, so starting at 100% selects DLAA, whose 99–100% range then pins
resolution at 100% for good. The 50% minimum is
real: lowering the minimum DLSS reports (tested by patching it to 33%) froze the
image as soon as resolution went below 50%; DLSS stops producing frames for inputs
outside its mode's range. Measured (RTX 3070 Laptop, 1440p,
120 Hz): 4x holds 120 fps from a 30 fps base with resolution at 80% in normal
flight (GPU ~29 ms of the 31.7 ms goal; the game's CPU threads ~7 ms per frame);
in the heaviest scenes the 50% floor needs 33.5–35 ms, ~107–114 fps.

## Check after launch

- `Game\Binaries\Win64\dlss_mfg_sm86.log`: `Streamline game, engine=Unreal`,
  `Start Mode=1 ...` (the shipped INI uses Dynamic MFG), then `Hook ... MH_OK` for
  both functions and `dynamicOverride=1` once frame generation starts. In Mode 3:
  `Mode3 found r.DynamicRes...` and `resolution=driven`.
- `Game\Binaries\Win64\dlssg_sm86\logs\backend_<pid>.jsonl`: `"multi_frame_count":3`
  for 4x.

`tools\verify_live_presets.py <pid>` checks that all seven preset patches are
applied in the running game. [VERIFICATION.md](VERIFICATION.md) has the earlier
test history (Mode 1 with the single-mode build on Streamline 2.14.1).
