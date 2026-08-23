param(
    [string]$ZigPath = 'zig'
)

$ErrorActionPreference = 'Stop'
$projectDirectory = $PSScriptRoot
$sourceDirectory = Join-Path $projectDirectory 'native'
$buildDirectory = Join-Path $projectDirectory 'build'
$resourceObject = Join-Path $buildDirectory 'BridgeNative-resource.o'
$outputExecutable = Join-Path $buildDirectory 'FencesQuickLookBridge.exe'

if (Test-Path -LiteralPath $ZigPath) {
    $zig = (Resolve-Path -LiteralPath $ZigPath).Path
} else {
    $command = Get-Command $ZigPath -ErrorAction SilentlyContinue
    if (-not $command) {
        throw 'Zig was not found. Install Zig 0.16.0 or pass -ZigPath C:\path\to\zig.exe.'
    }
    $zig = $command.Source
}

New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null

Push-Location $sourceDirectory
try {
    & $zig rc '/:output-format' 'coff' '/fo' $resourceObject 'BridgeNative.rc'
    if ($LASTEXITCODE -ne 0) { throw "Resource compilation failed with exit code $LASTEXITCODE." }

    & $zig cc '-target' 'x86_64-windows-gnu' '-Os' '-s' '-DUNICODE' '-D_UNICODE' `
        '-municode' 'BridgeNative.c' $resourceObject '-o' $outputExecutable `
        '-lcomctl32' '-lshell32' '-lole32' '-ladvapi32' '-lpsapi' '-luser32' '-lgdi32'
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed with exit code $LASTEXITCODE." }
} finally {
    Pop-Location
}

& $outputExecutable '--version'
if ($LASTEXITCODE -ne 0) { throw 'The built executable failed its version smoke test.' }

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $outputExecutable).Hash
Write-Host "Built: $outputExecutable"
Write-Host "SHA256: $hash"
