# dlss_mfg_sm86

Fills the gaps between [dlssg_sm86](../README.en.md) and multi-frame generation.
dlssg_sm86 makes DLSS Frame Generation run on RTX 30/20, but it only does what the
game asks for — and most games only ask for 2x, or have no MFG setting at all.
dlss_mfg_sm86 rewrites the game's own frame generation requests so the main mod
can run NVIDIA Dynamic MFG, a refresh-matched 2x/3x/4x, or a governor that trades
resolution for generated frames.

It works with **any game that uses Streamline DLSS-G** (DLSS Frame Generation),
whatever the engine. Unreal Engine games (UE4.22+ / UE5) additionally get the
console-variable features: Mode 3 dynamic resolution and `[ConsoleVariables]`.

## Install (any Streamline game)

1. Take `winmm.dll` and `dlss_mfg_sm86.ini` from `build\release\` (or the
   `dist\dlss_mfg_sm86.zip` package).
2. Put both next to the game's rendering EXE — for Unreal games
   `<Game>\<Project>\Binaries\Win64\`, the folder with `<Project>-Win64-Shipping.exe`
   (or the main EXE) and usually dlssg_sm86's `version.dll`.
3. If the game already has a `winmm.dll` there, use `dinput8.dll` or `dbghelp.dll`
   instead, or one from `alternatives\`. Back up any file you replace.
4. Start the game with frame generation on. `dlss_mfg_sm86.log` beside the INI
   shows `Streamline game, engine=…` and every frame generation request.

The defaults (Mode 2) suit a game with a 2x-only or missing MFG setting. Games
that need more — a launcher fix, a tuned INI — get a folder under `games/`.

## Layout

```text
dlss_mfg_sm86/
├─ src/                      core proxy DLL (Streamline DLSS-G hooks) + tests
├─ config/dlss_mfg_sm86.ini  default configuration (the generic release uses it)
├─ games/                   per-game extras, grouped by engine
│  ├─ ue4/                  category: Unreal Engine 4 games (shares the UE5 notes)
│  │  └─ the-first-berserker-khazan/
│  └─ ue5/                  category: Unreal Engine 5 games
│     ├─ README.md           what UE5 games have in common
│     ├─ ace-combat-8/       one folder per game
│     │  ├─ README.md        requirements and manual install
│     │  ├─ mod/             copy-ready files, laid out like the game folder
│     │  └─ bootstrap/ tools/  game-specific source and helpers
│     ├─ halo-campaign-evolved/
│     └─ ninja-gaiden-2-black/
├─ vendor/                   MinHook 1.3.4, Streamline 2.14.1 public headers
├─ tools/package.ps1         zips the generic release
└─ build.cmd
```

A game only needs a folder when the generic install isn't enough. Unreal games go
under `games/ue5/` or `games/ue4/`; other engines get a category folder of
their own (for example `games/unity/`). The core DLL needs no per-engine changes.

Deployment is always manual: the target is machine-dependent, so the repo holds
working mod files, not deploy scripts. Each game README lists what to copy where.

## Core DLL

Ships only as proxy DLLs named after system DLLs the game already loads, each
forwarding every System32 export of its namesake — the same convention as
dlssg_sm86:

```text
winmm.dll  dinput8.dll  dbghelp.dll   utility-class proxies
dlss_mfg_sm86.ini                     configuration (same name for every proxy)
alternatives\dxgi.dll  d3d12.dll  version.dll
```

dlssg_sm86 is usually `version.dll`, which is why it is only an alternative here.
If several copies load, the first one runs and the rest only forward. The DLL
waits up to `[Hooks] WaitTimeoutMs` (120 s) for `sl.interposer.dll`; processes
that never load Streamline (launchers, crash reporters) are left alone.

It only rewrites frame generation requests the game already makes; FG on/off and
Reflex stay with the game (games commonly turn FG off while unfocused or in
menus). `[FrameGeneration] Mode`:

| Mode | Behaviour | Needs |
|---|---|---|
| `0` | pass-through | — |
| `1` | **NVIDIA Dynamic MFG.** On/auto requests become `eDynamic`; the runtime picks the multiplier. `TargetFPS=0` follows the monitor's refresh rate. | a runtime reporting Dynamic support (Streamline 2.14.1 verified) |
| `2` (default) | **Refresh-based fixed count** from the primary display's refresh rate: 2x up to `LowRefreshMaxHz` (60), 3x up to `MediumRefreshMaxHz` (90), 4x above. Counts are configurable per band. | Streamline 2.7+ |
| `3` | **Experimental governor** (see below): generated frames first, then — in Unreal games — dynamic resolution fills the GPU time left. | Reflex on in the game; Streamline 2.7+ |

Thresholds are inclusive upper bounds. Streaming apps make their 60 Hz virtual
display primary, so Mode 2 picks 2x there and 4x on a local 120 Hz panel — and
switches when that changes mid-game (`[Reload] FollowRefreshRate`).

### Mode 3 (experimental)

Each `[Mode3] DecisionIntervalMs`:

1. **Measure.** Streamline's Reflex latency reports (`slReflexGetState`) give, per
   rendered frame, the game-thread (simulation) and render-thread (submit) times and
   the GPU busy time, frame generation included. The threads run pipelined, so the
   CPU limit is the slower of the two.
2. **Multiplier.** `Prefer=1` (default, *frames first*): the most generated frames
   whose base rate, target / (n+1), stays at or above `MinBaseFPS` (30) — 4x at
   120 Hz. GPU time goes to generation rather than resolution, and the low base
   rate frees CPU. `Prefer=0` (*latency first*): the fewest generated frames that
   reach the target, from a cost model of render time and per-frame generation
   cost (`CostModel=1`), or filled from the CPU limit (`CostModel=0`).
3. **Resolution (Unreal only, opt-in).** `DynamicResolution=1`: the DLL locates
   `r.DynamicRes.OperationMode`, `FrameTimeBudget`, `MinScreenPercentage` and
   `MaxScreenPercentage` in the running game and drives them live. UE compares its
   budget with its own render time, which excludes frame generation, so the budget
   is a feedback loop: total GPU time is steered to `BudgetPercent` of the base
   frame time (the CPU limit, or with `Prefer=1` the chosen base rate), never
   above that goal and never below `MinBudgetPercent` of it. Frame generation is
   told that depth and motion-vector sizes vary (`DLSSGFlags::eDynamicResolutionEnabled`,
   required by the Streamline DLSS-G guide; `FlagFrameGen`).
   `DynamicResolution=2` (last resort): for games whose DLSS setting fixes the
   render resolution and ignores engine dynamic resolution (AC8), the DLL steps
   `r.ScreenPercentage` between DLSS quality levels — Ultra Performance 33.3,
   Performance 50, Balanced 58, Quality 66.7, DLAA 100 — within the same range
   and toward the same goal. Down a level after two decisions over the goal, then
   30 decisions before trying higher; up a level after `SettleDecisions` in which
   the level above is predicted to fit (GPU time measured there before, else a
   cautious estimate). It starts from the level nearest the game's own value and
   restores that value when switched off. Don't also pin `r.ScreenPercentage` in
   `[ConsoleVariables]`.
   With `DynamicResolution=0`, or outside Unreal, Mode 3 sets the frame count only.

Each decision is logged: `Mode3 cpu=… gpu=… frame=… base=… target=… generated=…
output=… budget=… goal=… prefer=frames|latency resolution=driven|screen|advisory|off screen=…%`.
All `[Mode3]` keys apply live; leaving Mode 3 (or `DynamicResolution=0`) restores
the game's own resolution values.

**Findings (RTX 3070 Laptop, 1440p, 120 Hz).**

- Resolution alone reaches the CPU limit (~63 fps) at ≤40%; about 11 ms of GPU time
  per frame doesn't scale with resolution.
- Each generated frame costs ~3.5–7 ms of GPU time, so the CPU-limited base can't
  be kept once frame generation runs: 2x tops out at ~91 fps. With the game's 120
  cap, 4x reaches 120 from a 30 fps base at up to ~55–60% resolution in light
  scenes; heavy scenes stay at ~100–115 fps at the 33% floor.
- AC8's renderer never sets `GRHISupportsDynamicResolution`, so UE ignored
  `r.DynamicRes.*` there; its bootstrap now sets the flag. DLSS also limits the
  range per mode: Performance–Quality 50–100%, Ultra Performance fixed at 33%.
  With DLSS Performance, 4x holds 120 fps at 67%. Streamline is not involved.
- Without `FlagFrameGen` the image corrupts. Changing `MaxScreenPercentage` or
  switching dynamic resolution on can leave the 3D scene frozen or black behind a
  live HUD until the next resolution change; moving within a fixed range doesn't.
  So set the range once and leave it.

### `[ConsoleVariables]` (Unreal only)

`name=value` lines pin UE console variables in memory: re-applied every
`[Reload] IntervalMs`, so the game can't switch them back, and never saved. Integers
as `1`, floats with a dot (`1.0`). Removing a line unpins it; the value stays
until the game sets it (the value seen at the first write can predate the game's
own settings). Found variables and later changes by the game are logged. Example
(Halo): `r.Streamline.DLSSG.RetainResourcesWhenOff=1` keeps frame generation
resources while the game has FG off, instead of rebuilding them each time.

### Advanced keys

The shipped INI holds only the keys worth tweaking. These also work when added by
hand (shown with their defaults):

| Section | Key = default | Meaning |
|---|---|---|
| `[FrameGeneration]` | `LowRefreshGeneratedFrames=1` `MediumRefreshGeneratedFrames=2` `HighRefreshGeneratedFrames=3` | Mode 2 count per refresh band (generated frames: 1 = 2x) |
| `[Mode3]` | `TargetFPS=` | Mode 3 target, overriding `[FrameGeneration] TargetFPS` |
| | `DecisionIntervalMs=1000` | how often Mode 3 measures and decides |
| | `MinGeneratedFrames=1` | lower limit for the count |
| | `CostModel=1` `FrameGenCostMs=7` `TolerancePercent=10` `SettleDecisions=2` | `Prefer=0` only: cost model (0 = fill from the CPU limit), prior generation cost, target overshoot, decisions a new count must win |
| | `MinBudgetPercent=50` | lowest GPU budget, percent of the goal |
| | `FlagFrameGen=1` `FrameGenResolutionPercent=0` | announce varying depth/motion-vector sizes to frame generation (0 corrupts the image); its fixed internal resolution (0 = runtime default) |
| `[Reload]` | `IntervalMs=1000` | INI check interval; 0 = read once at launch |
| | `FollowRefreshRate=1` | Mode 2 re-picks the count when the primary display's refresh rate changes |
| `[Hooks]` | `WaitTimeoutMs=120000` `LookupDelayMs=10000` `RetryCount=30` `RetryDelayMs=1000` | how long to wait for Streamline and retry hooking |
| `[Logging]` | `Directory=.` `SampleIntervalMs=10000` | log folder (relative to the INI, environment variables expand); periodic sample interval |

### Live reload

Edits to `dlss_mfg_sm86.ini` are picked up within `[Reload] IntervalMs` (1 s) and
apply on the game's next frame. The log records each `Reload` / `RefreshChange`
and the next request.

| Live | Needs a game restart |
|---|---|
| `Mode` (0 / 1 / 2 / 3, any direction), all `[Mode3]` keys | `[Hooks]` timings |
| `TargetFPS`, Mode 2 bands and counts | `[Logging] Directory` |
| `[ConsoleVariables]` | dlssg_sm86's own `dlssg_sm86.ini` (`Optimized`, the `MaxGeneratedFrames` ceiling) |
| `[Reload]` itself, `[Logging] Enabled` / `SampleIntervalMs` | a game bootstrap's settings (e.g. AC8 Engine.ini CVars) |

`IntervalMs=0` reads the file once at launch. If the runtime rejects an override,
the DLL logs it, retries the game's original request and stops overriding for that
session.

## Build

```powershell
& .\build.cmd          # builds, runs every test suite, refreshes each mod\ folder
& .\tools\package.ps1  # dist\dlss_mfg_sm86.zip from build\release\
```

Requires Visual Studio with the x64 C++ tools and Python 3. Builds are reproducible
(`/Brepro`), so the committed `mod\` binaries only change when the source does.

Local-only (git-ignored): `build/`, `dist/`, `backups/` (game-file backups),
`logs/`, `research/`, `vendor/streamline/bin/` (NVIDIA runtime DLLs).

## Games

Any Streamline DLSS-G game works with the generic install. These have their own
folder:

| Category | Game | Proxy | Bootstrap | Status |
|---|---|---|---|---|
| UE5 | [ACE COMBAT 8](games/ue5/ace-combat-8/README.md) | `dinput8.dll` | yes — no FG toggle, DLSS preset bug, dynamic resolution flag | Mode 1 (shipped); Modes 1 and 3 verified live with the current build |
| UE5 | [Halo: Campaign Evolved](games/ue5/halo-campaign-evolved/README.md) | `winmm.dll` | no | Mode 1 (shipped); Modes 1–3 and live switching verified |
| UE5 | [Ninja Gaiden 2 Black](games/ue5/ninja-gaiden-2-black/README.md) | `winmm.dll` | no | Mode 1 (shipped; game speed follows its FPS limit — keep it at 30 for 120 Hz); Mode 1 verified live with the current build |
| UE4 | [The First Berserker: Khazan](games/ue4/the-first-berserker-khazan/README.md) | `winmm.dll` | no | Mode 1 (shipped; Streamline upgraded from 2.4.15); Mode 1 verified live with the current build |

MinHook: https://github.com/TsudaKageyu/minhook/tree/v1.3.4 (BSD, `vendor/minhook/LICENSE.txt`).
Streamline SDK: https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1.
