<#
.SYNOPSIS
    Build firmware and publish to docs/firmware for web installer
.DESCRIPTION
    Builds the esp32dev firmware using PlatformIO, then copies all required
    binary files to docs/firmware/ and updates the manifest.json with the
    version from platformio.ini.
.PARAMETER Version
    Override the version string (default: reads from platformio.ini FIRMWARE_VERSION)
.PARAMETER SkipBuild
    Skip the build step and just copy existing binaries
.EXAMPLE
    .\publish-firmware.ps1
    .\publish-firmware.ps1 -Version "1.2.3"
    .\publish-firmware.ps1 -SkipBuild
#>

param(
    [string]$Version,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $ProjectRoot ".pio\build\esp32dev"
$FirmwareDir = Join-Path $ProjectRoot "docs\firmware"
$PioExe = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
$FrameworkDir = "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32"

Write-Host "🔧 Bowie Phone Firmware Publisher" -ForegroundColor Cyan
Write-Host "=" * 40

# Extract version from platformio.ini if not provided
if (-not $Version) {
    $PioIni = Get-Content (Join-Path $ProjectRoot "platformio.ini") -Raw
    if ($PioIni -match 'DFIRMWARE_VERSION=\\"([^"]+)\\"') {
        $Version = $matches[1]
    } else {
        $Version = "1.0.0"
    }
}
Write-Host "📦 Version: $Version" -ForegroundColor Green

# Build firmware
if (-not $SkipBuild) {
    Write-Host "`n🔨 Building firmware..." -ForegroundColor Yellow
    Push-Location $ProjectRoot
    try {
        & $PioExe run -e esp32dev
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
    Write-Host "✅ Build successful!" -ForegroundColor Green
} else {
    Write-Host "`n⏭️  Skipping build (using existing binaries)" -ForegroundColor Yellow
}

# Verify build outputs exist
$FirmwareBin = Join-Path $BuildDir "firmware.bin"
$PartitionsBin = Join-Path $BuildDir "partitions.bin"
if (-not (Test-Path $FirmwareBin)) {
    throw "firmware.bin not found at $FirmwareBin - run build first"
}
if (-not (Test-Path $PartitionsBin)) {
    throw "partitions.bin not found at $PartitionsBin - run build first"
}

# Locate bootloader and boot_app0 from PlatformIO framework
# ESP32 with DIO flash mode @ 40MHz
$BootloaderSrc = Join-Path $FrameworkDir "tools\sdk\esp32\bin\bootloader_dio_40m.bin"
$BootApp0Src = Join-Path $FrameworkDir "tools\partitions\boot_app0.bin"

if (-not (Test-Path $BootloaderSrc)) {
    throw "Bootloader not found at $BootloaderSrc"
}
if (-not (Test-Path $BootApp0Src)) {
    throw "boot_app0.bin not found at $BootApp0Src"
}

# Create firmware directory
if (-not (Test-Path $FirmwareDir)) {
    New-Item -ItemType Directory -Path $FirmwareDir -Force | Out-Null
}

Write-Host "`n📁 Copying binaries to docs/firmware/..." -ForegroundColor Yellow

# Copy all binaries
Copy-Item $BootloaderSrc (Join-Path $FirmwareDir "bootloader.bin") -Force
Write-Host "   ✓ bootloader.bin (from $BootloaderSrc)"

Copy-Item $BootApp0Src (Join-Path $FirmwareDir "boot_app0.bin") -Force
Write-Host "   ✓ boot_app0.bin"

Copy-Item $PartitionsBin (Join-Path $FirmwareDir "partitions.bin") -Force
Write-Host "   ✓ partitions.bin"

Copy-Item $FirmwareBin (Join-Path $FirmwareDir "firmware.bin") -Force
Write-Host "   ✓ firmware.bin"

# Update manifest.json
$ManifestPath = Join-Path $FirmwareDir "manifest.json"
$Manifest = @{
    name = "Bowie Phone"
    version = $Version
    builds = @(
        @{
            chipFamily = "ESP32"
            parts = @(
                @{ path = "bootloader.bin"; offset = 4096 }
                @{ path = "partitions.bin"; offset = 32768 }
                @{ path = "boot_app0.bin"; offset = 57344 }
                @{ path = "firmware.bin"; offset = 65536 }
            )
        }
    )
}
$Manifest | ConvertTo-Json -Depth 10 | Set-Content $ManifestPath -Encoding UTF8
Write-Host "   ✓ manifest.json (version: $Version)"

# Calculate sizes
$TotalSize = (Get-ChildItem $FirmwareDir -Filter "*.bin" | Measure-Object -Property Length -Sum).Sum
$FwSize = (Get-Item (Join-Path $FirmwareDir "firmware.bin")).Length

Write-Host "`n📊 Summary:" -ForegroundColor Cyan
Write-Host "   Firmware size: $([math]::Round($FwSize / 1KB, 1)) KB"
Write-Host "   Total package: $([math]::Round($TotalSize / 1KB, 1)) KB"
Write-Host "   Output: $FirmwareDir"

Write-Host "`n✅ Firmware published successfully!" -ForegroundColor Green
Write-Host "   Open docs/update.html in a browser to flash devices." -ForegroundColor Gray
