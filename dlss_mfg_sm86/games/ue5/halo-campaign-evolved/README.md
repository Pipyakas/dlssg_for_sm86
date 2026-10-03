# Halo: Campaign Evolved

Halo's own settings only offer 2x frame generation. The shipped INI uses Mode 1
(NVIDIA Dynamic MFG); Mode 2 is configured one notch lower, and Mode 3 (experimental)
is set up with dynamic resolution at 50–80%. No bootstrap is needed: the in-game FG
toggle stays.

## Install (manual)

Paths are relative to the game folder, e.g. `D:\steamapps\common\Halo Campaign Evolved`.
Back up every file you replace first.

| Copy | To | What |
|---|---|---|
| `mod\Meteorite\Binaries\Win64\winmm.dll` | `Meteorite\Binaries\Win64\` | dlss_mfg_sm86 proxy |
| `mod\Meteorite\Binaries\Win64\dlss_mfg_sm86.ini` | `Meteorite\Binaries\Win64\` | MFG mode |
| `version.dll` + `dlssg_sm86.ini` from the [fork root](../../../../version.dll) | `Meteorite\Binaries\Win64\` | dlssg_sm86 itself |

- **Proxy name.** `HaloCampaignEvolved.exe` statically imports `winmm`, `version` and
  `dxgi`; `version.dll` is dlssg_sm86's, so the proxy is `winmm.dll`.
- **Streamline.** The game ships 2.7.30 in
  `Engine\Plugins\Halo.External\StreamlineCore\Binaries\ThirdParty\Win64`.
  Streamline loads a driver-delivered (OTA) plugin from `C:\ProgramData\NVIDIA\NGX\models`
  only when it is newer than the game's own, so as shipped Halo runs the driver's
  2.14.0, which supports all modes but changes with driver updates. Replacing the
  eight runtime DLLs there with the Streamline SDK 2.14.1 `bin\x64` ones (as for AC8
  and NG2B) makes the game's own 2.14.1 load instead — verified. Back up the
  originals first ([UE5 notes](../README.md)).

## In game

- Turn on DLSS Frame Generation in the game's settings, and Reflex. The game turns
  frame generation off while its window is unfocused. Controller input still
  reaches it in the background, so a window that took focus at launch (a VPN or
  launcher pop-up) leaves the game playable but without FG: the log shows only
  `mode=0`. Click into the game.
- Mode 1 turns the request into Dynamic MFG; the runtime varies the multiplier
  toward the refresh rate.
- `Mode=2` instead uses bands one notch lower than the default: 2x up to 80 Hz, 3x
  up to 120 Hz, 4x above. Mode and bands can be changed while playing.
- `Mode=3` (experimental) keeps 4x and drives UE dynamic resolution. Halo's DLSS
  allows 50–100% (the plugin picks its DLSS mode from the current resolution), so the
  range stays at 50–80%: below 50% DLSS switches to Ultra Performance and above ~83%
  to DLAA. Black or frozen 3D scenes still occurred with dynamic resolution on —
  switching `DynamicResolution` off recovers — and UE's controller was not seen
  lowering resolution under load, so Mode 3 is not the default.
- Known game issue: cutscenes are capped at 30 fps after frame generation, which stays
  on in cutscenes.

## Check after launch

`Meteorite\Binaries\Win64\dlss_mfg_sm86.log`: `Streamline game, engine=Unreal`,
`CVar r.Streamline.DLSSG.RetainResourcesWhenOff pinned`, `Hook ... MH_OK` for both
functions, then once FG is on `Options ... mode=1 ...` and `State ...` with
`presented` varying.

Status: verified live — Mode 1 Dynamic, Mode 2 at 4x, Mode 3 (4x from a 30 fps base,
120 fps in light scenes on an RTX 3070 Laptop at 1440p), and live switching, on the
OTA 2.14.0 plugins.
