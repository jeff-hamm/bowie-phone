# OTA Deployment Script for Bowie Phone
# This script builds firmware, prepares the device, and performs OTA update via WireGuard

param(
    [string]$DeviceIP = "10.253.0.2",
    [string]$UnraidIP = "100.86.189.46",
    [int]$OTAPort = 3232,
    [string]$OTAPassword = "bowie-ota-2024",
    [string]$ComPort = "COM3"
)

Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Bowie Phone OTA Deployment" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host ""

# Step 1: Build firmware
Write-Host "[1/5] Building firmware..." -ForegroundColor Yellow
pio run
if ($LASTEXITCODE -ne 0) {
    Write-Host "❌ Build failed!" -ForegroundColor Red
    exit 1
}
Write-Host "✅ Build successful" -ForegroundColor Green
Write-Host ""

# Step 2: Copy firmware to Unraid
Write-Host "[2/5] Copying firmware to Unraid server..." -ForegroundColor Yellow
scp .pio\build\esp32dev\firmware.bin root@${UnraidIP}:/tmp/firmware.bin
if ($LASTEXITCODE -ne 0) {
    Write-Host "❌ SCP failed!" -ForegroundColor Red
    exit 1
}
Write-Host "✅ Firmware copied to Unraid" -ForegroundColor Green
Write-Host ""

# Step 3: Prepare device for OTA (via HTTP over WireGuard)
Write-Host "[3/5] Preparing device for OTA..." -ForegroundColor Yellow

# Call HTTP endpoint via Unraid server (device is on WireGuard VPN)
try {
    Write-Host "   Calling http://$DeviceIP/prepareota via Unraid..."
    $result = ssh root@$UnraidIP "curl -s -m 10 http://$DeviceIP/prepareota"
    if ($LASTEXITCODE -eq 0) {
        Write-Host "   Response: $result" -ForegroundColor Gray
        Write-Host "✅ Device prepared for OTA (via HTTP)" -ForegroundColor Green
    }
    else {
        throw "curl returned exit code $LASTEXITCODE"
    }
}
catch {
    Write-Host "⚠️  HTTP prepare failed: $_" -ForegroundColor Yellow
    Write-Host "   OTA onStart will attempt preparation automatically" -ForegroundColor Yellow
}
Write-Host ""

# Step 4: Wait a moment for device to settle
Write-Host "[4/5] Waiting for device to stabilize..." -ForegroundColor Yellow
Start-Sleep -Seconds 3
Write-Host "✅ Ready" -ForegroundColor Green
Write-Host ""

# Step 5: Perform OTA update via HTTP
Write-Host "[5/5] Starting HTTP OTA update via WireGuard..." -ForegroundColor Yellow
Write-Host "   Device: $DeviceIP"
Write-Host "   Via: $UnraidIP"
Write-Host ""

# Use HTTP OTA instead of espota - this avoids the flash erase crash
# The /update endpoint accepts multipart form upload
ssh root@$UnraidIP "curl -f -m 120 -F 'firmware=@/tmp/firmware.bin' http://$DeviceIP/update"

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "=====================================" -ForegroundColor Green
    Write-Host "✅ OTA UPDATE SUCCESSFUL!" -ForegroundColor Green
    Write-Host "=====================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Device should reboot with new firmware in a few seconds." -ForegroundColor Cyan
} else {
    Write-Host ""
    Write-Host "=====================================" -ForegroundColor Red
    Write-Host "❌ OTA UPDATE FAILED" -ForegroundColor Red
    Write-Host "=====================================" -ForegroundColor Red
    Write-Host ""
    Write-Host "Troubleshooting:" -ForegroundColor Yellow
    Write-Host "  1. Check serial output for specific error" -ForegroundColor Yellow
    Write-Host "  2. Try espota fallback: ssh root@$UnraidIP 'python3 /tmp/espota.py -i $DeviceIP -p 3232 -a $OTAPassword -f /tmp/firmware.bin'" -ForegroundColor Yellow
    Write-Host "  3. As last resort, use serial upload: pio run -t upload" -ForegroundColor Yellow
    exit 1
}
