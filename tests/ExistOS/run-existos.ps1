[CmdletBinding()]
param(
    [switch]$ResetFlash,
    [switch]$Snapshot,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $PSCommandPath
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $scriptDir "..\..")).Path

$qemu = Join-Path $repoRoot "build\qemu\build\qemu-system-arm.exe"
$rom = Join-Path $scriptDir "hypervisor-rom.bin"
$initialFlash = Join-Path $scriptDir "flash.initial.bin"
$runtimeDir = Join-Path $repoRoot "build\ExistOS"
$runtimeFlash = Join-Path $runtimeDir "flash.bin"

if (-not (Test-Path -LiteralPath $qemu)) {
    throw "QEMU executable not found: $qemu. Build it first."
}
if (-not (Test-Path -LiteralPath $rom)) {
    throw "Missing Hypervisor ROM: $rom"
}
if (-not (Test-Path -LiteralPath $initialFlash)) {
    throw "Missing initial Flash image: $initialFlash"
}

New-Item -ItemType Directory -Force -Path $runtimeDir | Out-Null

if ($ResetFlash -or -not (Test-Path -LiteralPath $runtimeFlash)) {
    Copy-Item -LiteralPath $initialFlash -Destination $runtimeFlash -Force
}

$drive = "file=$runtimeFlash,if=none,format=raw"
if ($Snapshot) {
    $drive += ",snapshot=on"
}

$qemuArgs = @(
    "-M", "stmp3770",
    "-bios", $rom,
    "-drive", $drive,
    "-display", "none",
    "-monitor", "none",
    "-serial", "stdio"
)

Write-Host "ROM:   $rom"
Write-Host "Flash: $runtimeFlash"
if ($Snapshot) {
    Write-Host "Mode:  snapshot, runtime Flash changes are discarded by QEMU"
} else {
    Write-Host "Mode:  writable runtime Flash copy under build\ExistOS"
    Write-Host "Note:  runtime Flash may grow with appended OOB metadata"
}
Write-Host ""

if ($DryRun) {
    Write-Host "& `"$qemu`" $($qemuArgs -join ' ')"
    exit 0
}

& $qemu @qemuArgs
exit $LASTEXITCODE
