# Render-path proxy DLLs (alternatives/)

These are upstream SM86 proxies (English translation of upstream `alternatives/README.md`, maintained by this fork). They are not the [dlss_mfg_sm86](../../../dlss_mfg_sm86/README.md) proxies.

A proxy DLL enters the process by relying on the game loading a system DLL that shares its name. **The release root already ships four utility-class proxies** (`version.dll`, `winmm.dll`, `dbghelp.dll`, `dinput8.dll`). On install, copy every file from the root to the rendering EXE's directory. **No selection is needed:**

- Whichever one the game loads first becomes the active mod (logged as `configuration.proxies.active`);
- the others enter standby (`standby`) and forward their exports verbatim to the real same-named DLL in `C:\Windows\System32\`, installing no hooks, reading no INI and writing no logs;
- therefore several proxies in one directory is **normal** — they do not fight each other and frame generation is not installed twice.

Only when **none of those four are loaded by the game** should you consider the two render-path proxies in this directory.

## The two proxies in this directory (available, but higher risk)

`dxgi.dll` and `d3d12.dll` are themselves entry points of the D3D12 render pipeline. The game calls them densely every frame, and they are load-order sensitive (the game may have already resolved the real DLL by system path before our proxy is in place). Forwarding is complete and functionally correct, but they sit on the render hot path, so they are not shipped at the root and must be copied by hand when needed:

| Name | Placement | Notes |
|---|---|---|
| `alternatives/dxgi.dll` | Beside the EXE | Use only when none of the four root proxies are loaded. |
| `alternatives/d3d12.dll` | Beside the EXE | Same; pick one of `dxgi.dll` / `d3d12.dll`, never both. |

Remove them afterwards and return to the four root proxies.

## Steps

1. First follow the release `README.en.md` as usual: copy **all files from the release root** (the four proxies + `dlssg_sm86.ini`) to the rendering EXE's directory.
2. Start the game and inspect `dlssg_sm86\logs\loader_*.jsonl`: a `runtime_redirect` record means the proxy took effect, and the `proxies` field in the `configuration` record names which one is active and which are in standby (requires `[Logging] Level=2` to see).
3. Only if there is not a single `configuration` / `runtime_redirect` record, copy **one** of `dxgi.dll` or `d3d12.dll` from this directory across as well and try again.

See `docs/INSTALL.en.md` for the complete installation instructions and the full list of INI keys.
