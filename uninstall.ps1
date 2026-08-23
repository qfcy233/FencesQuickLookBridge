$ErrorActionPreference = 'Stop'
$installDirectory = Join-Path $env:LOCALAPPDATA 'Programs\FencesQuickLookBridge'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$shortcutName = 'Fences QuickLook Bridge.lnk'
$desktopShortcut = Join-Path ([Environment]::GetFolderPath('Desktop')) $shortcutName
$startMenuShortcut = Join-Path `
    (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs') $shortcutName

Get-Process -Name 'FencesQuickLookBridge' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Remove-ItemProperty -Path $runKey -Name 'FencesQuickLookBridge' -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $desktopShortcut -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $startMenuShortcut -Force -ErrorAction SilentlyContinue

if (Test-Path -LiteralPath $installDirectory) {
    Remove-Item -LiteralPath $installDirectory -Recurse -Force
}

Write-Host 'Uninstalled. Fences and QuickLook were not changed.'
Write-Host '卸载完成；Fences 和 QuickLook 未被修改。'
