# The First Berserker: Khazan

Khazan (UE 4.27) has its own FG toggle but only ever asks for 2x, and ships a
Streamline runtime too old for multi-frame generation (2.4.15, the same as NG2B).
No bootstrap is needed.

## Install (manual)

Paths are relative to the game folder, e.g. `D:\steamapps\common\The First Berserker Khazan`.
Back up every file you replace first.

| Copy | To | What |
|---|---|---|
| `mod\BBQ\Binaries\Win64\winmm.dll` | `BBQ\Binaries\Win64\` | dlss_mfg_sm86 proxy |
| `mod\BBQ\Binaries\Win64\dlss_mfg_sm86.ini` | `BBQ\Binaries\Win64\` | MFG mode (Mode 1, Dynamic MFG) |
| `mod\BBQ\Binaries\Win64\dlssg_sm86.ini` | `BBQ\Binaries\Win64\` | dlssg_sm86 settings (`Optimized=3`, 4x ceiling) |
| `version.dll` from the [fork root](../../../../version.dll) | `BBQ\Binaries\Win64\` | dlssg_sm86 itself |
| Streamline SDK 2.14.1 `bin\x64`: the eight runtime DLLs | `Engine\Plugins\Runtime\Nvidia\Streamline\Binaries\ThirdParty\Win64\` | **required**: Khazan ships 2.4.15, which has no multi-frame code ([UE5 notes](../../ue5/README.md)) |

`winmm.dll` is a static import of `BBQ-Win64-Shipping.exe`.

The 2.4.15 SDK sends version 1 DLSS-G options and state; Mode 1 upgrades them to
the current layout like Modes 2 and 3 do.

## In game

- Turn Frame Generation on in the game's settings. The game toggles FG off and on
  around loading screens; each request is rewritten again.

## Check after launch

- `BBQ\Binaries\Win64\dlss_mfg_sm86.log`: `Runtime ...\sl.dlss_g.dll version=2.14.1.0`,
  both hooks `MH_OK`, then `State callerVersion=1 dynamicSupported=1` and, once FG
  is on, `Options callerVersion=1 mode=1 ... effectiveMode=3 dynamicOverride=1 result=0`.

Verified live in Mode 1 at 120 Hz: `presented=3`, 120 fps (2x alone gave 112).
