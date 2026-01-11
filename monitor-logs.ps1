# Monitor logs from Bowie Phone via WireGuard/Unraid
# Connects to device telnet port through SSH tunnel

param(
    [string]$DeviceIP = "10.253.0.2",
    [string]$UnraidIP = "100.86.189.46",
    [int]$TelnetPort = 23
)

Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Bowie Phone Remote Logs" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Device: $DeviceIP (via $UnraidIP)" -ForegroundColor Gray
Write-Host "Press Ctrl+C to exit" -ForegroundColor Gray
Write-Host "" -ForegroundColor Cyan

# Connect via SSH to Unraid, then telnet to device
ssh root@$UnraidIP "telnet $DeviceIP $TelnetPort"
