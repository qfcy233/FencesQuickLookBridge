# Test suite for Fences QuickLook Bridge.
#
# Runs the built in self tests of the installed bridge, a live status query and
# (by default) the real end to end portal test.  Prints one line per check and a
# summary.  Output is intentionally ASCII only.
param(
    [string]$Exe = (Join-Path $env:LOCALAPPDATA 'Programs\FencesQuickLookBridge\FencesQuickLookBridge.exe'),
    [int]$NestedSamples = 6,
    [switch]$SkipE2E,
    [switch]$SkipFlicker,
    [switch]$ForceDesktop,
    [switch]$RestartQuickLook,
    [string]$LogDirectory = (Join-Path $env:TEMP 'FencesQuickLookBridge-tests')
)

$ErrorActionPreference = 'Stop'
$script:Failures = New-Object System.Collections.Generic.List[string]
$script:Results = New-Object System.Collections.Generic.List[string]

function Invoke-Bridge([string[]]$Arguments, [switch]$AllowFailure) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = ($Arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    # The bridge writes UTF-8; without this the default ANSI decoding mangles
    # non-ASCII paths that are fed back into later bridge commands.
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.CreateNoWindow = $true
    $process = [System.Diagnostics.Process]::Start($psi)
    $text = $process.StandardOutput.ReadToEnd()
    $process.WaitForExit()
    if (-not $AllowFailure -and $process.ExitCode -ne 0) {
        throw "Bridge command '$($Arguments -join ' ')' failed with exit code $($process.ExitCode): $text"
    }
    return @{ Text = $text; Code = $process.ExitCode }
}

function Add-Result([string]$Name, [bool]$Ok, [string]$Detail) {
    $state = if ($Ok) { 'PASS' } else { 'FAIL' }
    $line = "[$state] $Name - $Detail"
    $script:Results.Add($line)
    Write-Host $line
    if (-not $Ok) { $script:Failures.Add("$Name : $Detail") }
}

function Get-QuickLookInfo {
    $result = Invoke-Bridge @('--quicklook-state') -AllowFailure
    $visible = $false
    $title = ''
    foreach ($line in ($result.Text -split "`r?`n")) {
        if ($line -match '^Visible=(\w+)') { $visible = ($Matches[1] -eq 'True') }
        if ($line -match '^Visible=\w+ Title=(.*)$') { $title = $Matches[1] }
    }
    return [pscustomobject]@{ Visible = $visible; Title = $title }
}

function Restart-QuickLookProcess {
    $app = Get-StartApps | Where-Object { $_.Name -eq 'QuickLook' } | Select-Object -First 1
    if (-not $app) { return $false }
    Get-Process -Name QuickLook -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 800
    Start-Process ('shell:AppsFolder\' + $app.AppID)
    Start-Sleep -Seconds 4
    return [bool](Get-Process -Name QuickLook -ErrorAction SilentlyContinue)
}

if (-not (Test-Path -LiteralPath $Exe)) { throw "Bridge executable not found: $Exe" }
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null

Write-Host "Fences QuickLook Bridge test suite"
Write-Host "Executable: $Exe"
$started = Get-Date

# ---------------------------------------------------------------- version ----
$version = (Invoke-Bridge @('--version')).Text.Trim()
Add-Result 'version' ($version -ne '') "reports $version"

# ------------------------------------------------- executable fingerprint ----
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Exe).Hash
$installedVersion = (Get-Item -LiteralPath $Exe).VersionInfo.FileVersion
Add-Result 'binary' ($installedVersion -match '^3\.') "FileVersion=$installedVersion SHA256=$hash"

# ------------------------------------------------------------ status query ---
$statusResult = Invoke-Bridge @('--status') -AllowFailure
$statusOk = $statusResult.Code -eq 0 -and $statusResult.Text -match 'live status'
if ($statusOk) {
    ($statusResult.Text.Trim() -split "`r?`n") | Set-Content -LiteralPath (Join-Path $LogDirectory 'status.txt')
    $statusFields = @{}
    foreach ($line in ($statusResult.Text -split "`r?`n")) {
        if ($line -match '^([A-Za-z]+)=(.*)$') { $statusFields[$Matches[1]] = $Matches[2] }
    }
    Add-Result 'live status query' $true ("pid=$($statusFields['ProcessId']) portals=$($statusFields['PortalViews']) hookPid=$($statusFields['QuickLookHookPid']) handles=$($statusFields['Handles']) privateMB=$($statusFields['PrivateMB'])")
    $quickLookResponsive = $statusFields['QuickLookResponsive']
} else {
    Add-Result 'live status query' $false ('exit code ' + $statusResult.Code)
    $quickLookResponsive = 'NA'
}

# --------------------------------------------------------------- quicklook ---
$quickLook = Get-QuickLookInfo
Add-Result 'quicklook state probe' $true ("visible=$($quickLook.Visible) responding=$quickLookResponsive")

if ($quickLook.Visible -and $quickLookResponsive -eq 'False') {
    Write-Host 'QuickLook preview window is frozen; closing it before the tests.'
    Invoke-Bridge @('--close') -AllowFailure | Out-Null
    Start-Sleep -Milliseconds 500
    $quickLook = Get-QuickLookInfo
    if ($quickLook.Visible -and $RestartQuickLook) {
        Add-Result 'quicklook restart' (Restart-QuickLookProcess) 'restarted a frozen QuickLook process'
    }
}

# ------------------------------------------------------------ portal tests ---
$dump = (Invoke-Bridge @('--portal-dump')).Text
$portals = @()
foreach ($line in ($dump -split "`r?`n")) {
    if ($line -match '^PORTAL\[(\d+)\] hwnd=([0-9A-F]+) root=(.*?) items=(\d+) selected=(-?\d+) focused=(-?\d+) bounds=(-?\d+),(-?\d+),(-?\d+),(-?\d+)') {
        $portals += [pscustomobject]@{
            Index = [int]$Matches[1]; Hwnd = $Matches[2]; Root = $Matches[3]
            Items = [int]$Matches[4]; Selected = [int]$Matches[5]; Focused = [int]$Matches[6]
        }
    }
}
Add-Result 'folder portal discovery' ($portals.Count -gt 0) ("$($portals.Count) portal(s): " + (($portals | ForEach-Object { $_.Root }) -join ' | '))
if ($portals.Count -eq 0) { Write-Host 'No Folder Portal found; portal specific tests will be skipped.' }

$selection = Invoke-Bridge @('--portal-selection') -AllowFailure
$selectionPath = $selection.Text.Trim()
Add-Result 'portal selection resolve' ($selection.Code -eq 0 -and $selectionPath.Length -gt 0) "-> $selectionPath"

# ------------------------------------------------------- nested resolution ---
# The bridge resolves a deep Portal path by matching the list against the
# directories Fences itself recorded in ViewStates, so the test matrix must use
# exactly those directories (a random folder is intentionally not resolvable).
if ($portals.Count -gt 0) {
    $knownDirectories = New-Object System.Collections.Generic.List[string]
    $viewStates = Get-Item -Path 'HKCU:\Software\Stardock\Fences\ViewStates' -ErrorAction SilentlyContinue
    if ($viewStates) {
        foreach ($valueName in $viewStates.GetValueNames()) {
            if ($valueName -notmatch '\|') { continue }
            $candidate = ($valueName -split '\|')[0]
            if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Container)) {
                $knownDirectories.Add($candidate)
            }
        }
    }
    $samples = New-Object System.Collections.Generic.List[object]
    foreach ($portal in $portals) {
        $root = $portal.Root.TrimEnd('\')
        $matched = $knownDirectories | Where-Object {
            $_ -eq $root -or $_.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)
        }
        foreach ($directory in $matched) {
            if ($samples.Count -ge $NestedSamples) { break }
            $file = Get-ChildItem -LiteralPath $directory -File -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -notmatch '^\.' } | Select-Object -First 1
            if ($file) {
                $samples.Add([pscustomobject]@{ Root = $root; Directory = $directory; Name = $file.Name; Plain = $file.BaseName })
            }
        }
    }
    if ($samples.Count -eq 0) {
        Add-Result 'nested directory resolver' $true 'skipped (no Fences ViewStates directory with a file)'
    } else {
        $nestedPass = 0
        $nestedFail = New-Object System.Collections.Generic.List[string]
        foreach ($sample in $samples) {
            foreach ($name in @($sample.Name, $sample.Plain)) {
                $runner = Invoke-Bridge @('--nested-resolver-test', $sample.Root, $sample.Directory, $name) -AllowFailure
                if ($runner.Code -eq 0 -and $runner.Text -match 'NESTED_RESOLVER_TEST_OK') { $nestedPass++ }
                else { $nestedFail.Add("$($sample.Directory) :: $name (exit $($runner.Code))") }
            }
        }
        $nestedDetail = "$nestedPass passed, $($nestedFail.Count) failed"
        if ($nestedFail.Count -gt 0) { $nestedDetail += ' -> ' + ($nestedFail -join '; ') }
        Add-Result 'nested directory resolver' ($nestedFail.Count -eq 0) $nestedDetail
    }
}

# ------------------------------------------------------------ portal worker --
if ($portals.Count -gt 0 -and $selectionPath.Length -gt 0) {
    Invoke-Bridge @('--close') -AllowFailure | Out-Null
    Start-Sleep -Milliseconds 400
    $worker = Invoke-Bridge @('--portal-worker-test') -AllowFailure
    $match = $worker.Text -match 'PORTAL_WORKER_TEST_OK TitleMs=(\d+) Path=(.*)'
    $latency = if ($match) { $Matches[1] } else { '?' }
    Add-Result 'portal preview (worker)' ($worker.Code -eq 0) "exit=$($worker.Code) latency=${latency}ms"
    Invoke-Bridge @('--close') -AllowFailure | Out-Null
    Start-Sleep -Milliseconds 300
}

# ------------------------------------------------------------- portal rebind -
if ($portals.Count -gt 0) {
    $rebind = Invoke-Bridge @('--portal-rebind-test') -AllowFailure
    Add-Result 'portal window rebind' ($rebind.Code -eq 0 -and $rebind.Text -match 'PORTAL_REBIND_TEST_OK') ($rebind.Text.Trim())
}

# ------------------------------------------------------------------ hook -----
$benchmark = Invoke-Bridge @('--benchmark-hook') -AllowFailure
$benchmarkOk = $benchmark.Code -eq 0 -and $benchmark.Text -match 'HOOK_BENCHMARK'
$detail = ($benchmark.Text -replace 'HOOK_BENCHMARK\s*', '').Trim()
Add-Result 'hook benchmark' $benchmarkOk $detail

# ----------------------------------------------------------------- tray ------
$tray = Invoke-Bridge @('--tray-status') -AllowFailure
Add-Result 'tray icon' ($tray.Code -eq 0) $tray.Text.Trim()

# ---------------------------------------------------------------- flicker ----
if (-not $SkipFlicker -and $portals.Count -gt 0 -and $selectionPath.Length -gt 0) {
    $second = Get-ChildItem -LiteralPath $portals[0].Root -File -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if ($second) {
        $stateBefore = Get-QuickLookInfo
        if ($stateBefore.Visible) {
            Invoke-Bridge @('--close') -AllowFailure | Out-Null
            Start-Sleep -Milliseconds 600
        }
        $flicker = Invoke-Bridge @('--flicker-test', $selectionPath, $second) -AllowFailure
        Add-Result 'rapid switch flicker test' ($flicker.Code -eq 0) ("exit=$($flicker.Code) " + $flicker.Text.Trim())
        Invoke-Bridge @('--close') -AllowFailure | Out-Null
    }
}

# -------------------------------------------------------------------- E2E ----
if (-not $SkipE2E -and $portals.Count -gt 0) {
    $e2eScript = Join-Path $PSScriptRoot 'e2e-portal-space.ps1'
    if (Test-Path -LiteralPath $e2eScript) {
        $e2eArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $e2eScript, '-Exe', $Exe)
        if ($ForceDesktop) { $e2eArguments += '-ForceDesktop' }
        $output = & powershell @e2eArguments 2>&1
        $e2eExit = $LASTEXITCODE
        $e2eOk = ($e2eExit -eq 0)
        $e2eSkip = ($e2eExit -eq 2)
        ($output -join "`r`n") | Set-Content -LiteralPath (Join-Path $LogDirectory 'e2e.txt') -Encoding UTF8
        $summary = ($output | Where-Object { $_ -match '^(Space open|Space close|Click at|Click:|SKIP:)' }) -join '; '
        if ($e2eSkip) {
            Add-Result 'end to end portal space' $true "skipped: desktop is in use or a portal is covered (pass -ForceDesktop to minimise windows) $summary"
        } else {
            Add-Result 'end to end portal space' $e2eOk $summary
        }
    } else {
        Write-Host 'E2E script not found; skipping.'
    }
}

# ---------------------------------------------------------------- summary ----
$elapsed = [int]((Get-Date) - $started).TotalMilliseconds
Write-Host ''
if ($script:Failures.Count -eq 0) {
    Write-Host "TEST_SUITE_OK ($($script:Results.Count) checks, ${elapsed}ms)"
    Write-Host "Logs: $LogDirectory"
    exit 0
}
Write-Host "TEST_SUITE_FAILED ($($script:Failures.Count) of $($script:Results.Count) checks failed, ${elapsed}ms)"
foreach ($failure in $script:Failures) { Write-Host "  - $failure" }
Write-Host "Logs: $LogDirectory"
exit 1
