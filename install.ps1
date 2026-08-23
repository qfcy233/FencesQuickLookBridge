param(
    [switch]$NoDesktopShortcut
)

$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'FencesQuickLookBridge.exe'
$installDirectory = Join-Path $env:LOCALAPPDATA 'Programs\FencesQuickLookBridge'
$target = Join-Path $installDirectory 'FencesQuickLookBridge.exe'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$shortcutName = 'Fences QuickLook Bridge.lnk'
$desktopShortcut = Join-Path ([Environment]::GetFolderPath('Desktop')) $shortcutName
$startMenuDirectory = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
$startMenuShortcut = Join-Path $startMenuDirectory $shortcutName

if (-not (Test-Path -LiteralPath $source)) {
    throw "FencesQuickLookBridge.exe was not found next to this script: $source"
}

Get-Process -Name 'FencesQuickLookBridge' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
Copy-Item -LiteralPath $source -Destination $target -Force
New-ItemProperty -Path $runKey -Name 'FencesQuickLookBridge' -PropertyType String `
    -Value ('"' + $target + '"') -Force | Out-Null

$shell = New-Object -ComObject WScript.Shell
foreach ($shortcutPath in @($startMenuShortcut)) {
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = $target
    $shortcut.WorkingDirectory = $installDirectory
    $shortcut.Description = 'QuickLook preview bridge for Fences Folder Portals'
    $shortcut.IconLocation = "$target,0"
    $shortcut.Save()
}
if (-not $NoDesktopShortcut) {
    $shortcut = $shell.CreateShortcut($desktopShortcut)
    $shortcut.TargetPath = $target
    $shortcut.WorkingDirectory = $installDirectory
    $shortcut.Description = 'QuickLook preview bridge for Fences Folder Portals'
    $shortcut.IconLocation = "$target,0"
    $shortcut.Save()
}

Start-Process -FilePath $target -WindowStyle Hidden
Write-Host 'Installed. The bridge is now running in the notification area.'
Write-Host '安装完成。程序已在系统托盘中运行。'
