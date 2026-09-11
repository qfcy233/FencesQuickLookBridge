# Changelog

## 3.4.4 - 2026-09-12

- Add a live status query (`--status`) that reports the active Portal selection,
  hook order, counters and resource usage of the running instance.
- Make `--portal-selection` fall back to any Portal selection so the diagnostic
  works when no selection event has been received yet.
- Detect a frozen QuickLook preview window before swallowing Space and show a
  tray warning instead of failing silently.
- Report a clear reason when the flicker test cannot run because the preview
  window does not close.
- Keep the keyboard hook order correct while a QuickLook process is starting.
- Add `tools/run-tests.ps1` and `tools/e2e-portal-space.ps1` test automation.
- Align the embedded version resource with the reported version.

## 3.4.3 - 2026-08-23

- Build as a native Windows GUI application so launching the shortcut never opens a terminal.
- Embed the new F icon in the executable and reuse it for the shortcut, window class, and tray icon.

## 3.4.2 - 2026-08-23

- Limit interception to confirmed Fences Folder Portal selections.
- Leave regular desktop fences and File Explorer entirely to QuickLook.
- Use selection ownership rather than mouse position.
- Rebind Portal list windows recreated during nested-folder navigation.
- Reduce nested-folder latency by reusing one Explorer process handle and remote buffer.
- Replace broad directory sampling with up to 16 evenly distributed items.
- Cache three exact list anchors and the item count for fast repeat previews.
- Prevent stale-file flashes when switching files or leaving a Folder Portal.
- Add a notification-area icon, status dialog, refresh command, exit command, and
  single-instance protection.
- Keep idle operation event-driven with no polling loop or persistent timer.
