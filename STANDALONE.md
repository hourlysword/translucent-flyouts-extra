# TranslucentFlyouts standalone revival

This branch (`standalone-revival`) keeps TranslucentFlyouts a standalone tool, not a Windhawk mod, and
re-architects the parts that caused most of the archived project's open issues. It builds against the
current Windows SDK and toolset, fixes the confirmed crashes, and stops injecting into games and other
apps by default. Date: 2026-10-05. Base: `017970c` (post-v3.1.1 master).

## Status

Every change here is **compile-verified** on this machine (x64 Release, Visual Studio 2026 Build Tools,
Windows SDK 10.0.26100). The DLL builds and links cleanly. The behavioural changes — above all the new
process-scoping model in `ProcessScope.cpp` — have **not been runtime-tested**, because TranslucentFlyouts
is a global-injection service that was not installed here and should be validated on a real machine first.
Treat this as a reviewed, buildable foundation, not a shipped release. See "Testing before release" below.

## What changed, and why

### 1. Builds on current Windows (commit `5549988`)
- Windows SDK 10.0.26100 renamed `MENU_POPUPITEM_FOCUSABLE` to `MENU_POPUPITEMFOCUSABLE`; the old name is
  provided when missing so the code compiles on both old and new SDKs.
- Added a `VERSIONINFO` resource (3.2.0.0). Release DLLs previously had no file version, so neither users
  nor the config GUI could tell which build was installed.
- Stopped tracking IDE user files and `TFModern.aps`, ignored `Build/`, `Cache/`, `packages/`, removed the
  stale `RegHelper.cpp.orig`.

### 2. Confirmed crash and correctness fixes (commit `c02c7c5`)
- **Fluent animation use-after-free (issue 142).** The menu subclass held a raw pointer to an animation
  object the worker thread could destroy mid-message; hovering over an expiring menu used freed memory.
  Animations now live in a registry keyed by menu window, the subclass takes a strong reference per
  message, and only the menu's own thread frees the object.
- **Animation worker exit race.** The worker could exit just as new work was queued, leaving a menu cloaked
  and invisible; the exit decision is now made under the lock.
- Removed the thread-hijacking fallback (shellcode that preserved only two registers and ignored stack
  alignment); fell back to the compatibility path when uxtheme trampolines can't be allocated; guarded a
  null-target Detours attach; fixed a `wprintf` format-string bug; `/s` no longer means both stop and
  silent; dropped a dead unsigned loop bound.

### 3. No symbol downloads by default on 24H2+ (commit `558942a`)
The two features that download Microsoft PDB symbols are the main cause of post-update startup failures.
On build 26100 and later they now default off, so a stock install starts with no symbol download:
- `NoModernAppBackgroundColor` defaults to 0 (skips the shell32 symbol walk).
- `EnableCompatibilityMode` defaults to 1 (hooks exported `DrawThemeBackground`/`DrawThemeText` instead of
  byte-scanning uxtheme).
- Symbols, when fetched at all, go over HTTPS.

Explicit registry values always win, so existing setups and the config GUI keep full control.

### 4. Scope to Explorer by default (commit `0705d41`) — the headline change
`ProcessScope.{hpp,cpp}` replaces the single global in-context WinEvent hook. The host now runs one
**out-of-context** watcher (which maps nothing into other processes) and installs a **per-process
in-context** hook only for processes that pass a path policy. By default that is every
`%SystemRoot%\explorer.exe` in the session. Games, launchers, anti-cheat services, other users' processes,
AppContainers, higher-integrity and other-bitness processes are denied *before* the DLL is ever mapped.
`DllMain` re-checks the same policy as defence in depth. Explorer-crash detection moved here (it watches
each hooked process's exit code) instead of polling `GetShellWindow()` on every event, and per-event
registry reads are throttled.

Crash-dump capture and its system-modal dialog are now opt-in (`EnableMiniDump` defaults to 0); it was
forced on in explorer.exe and defaulted on everywhere, so TF blamed itself for unrelated app crashes
(issues 139, 144, 154).

### 5. Autorun task hygiene (commit `471c914`)
The logon task now runs `/start /silent`, uses `IgnoreNew`, and fires only for the installing user, which
removes the "instance already running" dialog on fast user switching (issue 152).

## New and changed registry values

All under `HKEY_CURRENT_USER\SOFTWARE\TranslucentFlyouts` (HKLM as fallback), DWORD unless noted.

| Value | Default | Meaning |
|---|---|---|
| `HookExplorer` | 1 | Style every `%SystemRoot%\explorer.exe` in the session. |
| `AllowList\<name>` | — | Opt-in: also load into this process. `<name>` is an exe name (`code.exe`) or a full image path. Non-zero = allowed. |
| `BlockList\<name>` | — | Never load into this process (pre-existing). |
| `LegacyGlobalHook` | 0 | Restore the pre-standalone behaviour: one global hook into every process. Not recommended. |
| `EnableMiniDump` | 0 | Opt-in crash dump + dialog. Was 1. |
| `Menu\EnableCompatibilityMode` | 1 on build >= 26100, else 0 | Hook exported DrawTheme* functions, no uxtheme symbols. Was 0. |
| `Menu\NoModernAppBackgroundColor` | 0 on build >= 26100, else 1 | Remove UWP icon colour plates (needs shell32 symbols). Was 1. |
| `ElevatedHost` | 0 | Reserved: register the autorun task elevated. Only needed to reach elevated apps; Explorer-only mode works at medium integrity. Not yet wired into the installer. |

## Building

Requires Visual Studio 2022 or 2026 with the C++ workload and a Windows 10/11 SDK. From the repo root:

```bat
msbuild TranslucentFlyouts.sln -t:Restore -p:RestorePackagesConfig=true -p:Configuration=Release -p:Platform=x64
msbuild TranslucentFlyouts.sln -p:Configuration=Release -p:Platform=x64
```

On the VS 2026 toolset (v145) only, VC-LTL 5.0.9 disables itself (warning LTL2003) and the binary links
against the regular UCRT; upgrade to VC-LTL 5.3.1 to restore the smaller build, or build with the v143
toolset from VS 2022. `TFModern` is unmaintained (the author dropped it after v3.1.1) and is not needed.

## Testing before release

The scoping model needs validation on a real install, in this order:
1. Confirm classic menus, tooltips and `Listviewpopup` dropdowns are still styled in Explorer (desktop,
   taskbar, Win+X, "Show more options").
2. Confirm the DLL is **not** present in a running game or other non-Explorer app (Task Manager → Details →
   find the process → check loaded modules), with default settings.
3. Add an app to `AllowList` and confirm it is styled; set `LegacyGlobalHook=1` and confirm the old
   global behaviour returns.
4. Open and dismiss menus rapidly with `Menu\EnableFluentAnimation=1` (the use-after-free repro) and
   confirm no crash.
5. Restart Explorer and confirm TF re-attaches (TaskbarCreated resync).
6. Switch users and back; confirm no "instance already running" dialog.

Not done here and still worth doing for a real release: Authenticode-signing the binaries, verifying the
DLL signature in the host before loading it, and making the per-user install path (no `System32` copy)
the default so Explorer-only mode needs no elevation.
