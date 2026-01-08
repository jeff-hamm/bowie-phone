# Static Update Deployment Script for Bowie Phone
# Builds firmware and prepares files for upload to static hosting
# The device checks update.json every 5 minutes and pulls new firmware if available

param(
    [string]$Version = "",
    [switch]$BuildOnly,
    [string]$OutputDir = ".\deploy"
)

Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Bowie Phone - Static OTA Deploy" -ForegroundColor Cyan  
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host ""

# Get version from platformio.ini if not specified
if (-not $Version) {
    $versionLine = Select-String -Path "platformio.ini" -Pattern 'FIRMWARE_VERSION.*"([^"]+)"' | Select-Object -First 1
    if ($versionLine) {
        $Version = $versionLine.Matches.Groups[1].Value
    } else {
        $Version = "1.0.0"
    }
}

Write-Host "Version: $Version" -ForegroundColor Yellow
Write-Host ""

# Step 1: Build firmware
Write-Host "[1/2] Building firmware..." -ForegroundColor Yellow
pio run
if ($LASTEXITCODE -ne 0) {
    Write-Host "❌ Build failed!" -ForegroundColor Red
    exit 1
}
Write-Host "✅ Build successful" -ForegroundColor Green
Write-Host ""

if ($BuildOnly) {
    Write-Host "Build only mode - stopping here." -ForegroundColor Cyan
    exit 0
}

# Step 2: Prepare deploy folder
Write-Host "[2/2] Preparing deploy files..." -ForegroundColor Yellow

# Create output directory
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
New-Item -ItemType Directory -Force -Path "$OutputDir\firmware" | Out-Null

# Copy firmware
$firmwareSrc = ".pio\build\esp32dev\firmware.bin"
$firmwareDst = "$OutputDir\firmware\bowie-phone-$Version.bin"
$firmwareLatest = "$OutputDir\firmware\bowie-phone-latest.bin"

Copy-Item $firmwareSrc $firmwareDst -Force
Copy-Item $firmwareSrc $firmwareLatest -Force

# Create update.json
$updateJson = @{
    version = $Version
    firmware_url = "https://bowie-phone.infinitebutts.com/firmware/bowie-phone-latest.bin"
    release_notes = "Version $Version"
    action = "none"
    message = ""
} | ConvertTo-Json

$updateJson | Out-File -FilePath "$OutputDir\firmware\update.json" -Encoding UTF8

Write-Host "✅ Deploy files ready in: $OutputDir\" -ForegroundColor Green
Write-Host ""
Write-Host "Created files:" -ForegroundColor Cyan
Get-ChildItem -Recurse $OutputDir | ForEach-Object {
    Write-Host "  $($_.FullName.Replace((Get-Location).Path + '\', ''))" -ForegroundColor Gray
}

Write-Host ""
Write-Host "=====================================" -ForegroundColor Green
Write-Host "✅ READY FOR UPLOAD!" -ForegroundColor Green
Write-Host "=====================================" -ForegroundColor Green
Write-Host ""
Write-Host "Upload the 'deploy\firmware\' folder contents to:" -ForegroundColor Cyan
Write-Host "  https://bowie-phone.infinitebutts.com/firmware/" -ForegroundColor White
Write-Host ""
Write-Host "Files to upload:" -ForegroundColor Yellow
Write-Host "  - update.json (update manifest)" -ForegroundColor White
Write-Host "  - bowie-phone-latest.bin (firmware)" -ForegroundColor White
Write-Host "  - bowie-phone-$Version.bin (versioned backup)" -ForegroundColor White
Write-Host ""
Write-Host "To force immediate update on device, use serial command:" -ForegroundColor Yellow
Write-Host "  > phonehome" -ForegroundColor Gray
Write-Host ""
Write-Host "Or to force OTA even if same version, edit update.json:" -ForegroundColor Yellow
Write-Host '  "action": "ota"' -ForegroundColor Gray
