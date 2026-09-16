param(
    [string]$AppDir = "D:\esp-claw\application\edge_agent"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $AppDir)) {
    throw "App directory not found: $AppDir"
}

Set-Location $AppDir

Write-Host ""
Write-Host "=== ESP-TTS compile probe report ==="
Write-Host "Project: $AppDir"
Write-Host ""

$bin = Join-Path $AppDir "build\edge_agent.bin"
if (-not (Test-Path $bin)) {
    throw "build\edge_agent.bin not found. Build the project first."
}

$bytes = (Get-Item $bin).Length
$partition = 0x400000
$remain = $partition - $bytes
$percent = [math]::Round(($bytes * 100.0) / $partition, 2)

Write-Host ("edge_agent.bin : {0:N0} bytes ({1:N2} MiB)" -f $bytes, ($bytes / 1MB))
Write-Host ("OTA slot limit  : {0:N0} bytes (4.00 MiB)" -f $partition)
Write-Host ("Usage           : {0}%" -f $percent)

if ($remain -ge 0) {
    Write-Host ("Headroom        : {0:N0} bytes ({1:N1} KiB)" -f $remain, ($remain / 1KB))
} else {
    Write-Host ("OVER LIMIT      : {0:N0} bytes ({1:N1} KiB)" -f (-$remain), ((-$remain) / 1KB))
}

Write-Host ""
Write-Host "Run these in the ESP-IDF terminal for the linker breakdown:"
Write-Host "  idf.py size"
Write-Host "  idf.py size-components"
Write-Host ""
