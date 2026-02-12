<#
.SYNOPSIS
    Build and deploy ESP32 firmware via SSH to a remote machine (resilient version)
.DESCRIPTION
    This script builds PlatformIO firmware, uploads it and a deployment script to a 
    remote machine (Mac or Unraid), then kicks off the deployment. The remote script
    runs autonomously - if your connection drops, the deployment continues.
    
    Features:
    - Multiple fallback WiFi SSIDs (compile-time or deployment-time)
    - Resilient deployment - remote script continues even if connection drops
    - Serial or OTA flashing
    - Deployment-time WiFi configuration via config portal API
    
    Can be dot-sourced for individual functions:
        . .\deploy_via_ssh.ps1
        Build-Firmware -Environment bowie-phone-1
.PARAMETER Environment
    PlatformIO environment to build (default: bowie-phone-1)
.PARAMETER Target
    Deployment target: "mac" or "unraid" (default: mac)
.PARAMETER FlashMethod
    How to flash: "serial" or "ota" (default: serial)
.PARAMETER SerialPort
    Serial port on remote machine (default: /dev/cu.usbserial-0001 for mac)
.PARAMETER DeviceIp
    Device IP for OTA (default: read from platformio.ini WIREGUARD_LOCAL_IP)
.PARAMETER WifiSsid
    WiFi SSID to configure at deployment time (optional - uses compile-time defaults if not set)
.PARAMETER WifiPassword
    WiFi password for deployment-time configuration
.PARAMETER SkipBuild
    Skip the build step (use existing firmware)
.PARAMETER MonitorAfter
    Monitor serial output after flashing (serial mode only)
.PARAMETER NoWait
    Don't wait for deployment to complete (fire and forget)
.EXAMPLE
    .\deploy_via_ssh.ps1
    .\deploy_via_ssh.ps1 -Target unraid -FlashMethod ota
    .\deploy_via_ssh.ps1 -WifiSsid "MyNetwork" -WifiPassword "secret123"
    .\deploy_via_ssh.ps1 -NoWait  # Fire and forget
#>

[CmdletBinding()]
param(
    [string]$Environment = "bowie-phone-1",
    [ValidateSet("mac", "unraid")]
    [string]$Target = "mac",
    [ValidateSet("serial", "ota")]
    [string]$FlashMethod = "serial",
    [string]$SerialPort,
    [string]$DeviceIp,
    [string]$WifiSsid,
    [string]$WifiPassword,
    [switch]$SkipBuild,
    [switch]$MonitorAfter,
    [switch]$Clean,
    [switch]$NoWait
)

#region Configuration
$Script:Config = @{
    # Project paths
    ProjectRoot = Split-Path -Parent $PSScriptRoot
    PioExe = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
    
    # Remote targets  
    Targets = @{
        mac = @{
            Host = "jumper@100.111.120.5"
            EsptoolPath = "/tmp/esptool-macos-arm64/esptool"
            DefaultSerialPort = "/dev/cu.usbserial-0001"
            StagingDir = "/tmp/fw_upload"
            LogFile = "/tmp/fw_deploy.log"
            ApPassword = "ziggystardust"  # WIFI_AP_PASSWORD for bowie-phone
        }
        unraid = @{
            Host = "root@100.86.189.46"
            EsptoolPath = "esptool.py"
            DefaultSerialPort = "/dev/ttyUSB0"
            StagingDir = "/tmp/fw_upload"
            LogFile = "/tmp/fw_deploy.log"
            ApPassword = "ziggystardust"
        }
    }
    
    # Flash settings for ESP32
    FlashMode = "dio"
    FlashFreq = "40m"
    FlashSize = "4MB"
    BaudRate = 460800
    
    # Offsets
    BootloaderOffset = "0x1000"
    PartitionsOffset = "0x8000"
    FirmwareOffset = "0x10000"
    
    # Config portal settings
    ApName = "Bowie-Phone-Setup"
    ApIp = "192.168.4.1"
}
#endregion

#region Helper Functions
function Write-Step {
    param([int]$Step, [int]$Total, [string]$Message)
    Write-Host "[$Step/$Total] " -NoNewline -ForegroundColor Cyan
    Write-Host $Message
}

function Write-Success {
    param([string]$Message)
    Write-Host "✅ $Message" -ForegroundColor Green
}

function Write-Failure {
    param([string]$Message)
    Write-Host "❌ $Message" -ForegroundColor Red
}

function Write-Warning {
    param([string]$Message)
    Write-Host "⚠️  $Message" -ForegroundColor Yellow
}

function Get-BuildFlags {
    param([string]$Environment)
    $compileCommands = Join-Path $Script:Config.ProjectRoot "compile_commands.json"
    if (-not (Test-Path $compileCommands)) { return @{} }
    
    $content = Get-Content $compileCommands -Raw
    $flags = @{}
    
    # Extract key build flags
    if ($content -match 'DWIREGUARD_LOCAL_IP=\\"([^"]+)\\"') { $flags.DeviceIp = $matches[1] }
    if ($content -match 'DOTA_PORT=(\d+)') { $flags.OtaPort = $matches[1] }
    if ($content -match 'DOTA_PASSWORD=\\"([^"]+)\\"') { $flags.OtaPassword = $matches[1] }
    if ($content -match 'DFIRMWARE_VERSION=\\"([^"]+)\\"') { $flags.Version = $matches[1] }
    if ($content -match 'DDEFAULT_SSID=\\"([^"]+)\\"') { $flags.DefaultSsid = $matches[1] }
    if ($content -match 'DWIFI_AP_NAME=\\"([^"]+)\\"') { $flags.ApName = $matches[1] }
    if ($content -match 'DWIFI_AP_PASSWORD=\\"([^"]+)\\"') { $flags.ApPassword = $matches[1] }
    
    return $flags
}

function Invoke-SshCommand {
    param(
        [string]$TargetHost,
        [string]$Command,
        [switch]$Silent,
        [int]$Timeout = 60
    )
    
    if (-not $Silent) {
        Write-Verbose "SSH: $Command"
    }
    
    # Use PowerShell job with timeout to prevent hanging
    $job = Start-Job -ScriptBlock {
        param($sshHost, $cmd)
        $output = & ssh -o ConnectTimeout=10 `
                       -o ServerAliveInterval=5 `
                       -o ServerAliveCountMax=3 `
                       -o BatchMode=yes `
                       $sshHost $cmd 2>&1
        return @{
            Output = $output
            ExitCode = $LASTEXITCODE
        }
    } -ArgumentList $TargetHost, $Command
    
    # Wait for job with timeout
    $completed = Wait-Job -Job $job -Timeout $Timeout
    
    if ($completed) {
        $jobResult = Receive-Job -Job $job
        Remove-Job -Job $job -Force
        
        return @{
            Output = $jobResult.Output
            ExitCode = $jobResult.ExitCode
            Success = ($jobResult.ExitCode -eq 0)
        }
    } else {
        # Timeout occurred
        Stop-Job -Job $job
        Remove-Job -Job $job -Force
        
        if (-not $Silent) {
            Write-Warning "SSH command timed out after $Timeout seconds"
        }
        
        return @{
            Output = @("Command timed out after $Timeout seconds")
            ExitCode = 124  # Standard timeout exit code
            Success = $false
        }
    }
}

function Test-SshConnection {
    param([string]$TargetHost)
    $result = Invoke-SshCommand -TargetHost $TargetHost -Command "echo OK" -Silent -Timeout 10
    return $result.Success
}

function Ensure-RemoteEsptool {
    <#
    .SYNOPSIS
        Ensures esptool is installed on the remote machine
    .DESCRIPTION
        Checks if esptool exists at the configured path. If not, downloads and
        installs the standalone binary from GitHub releases.
    #>
    param(
        [ValidateSet("mac", "unraid")]
        [string]$Target = "mac"
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    $esptoolPath = $targetConfig.EsptoolPath
    
    Write-Host "   Checking esptool on $Target..." -ForegroundColor DarkGray
    
    # Check if esptool exists (short timeout for simple check)
    $checkResult = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "test -f '$esptoolPath' && echo EXISTS" -Silent -Timeout 10
    
    if ($checkResult.Output -match "EXISTS") {
        Write-Host "   esptool found at $esptoolPath" -ForegroundColor DarkGray
        return $true
    }
    
    Write-Host "   esptool not found, downloading..." -ForegroundColor Yellow
    
    # Upload and run the install script from tools/
    $scriptFile = Join-Path $PSScriptRoot "install_esptool.sh"
    
    if (-not (Test-Path $scriptFile)) {
        Write-Failure "install_esptool.sh not found in tools directory"
        return $false
    }
    
    & scp $scriptFile "$($targetConfig.Host):/tmp/install_esptool.sh" 2>&1 | Out-Null
    $installResult = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "chmod +x /tmp/install_esptool.sh && /tmp/install_esptool.sh" -Silent
    
    if (-not $installResult.Success) {
        Write-Failure "Failed to install esptool"
        Write-Host "   Error: $($installResult.Output)" -ForegroundColor Red
        return $false
    }
    
    # Verify installation
    $verifyResult = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "test -f '$esptoolPath' && '$esptoolPath' version 2>/dev/null | head -1" -Silent
    if ($verifyResult.Success) {
        Write-Success "esptool installed: $($verifyResult.Output)"
        return $true
    }
    
    Write-Failure "esptool installation could not be verified"
    return $false
}

function Clear-SerialPort {
    <#
    .SYNOPSIS
        Kills any processes using the serial port
    .DESCRIPTION
        Checks for processes (screen, picocom, minicom, etc.) using the serial port
        and kills them to free the port for flashing.
    #>
    param(
        [ValidateSet("mac", "unraid")]
        [string]$Target = "mac",
        [string]$SerialPort
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    if (-not $SerialPort) {
        $SerialPort = $targetConfig.DefaultSerialPort
    }
    
    Write-Host "   Checking if port $SerialPort is in use..." -ForegroundColor DarkGray
    
    # Check if port is busy (short timeout for lsof check)
    $checkCmd = "lsof '$SerialPort' 2>/dev/null | grep -v '^COMMAND' | awk '{print `$2}' | head -5"
    $checkResult = Invoke-SshCommand -TargetHost $targetConfig.Host -Command $checkCmd -Silent -Timeout 10
    
    if (-not $checkResult.Output -or $checkResult.Output.Length -eq 0) {
        Write-Host "   Port is free" -ForegroundColor DarkGray
        return $true
    }
    
    $pids = $checkResult.Output -split "`n" | Where-Object { $_ -match '^\d+$' }
    if ($pids.Count -eq 0) {
        Write-Host "   Port is free" -ForegroundColor DarkGray
        return $true
    }
    
    Write-Warning "Port in use by PID(s): $($pids -join ', ')"
    Write-Host "   Killing processes..." -ForegroundColor DarkGray
    
    # Try multiple times to kill stubborn processes
    $maxRetries = 3
    for ($retry = 0; $retry -lt $maxRetries; $retry++) {
        foreach ($p in $pids) {
            $killCmd = "kill -9 $p 2>/dev/null || true"
            Invoke-SshCommand -TargetHost $targetConfig.Host -Command $killCmd -Silent | Out-Null
        }
        
        # Give processes time to die
        Start-Sleep -Milliseconds 1000
        
        # Verify port is now free
        $verifyResult = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "lsof '$SerialPort' 2>/dev/null | grep -v '^COMMAND'" -Silent -Timeout 10
        if (-not $verifyResult.Output -or $verifyResult.Output.Length -eq 0) {
            Write-Success "Port cleared"
            return $true
        }
        
        if ($retry -lt ($maxRetries - 1)) {
            Write-Host "   Retry $($retry + 1)/$maxRetries..." -ForegroundColor DarkGray
        }
    }
    
    # Last chance: try pkill for screen/minicom/etc
    Write-Host "   Trying pkill for serial monitors..." -ForegroundColor DarkGray
    $pkillCmds = @("screen", "minicom", "picocom", "cu")
    foreach ($prog in $pkillCmds) {
        $pkillCmd = "pkill -9 $prog 2>/dev/null || true"
        Invoke-SshCommand -TargetHost $targetConfig.Host -Command $pkillCmd -Silent | Out-Null
    }
    
    Start-Sleep -Milliseconds 1000
    
    # Final verification
    $finalResult = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "lsof '$SerialPort' 2>/dev/null | grep -v '^COMMAND'" -Silent -Timeout 10
    if (-not $finalResult.Output -or $finalResult.Output.Length -eq 0) {
        Write-Success "Port cleared"
        return $true
    }
    
    Write-Failure "Could not free port $SerialPort after $maxRetries attempts"
    Write-Host "   Run manually: ssh $($targetConfig.Host) 'pkill -9 screen; lsof $SerialPort | grep -v COMMAND | awk ''{print `$2}'' | xargs kill -9'" -ForegroundColor Yellow
    return $false
}
#endregion

#region Core Functions
function Build-Firmware {
    param(
        [string]$Environment = "bowie-phone-1",
        [switch]$Clean
    )
    
    $buildDir = Join-Path $Script:Config.ProjectRoot ".pio\build\$Environment"
    
    Write-Host "`n🔨 Building firmware for $Environment..." -ForegroundColor Cyan
    
    Push-Location $Script:Config.ProjectRoot
    try {
        if ($Clean) {
            Write-Host "   Cleaning..." -ForegroundColor DarkGray
            & $Script:Config.PioExe run -e $Environment -t clean 2>&1 | Out-Null
        }
        
        $output = & $Script:Config.PioExe run -e $Environment 2>&1
        
        # Save full output to log file
        $logFile = Join-Path $Script:Config.ProjectRoot "build.log"
        $output | Out-File -FilePath $logFile -Encoding UTF8
        
        if ($LASTEXITCODE -ne 0) {
            Write-Failure "Build failed!"
            Write-Host ""
            
            # Show error lines
            $errorLines = $output | Where-Object { $_ -match "error:|Error:|ERROR|undefined|fatal|In file|required from" }
            if ($errorLines) {
                Write-Host "   Build errors:" -ForegroundColor Red
                $errorLines | Select-Object -First 20 | ForEach-Object { 
                    Write-Host "   $_" -ForegroundColor Red 
                }
            }
            
            # Show last 15 lines for context
            Write-Host ""
            Write-Host "   Last 15 lines of output:" -ForegroundColor Yellow
            $output | Select-Object -Last 15 | ForEach-Object {
                Write-Host "   $_" -ForegroundColor DarkGray
            }
            
            Write-Host ""
            Write-Host "   Full log: $logFile" -ForegroundColor DarkGray
            return $false
        }
        
        # Verify outputs
        $firmware = Join-Path $buildDir "firmware.bin"
        
        if (-not (Test-Path $firmware)) {
            Write-Failure "firmware.bin not found"
            return $false
        }
        
        $fwSize = [math]::Round((Get-Item $firmware).Length / 1KB, 1)
        Write-Success "Build complete ($fwSize KB)"
        
        # Show key config
        $flags = Get-BuildFlags -Environment $Environment
        if ($flags.DefaultSsid) {
            Write-Host "   WiFi SSID: $($flags.DefaultSsid)" -ForegroundColor DarkGray
        }
        if ($flags.Version) {
            Write-Host "   Version: $($flags.Version)" -ForegroundColor DarkGray
        }
        
        return $true
    } finally {
        Pop-Location
    }
}

function New-RemoteDeployScript {
    <#
    .SYNOPSIS
        Generate the bash script that runs on the remote machine
    #>
    param(
        [string]$Target,
        [string]$SerialPort,
        [string]$WifiSsid,
        [string]$WifiPassword,
        [switch]$ConfigureWifi,
        [switch]$MonitorAfter
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    $port = if ($SerialPort) { $SerialPort } else { $targetConfig.DefaultSerialPort }
    $staging = $targetConfig.StagingDir
    $esptool = $targetConfig.EsptoolPath
    $logFile = $targetConfig.LogFile
    $apName = $Script:Config.ApName
    $apIp = $Script:Config.ApIp
    $apPassword = $targetConfig.ApPassword

    # Build the flash command
    $flashCmd = "$esptool --chip esp32 --port $port --baud $($Script:Config.BaudRate) --before default_reset --after hard_reset write_flash -z --flash_mode $($Script:Config.FlashMode) --flash_freq $($Script:Config.FlashFreq) --flash_size detect $($Script:Config.BootloaderOffset) $staging/bootloader.bin $($Script:Config.PartitionsOffset) $staging/partitions.bin $($Script:Config.FirmwareOffset) $staging/firmware.bin"
    
    # WiFi configuration section (if specified)
    $wifiConfigScript = ""
    if ($ConfigureWifi -and $WifiSsid) {
        $wifiConfigScript = @"

# ============================================================================
# STEP 3: Configure WiFi via config portal API
# ============================================================================
log "STEP 3: Configuring WiFi credentials"

# Wait for device to boot and start config portal
log "Waiting 20 seconds for device to boot into config portal..."
sleep 20

# Get current WiFi interface
ORIGINAL_WIFI=`$(networksetup -getairportnetwork en0 2>/dev/null | awk -F': ' '{print `$2}')
log "Current WiFi: `$ORIGINAL_WIFI"

# Function to restore original WiFi (called on exit)
restore_wifi() {
    if [ -n "`$ORIGINAL_WIFI" ]; then
        log "Restoring WiFi to: `$ORIGINAL_WIFI"
        networksetup -setairportnetwork en0 "`$ORIGINAL_WIFI" 2>/dev/null || true
    fi
}
trap restore_wifi EXIT

# Connect to device's AP
log "Connecting to device AP: $apName"
for attempt in 1 2 3; do
    networksetup -setairportnetwork en0 "$apName" "$apPassword" 2>/dev/null
    sleep 5
    CURRENT=`$(networksetup -getairportnetwork en0 2>/dev/null | awk -F': ' '{print `$2}')
    if [ "`$CURRENT" = "$apName" ]; then
        log "Connected to device AP"
        break
    fi
    log "Retry `$attempt/3..."
done

# Verify connection
CURRENT=`$(networksetup -getairportnetwork en0 2>/dev/null | awk -F': ' '{print `$2}')
if [ "`$CURRENT" != "$apName" ]; then
    log "ERROR: Could not connect to device AP"
    echo "RESULT:WIFI_CONNECT_FAILED"
    exit 1
fi

# Configure WiFi via API
log "Sending WiFi credentials to device..."
RESPONSE=`$(curl -s -m 10 -X POST -d "ssid=$WifiSsid&password=$WifiPassword" "http://$apIp/api/wifi" 2>&1)
log "Device response: `$RESPONSE"

if echo "`$RESPONSE" | grep -q '"ok":true'; then
    log "WiFi credentials saved successfully!"
else
    log "WARNING: WiFi config response unexpected: `$RESPONSE"
fi

# Device will reboot - restore our WiFi
log "Device rebooting with new credentials..."
restore_wifi

"@
    }
    
    # Monitoring section - simplified since PowerShell handles the live tail
    $monitorScript = ""
    if ($MonitorAfter) {
        $monitorScript = @"

# ============================================================================
# STEP 4: Brief boot capture (full monitoring handled by PowerShell)
# ============================================================================
log "STEP 4: Capturing initial boot"

# Wait for device to reboot
sleep 3

# Configure serial
stty -f $port 115200 raw -echo 2>/dev/null || stty -F $port 115200 raw -echo 2>/dev/null || true

# Reset device
$esptool --port $port --after hard_reset run 2>/dev/null || true
sleep 2

# Capture brief boot output (5 seconds)
log "--- BOOT LOG START ---"
(cat $port 2>/dev/null & CATPID=`$!; sleep 5; kill `$CATPID 2>/dev/null) || true
log "--- BOOT LOG END ---"

"@
    }
    
    # Full script
    $script = @"
#!/bin/bash
# ============================================================================
# ESP32 Firmware Deployment Script (Autonomous)
# Generated: $(Get-Date -Format "yyyy-MM-dd HH:mm:ss")
# ============================================================================
# This script runs autonomously on the remote machine.
# Even if the originating connection drops, deployment continues.
# Logs are written to: $logFile
# ============================================================================

set -e

LOGFILE="$logFile"
STAGING="$staging"
PORT="$port"

# Logging function
log() {
    echo "`$(date '+%Y-%m-%d %H:%M:%S') | `$1" | tee -a "`$LOGFILE"
}

# Start fresh log
echo "============================================================================" > "`$LOGFILE"
log "ESP32 Deployment Starting"
log "============================================================================"

# ============================================================================
# STEP 1: Verify files
# ============================================================================
log "STEP 1: Verifying firmware files"

for file in firmware.bin bootloader.bin partitions.bin; do
    if [ ! -f "`$STAGING/`$file" ]; then
        log "ERROR: Missing file: `$STAGING/`$file"
        echo "RESULT:MISSING_FILES"
        exit 1
    fi
    SIZE=`$(ls -l "`$STAGING/`$file" | awk '{print `$5}')
    log "  `$file: `$SIZE bytes"
done

# ============================================================================
# STEP 2: Flash firmware via serial
# ============================================================================
log "STEP 2: Flashing firmware"

# Release serial port
pkill -f "cat $port" 2>/dev/null || true
pkill screen 2>/dev/null || true
sleep 2

log "Running esptool flash command..."
log "Command: $flashCmd"

if $flashCmd >> "`$LOGFILE" 2>&1; then
    log "Flash completed successfully!"
else
    log "ERROR: Flash failed!"
    echo "RESULT:FLASH_FAILED"
    exit 1
fi
$wifiConfigScript
$monitorScript
# ============================================================================
# DONE
# ============================================================================
log "============================================================================"
log "Deployment completed successfully!"
log "============================================================================"
echo "RESULT:SUCCESS"
"@
    
    return $script
}

function Copy-FirmwareToRemote {
    param(
        [ValidateSet("mac", "unraid")]
        [string]$Target = "mac",
        [string]$Environment = "bowie-phone-1"
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    $buildDir = Join-Path $Script:Config.ProjectRoot ".pio\build\$Environment"
    
    Write-Host "`n📤 Uploading firmware to $Target ($($targetConfig.Host))..." -ForegroundColor Cyan
    
    # Create staging directory
    $result = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "mkdir -p $($targetConfig.StagingDir)" -Silent
    if (-not $result.Success) {
        Write-Failure "Failed to create staging directory"
        Write-Host "   Error: $($result.Output)" -ForegroundColor Red
        return $false
    }
    
    # Upload files
    $files = @(
        (Join-Path $buildDir "firmware.bin"),
        (Join-Path $buildDir "bootloader.bin"),
        (Join-Path $buildDir "partitions.bin")
    )
    
    foreach ($file in $files) {
        if (-not (Test-Path $file)) {
            Write-Failure "Missing: $file"
            return $false
        }
    }
    
    $scpArgs = $files + @("$($targetConfig.Host):$($targetConfig.StagingDir)/")
    & scp @scpArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Failure "SCP failed"
        return $false
    }
    
    Write-Success "Files uploaded to $($targetConfig.StagingDir)"
    return $true
}

function Watch-SerialOutput {
    <#
    .SYNOPSIS
        Monitor serial output from ESP32 device
    .DESCRIPTION
        Connects to the remote machine and opens a screen session on the serial port.
        Press Ctrl+A then K to kill the screen session, or Ctrl+C to disconnect.
    #>
    param(
        [ValidateSet("mac", "unraid")]
        [string]$Target = "mac",
        [string]$SerialPort,
        [int]$BaudRate = 115200,
        [switch]$NoReset
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    $port = if ($SerialPort) { $SerialPort } else { $targetConfig.DefaultSerialPort }
    $esptool = $targetConfig.EsptoolPath
    
    Write-Host ""
    Write-Host "📡 Monitoring serial output from $port" -ForegroundColor Yellow
    Write-Host "   Target: $($targetConfig.Host)" -ForegroundColor DarkGray
    Write-Host "   Baud: $BaudRate" -ForegroundColor DarkGray
    Write-Host "   Exit: Ctrl+A then K (kill screen), or Ctrl+C" -ForegroundColor DarkGray
    Write-Host "   Scroll: Ctrl+A then Esc (copy mode), then arrows/PgUp/PgDn, Esc to exit" -ForegroundColor DarkGray
    Write-Host ""
    
    # Kill any existing processes on the port first
    Clear-SerialPort -Target $Target -SerialPort $port | Out-Null
    Start-Sleep -Milliseconds 500
    
    if (-not $NoReset) {
        Write-Host "   Resetting ESP32..." -ForegroundColor DarkGray
        $resetCmd = "$esptool --port $port --after hard_reset run 2>/dev/null"
        Invoke-SshCommand -TargetHost $targetConfig.Host -Command $resetCmd -Silent -Timeout 15 | Out-Null
        Start-Sleep -Seconds 1
        Write-Host ""
    }
    
    # Use screen for proper serial monitoring
    & ssh -t $targetConfig.Host "screen $port $BaudRate"
}

function Set-DeviceVpnConfig {
    <#
    .SYNOPSIS
        Configure VPN (WireGuard/Tailscale) settings on a device via its config portal
    .DESCRIPTION
        POSTs VPN configuration to the device's /vpn/save endpoint.
        Device must be accessible at the specified IP (usually on the AP network 192.168.4.1
        or on local network if already connected to WiFi).
    .PARAMETER DeviceIp
        IP address of the device (default: 192.168.4.1 for config portal)
    .PARAMETER LocalIp
        WireGuard local IP (e.g., "10.253.0.2")
    .PARAMETER PrivateKey
        WireGuard private key (base64)
    .PARAMETER PeerEndpoint
        WireGuard peer endpoint hostname or IP
    .PARAMETER PeerPublicKey
        WireGuard peer public key (base64)
    .PARAMETER PeerPort
        WireGuard peer port (default: 51820)
    .EXAMPLE
        Set-DeviceVpnConfig -DeviceIp "192.168.6.128" -LocalIp "10.253.0.2" `
            -PrivateKey "oKQ..." -PeerEndpoint "jumprouter.infinitebutts.com" `
            -PeerPublicKey "JLG..." -PeerPort 51820
    #>
    param(
        [string]$DeviceIp = "192.168.4.1",
        [Parameter(Mandatory)]
        [string]$LocalIp,
        [Parameter(Mandatory)]
        [string]$PrivateKey,
        [Parameter(Mandatory)]
        [string]$PeerEndpoint,
        [Parameter(Mandatory)]
        [string]$PeerPublicKey,
        [int]$PeerPort = 51820
    )
    
    Write-Host "`n🔐 Configuring VPN on device at $DeviceIp..." -ForegroundColor Cyan
    
    # Build form data
    $formData = @{
        localIp = $LocalIp
        privateKey = $PrivateKey
        peerEndpoint = $PeerEndpoint
        peerPublicKey = $PeerPublicKey
        peerPort = $PeerPort
    }
    
    $url = "http://$DeviceIp/vpn/save"
    
    try {
        Write-Host "   Sending VPN configuration..." -ForegroundColor DarkGray
        
        $response = Invoke-WebRequest -Uri $url -Method POST -Body $formData -TimeoutSec 10
        
        if ($response.StatusCode -eq 200 -or $response.StatusCode -eq 302) {
            Write-Success "VPN configuration saved!"
            Write-Host "   Local IP: $LocalIp" -ForegroundColor DarkGray
            Write-Host "   Peer: $PeerEndpoint`:$PeerPort" -ForegroundColor DarkGray
            return $true
        } else {
            Write-Failure "Unexpected response: $($response.StatusCode)"
            return $false
        }
    }
    catch {
        Write-Failure "Failed to configure VPN: $_"
        Write-Host "   Make sure device is accessible at $DeviceIp" -ForegroundColor Yellow
        Write-Host "   Try: curl http://$DeviceIp/" -ForegroundColor Yellow
        return $false
    }
}

function Start-RemoteDeployment {
    param(
        [ValidateSet("mac", "unraid")]
        [string]$Target = "mac",
        [string]$SerialPort,
        [string]$WifiSsid,
        [string]$WifiPassword,
        [switch]$MonitorAfter,
        [switch]$NoWait
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    
    Write-Host "`n🚀 Starting remote deployment..." -ForegroundColor Cyan
    
    # Generate the deployment script
    $configureWifi = $WifiSsid -and $WifiSsid.Length -gt 0
    $script = New-RemoteDeployScript -Target $Target -SerialPort $SerialPort `
        -WifiSsid $WifiSsid -WifiPassword $WifiPassword `
        -ConfigureWifi:$configureWifi -MonitorAfter:$MonitorAfter
    
    # Upload the script
    $scriptPath = Join-Path $env:TEMP "deploy_esp32.sh"
    $script | Set-Content -Path $scriptPath -Encoding UTF8 -NoNewline
    
    # Convert line endings for Unix
    $content = Get-Content $scriptPath -Raw
    $content = $content -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($scriptPath, $content)
    
    Write-Host "   Uploading deployment script..." -ForegroundColor DarkGray
    & scp $scriptPath "$($targetConfig.Host):$($targetConfig.StagingDir)/deploy.sh"
    if ($LASTEXITCODE -ne 0) {
        Write-Failure "Failed to upload deployment script"
        return $false
    }
    
    # Make it executable and run it
    $setupCmd = "chmod +x $($targetConfig.StagingDir)/deploy.sh"
    Invoke-SshCommand -TargetHost $targetConfig.Host -Command $setupCmd -Silent | Out-Null
    
    if ($NoWait) {
        # Fire and forget - run in background with nohup
        Write-Host "   Starting deployment (fire and forget)..." -ForegroundColor DarkGray
        $runCmd = "nohup $($targetConfig.StagingDir)/deploy.sh > /dev/null 2>&1 &"
        Invoke-SshCommand -TargetHost $targetConfig.Host -Command $runCmd -Silent | Out-Null
        
        Write-Success "Deployment started in background!"
        Write-Host "   Check logs on remote: cat $($targetConfig.LogFile)" -ForegroundColor DarkGray
        return $true
    } else {
        # Stream deployment output in real-time using ssh -t for PTY
        Write-Host "   Running deployment..." -ForegroundColor DarkGray
        Write-Host ""
        
        $port = if ($SerialPort) { $SerialPort } else { $targetConfig.DefaultSerialPort }
        $runCmd = "$($targetConfig.StagingDir)/deploy.sh"
        
        # Run with real-time output streaming
        $success = $false
        & ssh -t $targetConfig.Host $runCmd 2>&1 | ForEach-Object {
            $line = $_
            if ($line -match "RESULT:SUCCESS") {
                $success = $true
            } elseif ($line -match "RESULT:(.+)") {
                # Don't show other RESULT lines
            } elseif ($line -match "^\d{4}-\d{2}-\d{2}") {
                # Timestamp lines from log
                Write-Host "   $line" -ForegroundColor DarkGray
            } elseif ($line -match "STEP|ERROR|SUCCESS|completed|Flashing|Flash") {
                Write-Host "   $line" -ForegroundColor Cyan
            } else {
                Write-Host "   $line" -ForegroundColor DarkGray
            }
        }
        
        if (-not $success) {
            Write-Failure "Deployment failed - check remote log: $($targetConfig.LogFile)"
            return $false
        }
        
        # If MonitorAfter, tail the serial output
        if ($MonitorAfter) {
            Watch-SerialOutput -Target $Target -SerialPort $SerialPort
        }
        
        return $true
    }
}

function Deploy-ViaSsh {
    param(
        [string]$Environment = "bowie-phone-1",
        [ValidateSet("mac", "unraid")]
        [string]$Target = "mac",
        [ValidateSet("serial", "ota")]
        [string]$FlashMethod = "serial",
        [string]$SerialPort,
        [string]$DeviceIp,
        [string]$WifiSsid,
        [string]$WifiPassword,
        [switch]$SkipBuild,
        [switch]$MonitorAfter,
        [switch]$Clean,
        [switch]$NoWait
    )
    
    $totalSteps = if ($SkipBuild) { 2 } else { 3 }
    $step = 0
    
    Write-Host "`n" + ("=" * 55) -ForegroundColor Cyan
    Write-Host "🚀 ESP32 Deployment via SSH (Resilient)" -ForegroundColor Cyan
    Write-Host ("=" * 55) -ForegroundColor Cyan
    Write-Host "   Environment: $Environment"
    Write-Host "   Target: $Target"
    Write-Host "   Method: $FlashMethod"
    if ($WifiSsid) {
        Write-Host "   Deploy-time WiFi: $WifiSsid"
    }
    if ($NoWait) {
        Write-Host "   Mode: Fire and forget" -ForegroundColor Yellow
    }
    Write-Host ""
    
    # Test SSH connection
    $targetConfig = $Script:Config.Targets[$Target]
    Write-Host "🔗 Testing SSH connection to $($targetConfig.Host)..." -ForegroundColor DarkGray
    if (-not (Test-SshConnection -TargetHost $targetConfig.Host)) {
        Write-Failure "Cannot connect to $($targetConfig.Host)"
        Write-Host "   Run: ssh-copy-id $($targetConfig.Host)" -ForegroundColor Yellow
        return $false
    }
    Write-Success "SSH connected"
    
    # Ensure esptool is installed on remote
    if (-not (Ensure-RemoteEsptool -Target $Target)) {
        return $false
    }
    
    # Step 1: Build
    if (-not $SkipBuild) {
        $step++
        Write-Step $step $totalSteps "Building firmware"
        if (-not (Build-Firmware -Environment $Environment -Clean:$Clean)) {
            return $false
        }
    }
    
    # Step 2: Upload files
    $step++
    Write-Step $step $totalSteps "Uploading to $Target"
    if (-not (Copy-FirmwareToRemote -Target $Target -Environment $Environment)) {
        return $false
    }
    
    # Clear serial port if using serial flash
    if ($FlashMethod -eq "serial") {
        $port = if ($SerialPort) { $SerialPort } else { $targetConfig.DefaultSerialPort }
        if (-not (Clear-SerialPort -Target $Target -SerialPort $port)) {
            return $false
        }
    }
    
    # Step 3: Run deployment
    $step++
    Write-Step $step $totalSteps "Running remote deployment"
    $success = Start-RemoteDeployment -Target $Target -SerialPort $SerialPort `
        -WifiSsid $WifiSsid -WifiPassword $WifiPassword `
        -MonitorAfter:$MonitorAfter -NoWait:$NoWait
    
    if (-not $success) {
        return $false
    }
    
    Write-Host "`n" + ("=" * 55) -ForegroundColor Green
    Write-Success "Deployment complete!"
    if (-not $NoWait) {
        Write-Host "   Remote log: $($targetConfig.LogFile)" -ForegroundColor DarkGray
    }
    Write-Host ("=" * 55) -ForegroundColor Green
    
    return $true
}

function Get-RemoteDeployLog {
    <#
    .SYNOPSIS
        Retrieve deployment log from remote machine
    #>
    param(
        [ValidateSet("mac", "unraid")]
        [string]$Target = "mac",
        [switch]$Follow
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    if($Follow) {
        $Cmd="tail -f"
    } else {
        $Cmd="cat"
    }
    $result = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "$Cmd $($targetConfig.LogFile) 2>/dev/null"
    
    if ($result.Success) {
        return $result.Output
    }
    return $null
}
#endregion

#region Main Execution
# Only run main if called directly (not dot-sourced)
if ($MyInvocation.InvocationName -ne '.') {
    $result = Deploy-ViaSsh `
        -Environment $Environment `
        -Target $Target `
        -FlashMethod $FlashMethod `
        -SerialPort $SerialPort `
        -DeviceIp $DeviceIp `
        -WifiSsid $WifiSsid `
        -WifiPassword $WifiPassword `
        -SkipBuild:$SkipBuild `
        -MonitorAfter:$MonitorAfter `
        -Clean:$Clean `
        -NoWait:$NoWait
    
    if (-not $result) {
        exit 1
    }
}
else {
    echo "$PSScriptroot\README.md"
    cat "$PSScriptroot\README.md"
}
#endregion
