# Changelog

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
