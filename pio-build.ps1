<#
.SYNOPSIS
    Build, upload, and monitor PlatformIO project for bowie-phone
.DESCRIPTION
    This script handles building, uploading, and monitoring the ESP32 firmware.
    It automatically kills any existing platformio processes to free the COM port.
.PARAMETER Build
    Build the project only (no upload or monitor)
.PARAMETER Upload
    Build and upload the firmware
.PARAMETER Monitor
    Start the serial monitor only
.PARAMETER All
    Build, upload, and monitor (default if no parameters specified)
.PARAMETER Port
    COM port to use (default: COM3)
.PARAMETER Baud
    Baud rate for monitor (default: 115200)
.PARAMETER Clean
    Clean build before building
.EXAMPLE
    .\pio-build.ps1 -All
    .\pio-build.ps1 -Upload -Monitor
    .\pio-build.ps1 -Monitor -Port COM4
    .\pio-build.ps1 -Build -Clean
#>

param(
    [switch]$Build,
    [switch]$Upload,
    [switch]$Monitor,
    [switch]$All,
    [switch]$Clean,
    [string]$Port = "COM3",
    [int]$Baud = 115200
)

$ErrorActionPreference = "Continue"
$PioExe = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"

# If no parameters specified, default to All
if (-not ($Build -or $Upload -or $Monitor -or $All -or $Clean)) {
    $All = $true
}

if ($All) {
    $Build = $true
    $Upload = $true
    $Monitor = $true
}

# Kill any existing platformio processes to free the COM port
Write-Host "🔄 Checking for existing platformio processes..." -ForegroundColor Cyan
$pioProcesses = Get-Process | Where-Object { $_.ProcessName -match "platformio" }
if ($pioProcesses) {
    Write-Host "⚠️ Killing existing platformio processes..." -ForegroundColor Yellow
    $pioProcesses | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

# Change to project directory
Set-Location $PSScriptRoot

# Clean if requested
if ($Clean) {
    Write-Host "🧹 Cleaning build..." -ForegroundColor Cyan
    & $PioExe run -t clean
}

# Build and/or Upload
if ($Build -or $Upload) {
    if ($Upload) {
        Write-Host "🔨 Building and uploading firmware..." -ForegroundColor Cyan
        & $PioExe run -t upload
        if ($LASTEXITCODE -ne 0) {
            Write-Host "❌ Upload failed!" -ForegroundColor Red
            exit $LASTEXITCODE
        }
        Write-Host "✅ Upload successful!" -ForegroundColor Green
        Start-Sleep -Seconds 2
    } else {
        Write-Host "🔨 Building firmware..." -ForegroundColor Cyan
        & $PioExe run
        if ($LASTEXITCODE -ne 0) {
            Write-Host "❌ Build failed!" -ForegroundColor Red
            exit $LASTEXITCODE
        }
        Write-Host "✅ Build successful!" -ForegroundColor Green
    }
}

# Monitor
if ($Monitor) {
    Write-Host "📡 Starting serial monitor on $Port at $Baud baud..." -ForegroundColor Cyan
    Write-Host "   Press Ctrl+C to exit monitor" -ForegroundColor DarkGray
    & $PioExe device monitor --port $Port --baud $Baud
}
