# Ninja Gaiden 2 Black

NG2B has its own FG and Reflex toggles but only ever asks for 2x, and ships a
Streamline runtime too old for multi-frame generation. No bootstrap is needed.

## Install (manual)

Paths are relative to the game folder, e.g. `D:\steamapps\common\NINJAGAIDEN2BLACK`.
Back up every file you replace first.

| Copy | To | What |
|---|---|---|
| `mod\NINJAGAIDEN2BLACK\Binaries\Win64\winmm.dll` | `NINJAGAIDEN2BLACK\Binaries\Win64\` | dlss_mfg_sm86 proxy |
| `mod\NINJAGAIDEN2BLACK\Binaries\Win64\dlss_mfg_sm86.ini` | `NINJAGAIDEN2BLACK\Binaries\Win64\` | MFG mode (Mode 1, Dynamic MFG) |
| `mod\NINJAGAIDEN2BLACK\Binaries\Win64\dlssg_sm86.ini` | `NINJAGAIDEN2BLACK\Binaries\Win64\` | dlssg_sm86 settings (`Optimized=3`, 4x ceiling) |
| `version.dll` from the [fork root](../../../../version.dll) | `NINJAGAIDEN2BLACK\Binaries\Win64\` | dlssg_sm86 itself |
| Streamline SDK 2.14.1 `bin\x64`: the eight runtime DLLs | `NINJAGAIDEN2BLACK\Plugins\Streamline\Binaries\ThirdParty\Win64\` | **required**: NG2B ships 2.4.15, which has no multi-frame code ([UE5 notes](../README.md)) |

`winmm.dll` is a static import of `NINJAGAIDEN2BLACK-Win64-Shipping.exe`.

The 2.4.15 SDK sends version 1 DLSS-G options and state. Mode 1 upgrades them
only since the build that added Khazan; earlier builds passed them through, so
NG2B stayed at the game's 2x in Mode 1 (Mode 2 always upgraded them).

## In game

- Turn Frame Generation on in the game's settings. FG applies at game start, so
  restart after changing it.
- **Set the in-game FPS limit to 30 for 4x at 120 Hz.** NG2B runs Sigma 2's game
  logic, whose speed follows the selected FPS limit. At 4x the base frame rate is
  120 / 4 = 30, so with the limit at 60 the game runs at half speed. Alternatively
  keep the limit at 60 and set `HighRefreshGeneratedFrames=1` (2x, 60 → 120) for
  lower latency.
- When streaming at 60 Hz, Mode 2 picks 2x: use the 30 limit (30 → 60).

## Check after launch

- `NINJAGAIDEN2BLACK\Binaries\Win64\dlss_mfg_sm86.log`: `UE5 Mode=2 refreshHz=120 ...
  multiplier=4`, then `Options ... mode=1 ... effectiveGenerated=3 override=1 result=0`
  once FG is on, and `State ... presented=4`.

Verified with the earlier single-mode build: 4x at 120 Hz, `presented=4`, no failed
kernel launches in dlssg_sm86's log. Mode 1 verified live with the build that added
Khazan: `Options callerVersion=1 mode=1 ... effectiveMode=3 dynamicOverride=1 result=0`,
then `presented=4` at the 30 FPS limit (~116 fps at 120 Hz).
