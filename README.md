# Fences QuickLook Bridge

[简体中文](README.zh-CN.md)

A small native Windows bridge that enables <kbd>Space</kbd> preview with
[QL-Win QuickLook](https://github.com/QL-Win/QuickLook) for files selected inside a
Stardock Fences 6 **Folder Portal**.

Regular desktop fences and File Explorer are intentionally left to QuickLook's own
keyboard hook. The bridge handles only a confirmed Folder Portal selection, which
avoids duplicate previews and stale-file flashes when switching between surfaces.

This project addresses the long-standing
[QuickLook issue #332](https://github.com/QL-Win/QuickLook/issues/332).

## Features

- Selection-based detection; the mouse position is not used.
- Works after navigating into nested folders inside a Folder Portal.
- Rebinds when Fences recreates the Portal list window during navigation.
- Event-driven native Win32 implementation with no polling loop, CLR, or telemetry.
- Runs as a single notification-area process and needs no administrator rights.
- Does not inject into, patch, or modify Fences or QuickLook.

## Requirements

- Windows 10 or Windows 11, x64
- Stardock Fences 6 with a Folder Portal
- [QL-Win QuickLook](https://github.com/QL-Win/QuickLook)

The bridge depends on implementation details that are not a public Fences API.
Future Fences or QuickLook updates may require a bridge update.

## Install

1. Download `FencesQuickLookBridge-v3.4.4-win-x64.zip` from the Releases page.
2. Extract the archive.
3. Right-click `install.ps1` and choose **Run with PowerShell**.
4. Select a file in a Folder Portal and press <kbd>Space</kbd>.

If Windows blocks the script, open PowerShell in the extracted directory and run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
```

The installer copies the executable to the current user's Local AppData, adds a
current-user startup entry, creates a desktop shortcut, and starts the tray process.
It does not require elevation.

The executable is currently unsigned, so Windows SmartScreen may show a warning.
The source and reproducible build command are included in this repository for audit.

## Tray menu

- Double-click the tray icon to show version, process ID, and detected Portal count.
- Right-click and choose **Refresh** after changing Folder Portal mappings.
- Right-click and choose **Exit** to stop the bridge.

Only one instance can run at a time.

## Diagnostics

The same executable answers a few read-only queries while an instance is running:

| Command | Purpose |
| --- | --- |
| `FencesQuickLookBridge.exe --status` | Live state of the running instance: active Portal selection, QuickLook hook order, counters, memory and handles. |
| `FencesQuickLookBridge.exe --diagnose` | One-shot snapshot of the Portal cache and the QuickLook window. |
| `FencesQuickLookBridge.exe --portal-dump` | Every detected Folder Portal, its list window and its items. |
| `FencesQuickLookBridge.exe --portal-selection` | Resolve the currently selected Portal file to a full path. |
| `FencesQuickLookBridge.exe --quicklook-state` | Whether a QuickLook preview window is open, and its title. |
| `FencesQuickLookBridge.exe --tray-status` | Whether the tray icon of the running instance exists. |

If the QuickLook preview window stops responding, the bridge shows a tray warning
once instead of silently swallowing <kbd>Space</kbd>; restart QuickLook and press
<kbd>Space</kbd> again.

## Test suite

The `tools` directory contains the automation used to validate a build:

```powershell
.\tools\run-tests.ps1                  # self tests, resolution matrix, flicker, status
.\tools\run-tests.ps1 -ForceDesktop    # also run the real click + Space end-to-end test
```

The end-to-end test needs an unobstructed desktop because it clicks a real file
inside a Folder Portal and presses a real <kbd>Space</kbd>. Without `-ForceDesktop`
it skips whenever another window covers the desktop, so it never disturbs the
current session.

## Uninstall

Run `uninstall.ps1`. It stops the bridge, removes its current-user startup entry and
shortcuts, and deletes only `%LOCALAPPDATA%\Programs\FencesQuickLookBridge`.
Fences and QuickLook are not changed.

## Build from source

Install [Zig 0.16.0](https://ziglang.org/) and place `zig` on `PATH`, then run:

```powershell
.\build.ps1
```

Or pass a specific compiler:

```powershell
.\build.ps1 -ZigPath C:\path\to\zig.exe
```

The x64 executable is written to `build\FencesQuickLookBridge.exe`.

## How it works

The bridge observes Windows selection-change events and remembers which file list owns
the current selection. When an unmodified <kbd>Space</kbd> press belongs to a confirmed
Folder Portal, a sleeping worker resolves the selected path and sends it to QuickLook's
existing current-user named pipe. All other Space events are forwarded immediately.

Nested-directory resolution opens the Explorer process once per request and reuses one
remote list buffer. A small cache stores the item count and three exact list anchors, so
repeat previews usually avoid a full directory match. No filenames leave the machine.

## Privacy and security

- No network access, update checker, analytics, or telemetry.
- No administrator rights, service, driver, DLL injection, or process patching.
- Reads only the selected Folder Portal list data needed to resolve the local path.
- Communicates only with the current user's local QuickLook named pipe.

## License and trademarks

The source code is released under the [MIT License](LICENSE).

This is an independent community project and is not affiliated with or endorsed by
Stardock Systems, Inc. or the QL-Win QuickLook project. Fences and QuickLook are names
and trademarks of their respective owners.
