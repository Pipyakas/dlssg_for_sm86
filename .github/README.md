# DLSSG for SM86 — Pipyakas hub

A fork of [sdli1995/dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86), the
proxy that enables DLSS Frame Generation on RTX 30 (SM86) and RTX 20 (SM75) cards.
This fork is two things:

1. **Upstream, untouched.** Every upstream file — binaries, INIs and docs — is
   exactly as upstream has it, kept current automatically (see [Upstream sync](#upstream-sync)).
2. **A hub for companion mods** that build on dlssg_sm86 — for now my own dynamic
   multi-frame generation needs, for any Streamline DLSS-G game.

| Start here | |
|---|---|
| Upstream release notes and usage (English) | [README.en.md](../README.en.md) |
| Upstream INI reference (English) | [docs/INSTALL.en.md](../docs/INSTALL.en.md) |
| Alternative SM86 proxy names (English translation) | [translations/alternatives/README.md](translations/alternatives/README.md) |
| **dlss_mfg_sm86** — dynamic MFG for games that don't offer it | [dlss_mfg_sm86/README.md](../dlss_mfg_sm86/README.md) |

Upstream docs that exist only in Chinese have English translations under
[`translations/`](translations), mirroring their upstream paths. They are maintained
by hand and can lag behind upstream.

## Companion mods

### dlss_mfg_sm86

dlssg_sm86 only does what the game asks for, and most games ask for 2x or have no
FG setting at all. dlss_mfg_sm86 fills those gaps: a proxy DLL (named like the main
mod's proxies — `winmm`, `dinput8`, `dbghelp`, ...) that turns the game's own FG
requests into

- **Mode 1** — NVIDIA Dynamic MFG: the runtime picks the multiplier, or
- **Mode 2** — a fixed count picked from the display refresh rate (2x up to 60 Hz,
  3x up to 90 Hz, 4x above), so streaming to a 60 Hz virtual display and playing
  locally at 120 Hz each get the right multiplier, or
- **Mode 3** (experimental) — the most generated frames for the refresh rate, with
  Unreal's dynamic resolution filling the GPU time left.

It works in any Streamline DLSS-G game with a generic drop-in install; Unreal
games also get dynamic resolution and console-variable pins. Games that need more
(an FG switch, a newer Streamline runtime, a bug fix) are grouped by engine —
[`games/ue5/`](../dlss_mfg_sm86/games/ue5/README.md) today, with ACE COMBAT 8, Halo:
Campaign Evolved and Ninja Gaiden 2 Black — and each has a copy-ready `mod/` folder
with its working files, including its `dlssg_sm86` settings (this fork runs
`Optimized=3`, the fastest tier). Installation is manual.

## How this fork is kept

All fork changes are **one commit on top of upstream `main`**, and that commit only
adds files under `.github/` and `dlss_mfg_sm86/`. Because it never edits an upstream
file, rebasing onto a new upstream release cannot conflict.

To change something, amend that commit and force-push:

```powershell
git commit --amend --no-edit   # or: git commit --fixup HEAD; git rebase -i --autosquash
git push --force-with-lease fork main
```

## Upstream sync

[`workflows/sync-upstream.yml`](workflows/sync-upstream.yml) checks upstream every
15 minutes (and on demand from the Actions tab). When upstream `main` has moved, it
rebases the fork commit onto it and force-pushes. If a rebase ever fails (upstream
started using a path the fork uses), it aborts, leaves the fork as it was, and
opens an issue labelled `upstream-sync`.

Local clones follow with `git fetch origin && git rebase origin/main` (`origin` =
upstream), or simply `git pull --rebase fork main`. GitHub pauses scheduled
workflows after 60 days without repository activity; re-enable it from the Actions
tab if that happens.
