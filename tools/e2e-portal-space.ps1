# End-to-end test for Fences QuickLook Bridge.
#
# The test temporarily shows the desktop (Win+D), performs a real mouse click
# inside a Fences Folder Portal and then a real Space key press, and verifies
# that QuickLook opens on the selected file and closes again.  The desktop is
# restored (Win+D again) when the test finishes.
param(
    [int]$PortalIndex = -1,
    [string]$Exe = (Join-Path $env:LOCALAPPDATA 'Programs\FencesQuickLookBridge\FencesQuickLookBridge.exe'),
    [int]$TimeoutMs = 4000,
    [switch]$ForceDesktop,
    [switch]$KeepQuickLookOpen,
    [switch]$Verbose
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class E2EInput
{
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr hWnd, uint flags);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint attach, uint attachTo, bool attach2);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern bool IsChild(IntPtr parent, IntPtr child);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public const uint LEFTDOWN = 0x0002;
    public const uint LEFTUP = 0x0004;
    public const uint KEYUP = 0x0002;
    public const int SW_MINIMIZE = 6;
    public const int SW_RESTORE = 9;
    public static void Click(int x, int y)
    {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(60);
        mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(40);
        mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
    }
    public static void Space()
    {
        keybd_event(0x20, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(40);
        keybd_event(0x20, 0, KEYUP, UIntPtr.Zero);
    }
    public static bool Focus(IntPtr target)
    {
        IntPtr foreground = GetForegroundWindow();
        uint foregroundThread = GetWindowThreadProcessId(foreground, IntPtr.Zero);
        uint currentThread = GetCurrentThreadId();
        bool attached = AttachThreadInput(foregroundThread, currentThread, true);
        bool ok = SetForegroundWindow(target);
        if (attached) AttachThreadInput(foregroundThread, currentThread, false);
        return ok;
    }
    public static string ClassOfPoint(int x, int y)
    {
        POINT p = new POINT(); p.X = x; p.Y = y;
        IntPtr h = WindowFromPoint(p);
        var sb = new System.Text.StringBuilder(256);
        GetClassName(h, sb, 256);
        return "0x" + h.ToInt64().ToString("X") + " " + sb.ToString();
    }
    public static long WindowAtPoint(int x, int y)
    {
        POINT p = new POINT(); p.X = x; p.Y = y;
        return WindowFromPoint(p).ToInt64();
    }
    public static bool IsSameOrChild(long hwnd, long parent)
    {
        return hwnd == parent || IsChild(new IntPtr(parent), new IntPtr(hwnd));
    }
    public static long[] FindCoveringWindows(int left, int top, int right, int bottom, long portalHwnd, string[] excludeClasses)
    {
        var result = new System.Collections.Generic.List<long>();
        EnumWindows(delegate(IntPtr h, IntPtr l)
        {
            if (h.ToInt64() == portalHwnd) return true;
            if (!IsWindowVisible(h) || IsIconic(h)) return true;
            var sb = new System.Text.StringBuilder(256);
            GetClassName(h, sb, 256);
            string cls = sb.ToString();
            foreach (string excluded in excludeClasses)
            {
                if (string.Equals(excluded, cls, StringComparison.OrdinalIgnoreCase)) return true;
            }
            RECT r;
            if (!GetWindowRect(h, out r)) return true;
            if (r.R - r.L <= 0 || r.B - r.T <= 0) return true;
            bool overlaps = !(r.R <= left || r.L >= right || r.B <= top || r.T >= bottom);
            if (overlaps) result.Add(h.ToInt64());
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }
}
'@

$script:ShellClasses = @('Progman', 'WorkerW', 'Shell_TrayWnd', 'Shell_SecondaryTrayWnd',
    'tooltips_class32', 'ForegroundStaging', 'Windows.UI.Core.CoreWindow',
    'Windows.UI.Composition.DesktopWindowContentBridge', 'DesktopWindowXamlSource',
    'ApplicationManager_DesktopShellWindow', 'TopLevelWindowForOverflowXamlIsland')

# Minimises every visible, non-minimised top level window that overlaps the
# portal area, so that synthetic mouse clicks can reach the Folder Portal.
# The caller restores the returned handles.
function Hide-CoveringWindows([int[]]$bounds, [Int64]$portalHwnd) {
    $hidden = New-Object System.Collections.Generic.List[Int64]
    $handles = [E2EInput]::FindCoveringWindows($bounds[0], $bounds[1], $bounds[2], $bounds[3],
        $portalHwnd, [string[]]$script:ShellClasses)
    foreach ($handle in $handles) {
        [void][E2EInput]::ShowWindow([IntPtr]$handle, [E2EInput]::SW_MINIMIZE)
        $hidden.Add([Int64]$handle)
    }
    return $hidden
}

function Invoke-Bridge([string[]]$BridgeArgs) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = ($BridgeArgs -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    # The bridge writes UTF-8; without this the default ANSI decoding mangles
    # non-ASCII paths that are fed back into later bridge commands.
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.CreateNoWindow = $true
    $process = [System.Diagnostics.Process]::Start($psi)
    $text = $process.StandardOutput.ReadToEnd()
    $process.WaitForExit()
    return @{ Text = $text; Code = $process.ExitCode }
}

function Get-Portals {
    $result = Invoke-Bridge @('--portal-dump')
    $portals = @()
    foreach ($line in ($result.Text -split "`r?`n")) {
        if ($line -match '^PORTAL\[(\d+)\] hwnd=([0-9A-F]+) root=(.*?) items=(\d+) selected=(-?\d+) focused=(-?\d+) bounds=(-?\d+),(-?\d+),(-?\d+),(-?\d+)') {
            $portals += [pscustomobject]@{
                Index = [int]$Matches[1]
                Hwnd = [Int64]("0x" + $Matches[2])
                Root = $Matches[3]
                Items = [int]$Matches[4]
                Selected = [int]$Matches[5]
                Bounds = @([int]$Matches[7], [int]$Matches[8], [int]$Matches[9], [int]$Matches[10])
            }
        }
    }
    return $portals
}

function Get-PortalItemNames([int]$index) {
    $result = Invoke-Bridge @('--portal-dump')
    $names = @{}
    $current = -1
    foreach ($line in ($result.Text -split "`r?`n")) {
        if ($line -match '^PORTAL\[(\d+)\]') { $current = [int]$Matches[1]; continue }
        if ($current -ne $index) { continue }
        if ($line -match '^  ITEM\[(\d+)\](.*?) text=(.*)$') {
            # Capture immediately: -match updates the automatic $Matches variable.
            $itemIndex = [int]$Matches[1]
            $middle = $Matches[2]
            $itemText = $Matches[3]
            $names[$itemIndex] = [pscustomobject]@{
                Text = $itemText
                Selected = ($middle -match 'selected')
                Focused = ($middle -match 'focused')
            }
        }
    }
    return $names
}

# Fences Folder Portals keep the keyboard focus on a file after a click but do
# not always keep the ListView "selected" flag, so the bridge itself accepts
# either state.  The test uses the same rule.
function Get-SelectedItem([int]$index) {
    $names = Get-PortalItemNames $index
    $entry = $names.GetEnumerator() | Where-Object { $_.Value.Selected } | Select-Object -First 1
    if (-not $entry) {
        $entry = $names.GetEnumerator() | Where-Object { $_.Value.Focused } | Select-Object -First 1
    }
    if (-not $entry) { return $null }
    return [pscustomobject]@{ Index = [int]$entry.Key; Text = $entry.Value.Text }
}

function Get-QuickLookState {
    $result = Invoke-Bridge @('--quicklook-state')
    $visible = $false
    $title = ''
    foreach ($line in ($result.Text -split "`r?`n")) {
        if ($line -match '^Visible=(\w+)') { $visible = ($Matches[1] -eq 'True') }
        if ($line -match '^Visible=\w+ Title=(.*)$') { $title = $Matches[1] }
    }
    return [pscustomobject]@{ Visible = $visible; Title = $title }
}

function Wait-QuickLookState([bool]$visible, [string]$contains, [int]$timeout) {
    $started = Get-Date
    while (((Get-Date) - $started).TotalMilliseconds -lt $timeout) {
        $state = Get-QuickLookState
        if ($state.Visible -eq $visible) {
            if (-not $contains -or $state.Title -like ('*' + $contains + '*')) {
                return @{ Ok = $true; Ms = [int]((Get-Date) - $started).TotalMilliseconds; State = $state }
            }
        }
        Start-Sleep -Milliseconds 10
    }
    return @{ Ok = $false; Ms = $timeout; State = (Get-QuickLookState) }
}

# Trace writes to the host and never to the success stream: helpers such as
# Select-HittablePortal return their result on the success stream, and stray
# Write-Output inside a helper would corrupt that value.
function Trace([string]$text) { if ($Verbose) { Write-Host "  . $text" } }

$failures = New-Object System.Collections.Generic.List[string]
$report = New-Object System.Collections.Generic.List[string]
$startedAt = Get-Date

$portals = Get-Portals
$report.Add("Portals detected: $($portals.Count)")
if ($PortalIndex -ge $portals.Count) { throw "Portal index $PortalIndex not found (found $($portals.Count))." }

function Select-HittablePortal([object[]]$Candidates) {
    foreach ($candidate in $Candidates) {
        if ($PortalIndex -ge 0 -and $candidate.Index -ne $PortalIndex) { continue }
        $centerX = [int](($candidate.Bounds[0] + $candidate.Bounds[2]) / 2)
        $centerY = [int](($candidate.Bounds[1] + $candidate.Bounds[3]) / 2)
        $atPoint = [E2EInput]::WindowAtPoint($centerX, $centerY)
        $hit = [E2EInput]::IsSameOrChild($atPoint, $candidate.Hwnd)
        Trace ("portal $($candidate.Index) centre $centerX,$centerY -> 0x$('{0:X}' -f $atPoint) hit=$hit")
        if (-not $hit) {
            # The desktop icon layer or another window covers the portal here.
            foreach ($probe in @(@(40, 40), @(40, 120), @(40, 200), @(40, 300), @(200, 40))) {
                $px = $candidate.Bounds[0] + $probe[0]
                $py = $candidate.Bounds[1] + $probe[1]
                if ($px -ge $candidate.Bounds[2] -or $py -ge $candidate.Bounds[3]) { continue }
                if ([E2EInput]::IsSameOrChild([E2EInput]::WindowAtPoint($px, $py), $candidate.Hwnd)) { $hit = $true; break }
            }
        }
        if ($hit) { return $candidate }
    }
    return $null
}

$cursor = New-Object E2EInput+POINT
[void][E2EInput]::GetCursorPos([ref]$cursor)
$savedX = $cursor.X
$savedY = $cursor.Y
$desktopShown = $false
$hiddenWindows = New-Object System.Collections.Generic.List[Int64]
$savedForeground = [E2EInput]::GetForegroundWindow()

try {
    if ($ForceDesktop) {
        # Only when explicitly requested: minimise every window that overlaps a
        # portal, otherwise the desktop (and the Folder Portals on it) stay
        # exactly as the user left them.
        $collected = New-Object System.Collections.Generic.List[Int64]
        foreach ($candidate in $portals) {
            foreach ($handle in (Hide-CoveringWindows $candidate.Bounds $candidate.Hwnd)) {
                if (-not $collected.Contains($handle)) { $collected.Add($handle) }
            }
        }
        $hiddenWindows = $collected
        $desktopShown = $true
        Trace ("minimised covering windows: " + ($hiddenWindows -join ','))
        Start-Sleep -Milliseconds 600
    }

    # Re-read after minimising: the portal list window can be recreated when
    # Fences redraws the desktop.
    $portals = Get-Portals
    $portal = Select-HittablePortal $portals
    if (-not $portal) {
        $report.Add('SKIP: no Folder Portal is reachable by mouse in the current desktop state.')
        $report | ForEach-Object { Write-Output $_ }
        Write-Output 'E2E_SKIPPED'
        exit 2
    }
    $PortalIndex = $portal.Index
    $report.Add("Testing PORTAL[$PortalIndex] hwnd=0x$('{0:X}' -f $portal.Hwnd) root=$($portal.Root) items=$($portal.Items) bounds=$($portal.Bounds -join ',')")

    $before = Get-SelectedItem $PortalIndex
    if ($before) { $report.Add("Selected before: #$($before.Index) = $($before.Text)") }
    else { $report.Add('Selected before: <none>') }

    # Make sure QuickLook starts closed.
    Invoke-Bridge @('--close') | Out-Null
    Start-Sleep -Milliseconds 500

    $cx = [int](($portal.Bounds[0] + $portal.Bounds[2]) / 2)
    $cy = [int](($portal.Bounds[1] + $portal.Bounds[3]) / 2)
    Trace ("hit test at centre: " + [E2EInput]::ClassOfPoint($cx, $cy))

    # Probe a small grid of click points until the portal selection changes.
    $offsets = @(40, 120, 200, 280, 360)
    $clicked = $false
    $selectedAfterClick = $null
    foreach ($oy in $offsets) {
        foreach ($ox in $offsets) {
            $px = $portal.Bounds[0] + $ox
            $py = $portal.Bounds[1] + $oy
            if ($px -ge $portal.Bounds[2] - 4 -or $py -ge $portal.Bounds[3] - 4) { continue }
            Trace ("clicking $px,$py -> " + [E2EInput]::ClassOfPoint($px, $py))
            [E2EInput]::Click($px, $py)
            Start-Sleep -Milliseconds 350
            $selectedAfterClick = Get-SelectedItem $PortalIndex
            $changed = $selectedAfterClick -and (-not $before -or $selectedAfterClick.Index -ne $before.Index)
            if ($changed) {
                $clicked = $true
                $report.Add("Click at $px,$py selected #$($selectedAfterClick.Index) = $($selectedAfterClick.Text)")
                break
            }
        }
        if ($clicked) { break }
    }

    if (-not $clicked) {
        $failures.Add('No portal item could be selected by clicking inside the portal bounds.')
        $report.Add('Click: FAILED (selection never changed)')
    } else {
        $selectedName = $selectedAfterClick.Text
        Start-Sleep -Milliseconds 300

        # Real Space press -> QuickLook should show that file.
        [E2EInput]::Space()
        $open = Wait-QuickLookState $true $selectedName $TimeoutMs
        if ($open.Ok) {
            $report.Add("Space open: OK in $($open.Ms) ms -> $($open.State.Title)")
        } else {
            $failures.Add("Space did not open QuickLook for '$selectedName' (visible=$($open.State.Visible) title='$($open.State.Title)').")
            $report.Add("Space open: FAILED after $($open.Ms) ms (visible=$($open.State.Visible) title='$($open.State.Title)')")
        }

        if (-not $KeepQuickLookOpen) {
            Start-Sleep -Milliseconds 300
            [E2EInput]::Space()
            $closed = Wait-QuickLookState $false '' $TimeoutMs
            if ($closed.Ok) {
                $report.Add("Space close: OK in $($closed.Ms) ms")
            } else {
                $failures.Add('Space did not close QuickLook again.')
                $report.Add("Space close: FAILED after $($closed.Ms) ms (visible=$($closed.State.Visible) title='$($closed.State.Title)')")
            }
        }
    }
} finally {
    [void][E2EInput]::SetCursorPos($savedX, $savedY)
    if ($desktopShown) {
        foreach ($hwnd in $hiddenWindows) {
            [void][E2EInput]::ShowWindow([IntPtr]$hwnd, [E2EInput]::SW_RESTORE)
        }
        Start-Sleep -Milliseconds 200
        if ($savedForeground -ne [IntPtr]::Zero) { [void][E2EInput]::Focus($savedForeground) }
    }
}

$report.Add("ElapsedMs=$([int]((Get-Date) - $startedAt).TotalMilliseconds)")
$report | ForEach-Object { Write-Output $_ }
if ($failures.Count -gt 0) {
    Write-Output ''
    $failures | ForEach-Object { Write-Output "FAIL: $_" }
    exit 1
}
Write-Output ''
Write-Output 'E2E_OK'
exit 0
