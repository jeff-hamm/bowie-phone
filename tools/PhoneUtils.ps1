#region Configuration
$Script:Config = @{
    # Project paths
    ProjectRoot = Split-Path -Parent $PSScriptRoot
    PioExe = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
    
    # Remote targets  
    Targets = @{
        mac = @{
            Host = "jumper@100.111.120.5"
            EsptoolPath = "~/.local/opt/bin/esptool"
            DefaultSerialPort = "/dev/cu.usbserial-0001"
            StagingDir = "/tmp/fw_upload"
            LogFile = "/tmp/fw_deploy.log"
            ApPassword = "ziggystardust"  # WIFI_AP_PASSWORD for bowie-phone
        }
        unraid = @{
            Host = "root@192.168.1.216"
            EsptoolPath = "~/.local/opt/bin/esptool"
            DefaultSerialPort = "/dev/ttyUSB0"
            StagingDir = "/tmp/fw_upload"
            LogFile = "/tmp/fw_deploy.log"
            ApPassword = "ziggystardust"
            DefaultOtaDeviceIp = "10.253.0.2"  # bowie-phone-1 WireGuard IP; unraid can reach it directly
            OtaPort = "3232"
            OtaPassword = "photo-ota-2000"
            DeviceId = "bowie-phone-1"        # Used by Monitor-SerialOutput / Watch-RemoteLogs
            TelnetLogs = $true                 # Stream logs from server file via SSH tail
        }
        'bowie-phone' = @{
            Host = "root@192.168.1.216"   # SSH via unraid; unraid can reach the device over WireGuard
            EsptoolPath = "~/.local/opt/bin/esptool"
            StagingDir = "/tmp/fw_upload"
            LogFile = "/tmp/fw_deploy.log"
            ApPassword = "ziggystardust"
            DefaultOtaDeviceIp = "10.253.0.2"
            OtaPort = "3232"
            OtaPassword = "photo-ota-2000"
            DeviceId = "bowie-phone-1"        # Used by Monitor-SerialOutput / Watch-RemoteLogs
            DefaultFlashMethod = "ota"         # No serial cable; always deploy via OTA
            TelnetLogs = $true                 # Stream logs from server file via SSH tail
        }
        local = @{
            Host = $null  # Local machine, no SSH
            EsptoolPath = "esptool"  # Assumes esptool is in PATH or will use python -m esptool
            DefaultSerialPort = "COM3"
            StagingDir = $null  # Not needed for local
            LogFile = "$env:TEMP\fw_deploy.log"
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
    
    # Remote log file storage (written by log server on unraid)
    RemoteLogBasePath = "/mnt/pool/appdata/phone-receiver/logs"

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

function Test-IsLocalTarget {
    param([string]$Target)
    return $Target -eq "local"
}

function Get-BuildFlags {
    param([string]$BuildEnvironment)
    $compileCommands = Join-Path $Script:Config.ProjectRoot "compile_commands.json"
    if (-not (Test-Path $compileCommands)) { return @{} }
    
    $content = Get-Content $compileCommands -Raw
    $flags = @{}
    
    # Extract key build flags
    # In compile_commands.json, strings are escaped as: \\\"value\\\"
    # After reading, this becomes: \"value\" in the string
    if ($content -match 'DWIREGUARD_LOCAL_IP=\\+"([^\\]+)\\+"') { $flags.DeviceIp = $matches[1] }
    if ($content -match 'DOTA_PORT=(\d+)') { $flags.OtaPort = $matches[1] }
    if ($content -match 'DOTA_PASSWORD=\\+"([^\\]+)\\+"') { $flags.OtaPassword = $matches[1] }
    if ($content -match 'DFIRMWARE_VERSION=\\+"([^\\]+)\\+"') { $flags.Version = $matches[1] }
    if ($content -match 'DDEFAULT_SSID=\\+"([^\\]+)\\+"') { $flags.DefaultSsid = $matches[1] }
    if ($content -match 'DWIFI_AP_NAME=\\+"([^\\]+)\\+"') { $flags.ApName = $matches[1] }
    if ($content -match 'DWIFI_AP_PASSWORD=\\+"([^\\]+)\\+"') { $flags.ApPassword = $matches[1] }
    
    return $flags
}

function Invoke-SshCommand {
    param(
        [string]$TargetHost,
        [string]$Command,
        [switch]$Silent,
        [int]$Timeout = 60
    )
    
    Write-Debug "SSH: $Command"
    
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

function Ensure-RemoteDependencies {
    <#
    .SYNOPSIS
        Ensures required tools are installed on the remote machine for flashing
    .DESCRIPTION
        For serial: Checks if esptool exists at the configured path. If not, installs it via pip.
        For OTA: Ensures Python3 and espota.py are available.
        For local: Checks if esptool is available on Windows PATH or via Python.
    #>
    param(
        [ValidateSet("mac", "unraid", "bowie-phone", "local")]
        [string]$Target = "mac",
        [ValidateSet("serial", "ota")]
        [string]$FlashMethod = "serial"
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    
    # Handle local Windows target
    if (Test-IsLocalTarget -Target $Target) {
        if ($FlashMethod -eq "ota") {
            Write-Host "   Local OTA mode - no dependencies to check" -ForegroundColor DarkGray
            return @{ Success = $true; PythonCmd = "curl" }
        }
        
        # Serial mode - check for esptool locally
        Write-Host "   Checking esptool on local machine..." -ForegroundColor DarkGray
        
        # Try esptool.exe or esptool.py in PATH
        $esptoolFound = $false
        $esptoolCmd = $null
        
        # Check if esptool.exe exists in PATH
        $esptoolExe = Get-Command "esptool.exe" -ErrorAction SilentlyContinue
        if ($esptoolExe) {
            Write-Host "   esptool.exe found: $($esptoolExe.Source)" -ForegroundColor DarkGray
            return @{ Success = $true; EsptoolCmd = "esptool.exe" }
        }
        
        # Check if esptool.py exists in PATH
        $esptoolPy = Get-Command "esptool.py" -ErrorAction SilentlyContinue
        if ($esptoolPy) {
            Write-Host "   esptool.py found: $($esptoolPy.Source)" -ForegroundColor DarkGray
            return @{ Success = $true; EsptoolCmd = "esptool.py" }
        }
        
        # Check if Python is available and can import esptool
        Write-Host "   Checking for Python module esptool..." -ForegroundColor DarkGray
        $pythonCheck = & python -c "import esptool; print('OK')" 2>$null
        if ($LASTEXITCODE -eq 0 -and $pythonCheck -match "OK") {
            Write-Host "   Python module esptool found" -ForegroundColor DarkGray
            return @{ Success = $true; EsptoolCmd = "python -m esptool" }
        }
        
        # esptool not found - try to install via pip
        Write-Host "   esptool not found, attempting to install via pip..." -ForegroundColor Yellow
        
        # Check if pip is available
        $pipCheck = Get-Command "pip" -ErrorAction SilentlyContinue
        if (-not $pipCheck) {
            Write-Failure "pip not found - cannot install esptool"
            Write-Host "   Install Python and pip first: https://www.python.org/downloads/" -ForegroundColor Yellow
            return $false
        }
        
        Write-Host "   Running: pip install esptool..." -ForegroundColor DarkGray
        $installOutput = & pip install esptool 2>&1
        
        if ($LASTEXITCODE -eq 0) {
            Write-Success "esptool installed successfully"
            
            # Verify installation
            $verifyCheck = & python -c "import esptool; print(esptool.__version__)" 2>$null
            if ($LASTEXITCODE -eq 0) {
                Write-Host "   esptool version: $verifyCheck" -ForegroundColor DarkGray
                return @{ Success = $true; EsptoolCmd = "python -m esptool" }
            }
        }
        
        Write-Failure "Failed to install esptool"
        Write-Host "   Try manually: pip install esptool" -ForegroundColor Yellow
        return $false
    }
    
    # Remote target - use SSH commands
    if (-not (Test-SshConnection -TargetHost $targetConfig.Host)) {
        Write-Failure "Cannot reach $Target host over SSH: $($targetConfig.Host)"
        Write-Host "   Verify network/Tailscale and SSH access, then retry." -ForegroundColor Yellow
        return $false
    }

    if ($FlashMethod -eq "ota") {
        # OTA mode - uses HTTP OTA (curl POST to /update endpoint)
        # This works reliably over WireGuard since it uses TCP, unlike espota.py which uses UDP
        Write-Host "   Checking OTA dependencies on $Target..." -ForegroundColor DarkGray
        
        # Check curl availability (should always be present)
        $curlCheck = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "command -v curl >/dev/null 2>&1 && echo FOUND || echo MISSING" -Silent -Timeout 10
        
        if ($curlCheck.Output -notmatch "FOUND") {
            Write-Failure "curl not found on $Target"
            Write-Host "   Install with: apt install curl (Ubuntu/Debian)" -ForegroundColor Yellow
            return $false
        }
        
        Write-Host "   curl found - will use HTTP OTA upload" -ForegroundColor DarkGray
        return @{ Success = $true; PythonCmd = "curl" }
    }
    
    # Serial mode - check esptool
    $esptoolPath = $targetConfig.EsptoolPath
    
    Write-Host "   Checking esptool on $Target..." -ForegroundColor DarkGray
    
    # Check if esptool exists at configured path (short timeout for simple check)
    $checkResult = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "test -f '$esptoolPath' && echo EXISTS" -Silent -Timeout 10
    
    if ($checkResult.Output -match "EXISTS") {
        Write-Host "   esptool found at $esptoolPath" -ForegroundColor DarkGray
        return $true
    }

    # Fallback: PlatformIO virtualenv often already contains esptool on remote hosts.
    $pioEsptoolPaths = @(
        "~/.platformio/penv/bin/esptool",
        "~/.platformio/penv/bin/esptool.py"
    )
    foreach ($pioPath in $pioEsptoolPaths) {
        $pioCheck = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "test -f '$pioPath' && echo EXISTS" -Silent -Timeout 10
        if ($pioCheck.Output -match "EXISTS") {
            Write-Host "   Found PlatformIO esptool at $pioPath" -ForegroundColor DarkGray
            $Script:Config.Targets[$Target].EsptoolPath = $pioPath
            return $true
        }
    }

    # Fallback: use any esptool already available in PATH.
    $pathEsptoolCheck = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "command -v esptool >/dev/null 2>&1 && command -v esptool" -Silent -Timeout 10
    if ($pathEsptoolCheck.Success -and $pathEsptoolCheck.Output) {
        $pathEsptool = ($pathEsptoolCheck.Output -join " ").Trim()
        if ($pathEsptool) {
            Write-Host "   Found esptool in PATH: $pathEsptool" -ForegroundColor DarkGray
            $Script:Config.Targets[$Target].EsptoolPath = $pathEsptool
            return $true
        }
    }

    # Additional fallback: PlatformIO python may already have esptool module installed.
    $pioPythonCheckCmd = "test -x ~/.platformio/penv/bin/python && ~/.platformio/penv/bin/python -c 'import esptool' >/dev/null 2>&1 && echo EXISTS"
    $pioPythonCheck = Invoke-SshCommand -TargetHost $targetConfig.Host -Command $pioPythonCheckCmd -Silent -Timeout 10
    if ($pioPythonCheck.Output -match "EXISTS") {
        $Script:Config.Targets[$Target].EsptoolPath = "~/.platformio/penv/bin/python -m esptool"
        Write-Host "   Using PlatformIO Python module: ~/.platformio/penv/bin/python -m esptool" -ForegroundColor DarkGray
        return $true
    }

    # On macOS, auto-install missing dependency chain (Homebrew + Python) when needed.
    if ($Target -eq "mac") {
        Write-Host "   Checking Python toolchain on macOS..." -ForegroundColor DarkGray
        $pythonReadyCmd = "if [ -x /opt/homebrew/bin/python3 ] || [ -x /usr/local/bin/python3 ] || [ -x `$HOME/miniforge3/bin/python ]; then echo READY; elif command -v pip3 >/dev/null 2>&1 && pip3 --version >/dev/null 2>&1; then echo READY; elif command -v python3 >/dev/null 2>&1 && python3 -m pip --version >/dev/null 2>&1; then echo READY; else echo MISSING; fi"
        $pythonReady = Invoke-SshCommand -TargetHost $targetConfig.Host -Command $pythonReadyCmd -Silent -Timeout 15

        if ($pythonReady.Output -notmatch "READY") {
            Write-Host "   Python toolchain missing - attempting auto-install (Homebrew + Python)..." -ForegroundColor Yellow

            $bootstrapContent = Get-Content (Join-Path $PSScriptRoot "scripts\bootstrap-python.sh") -Raw
            $bootstrapContent = $bootstrapContent -replace "`r`n", "`n"
            $bootstrapB64 = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($bootstrapContent))
            $bootstrapResult = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "echo $bootstrapB64 | base64 -d | bash" -Timeout 900
            if (-not $bootstrapResult.Success -or $bootstrapResult.Output -notmatch 'PYTHON_READY') {
                Write-Failure "Failed to auto-install Python dependencies on macOS"
                if ($bootstrapResult.Output) {
                    Write-Host "   Output: $($bootstrapResult.Output -join ' ')" -ForegroundColor Red
                }
                return $false
            }

            Write-Success "Python toolchain ready"
        }
    }
    
    Write-Host "   esptool not found, installing via pip..." -ForegroundColor Yellow
    
    # Use explicit separators so remote shells don't misparse command blocks.
    # Avoid `which` on macOS (can trigger xcode-select), and only emit INSTALL_OK on verified success.
    $installContent = Get-Content (Join-Path $PSScriptRoot "scripts\install-esptool.sh") -Raw
    $installContent = $installContent -replace "`r`n", "`n"
    $installB64 = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($installContent))

    Write-Host "   Running: pip install esptool..." -ForegroundColor DarkGray
    $installResult = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "echo $installB64 | base64 -d | bash" -Timeout 120
    
    if (-not $installResult.Success -or $installResult.Output -notmatch 'INSTALL_OK') {
        Write-Failure "Failed to install esptool"
        if ($installResult.Output) {
            Write-Host "   Output: $($installResult.Output -join ' ')" -ForegroundColor Red
            if (($installResult.Output -join ' ') -match 'xcode-select: error: No developer tools were found') {
                Write-Host "   Remote macOS only has Apple shim python/pip (/usr/bin)." -ForegroundColor Yellow
                Write-Host "   Install real Python first (recommended):" -ForegroundColor Yellow
                Write-Host "     1) Install Homebrew (if missing)" -ForegroundColor Yellow
                Write-Host "     2) brew install python" -ForegroundColor Yellow
                Write-Host "   Or install Command Line Tools on the remote Mac and retry." -ForegroundColor Yellow
            }
        }
        return $false
    }
    
    Write-Host "   Install complete, verifying..." -ForegroundColor DarkGray
    
    # Verify installation
    $verifyResult = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "$esptoolPath version 2>&1 | head -1" -Silent -Timeout 10
    if ($verifyResult.Success -and $verifyResult.Output) {
        $version = ($verifyResult.Output -join ' ').Trim()
        Write-Success "esptool installed: $version"
        return $true
    }
    
    Write-Failure "esptool installation could not be verified"
    return $false
}

function Get-SerialPortPids {
    <#
    .SYNOPSIS
        Gets PIDs of processes using a serial port
    .DESCRIPTION
        Queries the remote machine for processes using the specified serial port.
        Returns an array of PIDs (empty if port is free).
    #>
    param(
        [ValidateSet("mac", "unraid", "bowie-phone", "local")]
        [string]$Target = "mac",
        [string]$SerialPort
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    if (-not $SerialPort) {
        $SerialPort = $targetConfig.DefaultSerialPort
    }
    
    # Local target doesn't use this function
    if (Test-IsLocalTarget -Target $Target) {
        return @()
    }
    
    $checkCmd = "lsof '$SerialPort' 2>/dev/null | grep -v '^COMMAND' | awk '{print `$2}' | head -5"
    $result = Invoke-SshCommand -TargetHost $targetConfig.Host -Command $checkCmd -Silent -Timeout 10
    
    if (-not $result.Output -or $result.Output.Length -eq 0) {
        return @()
    }
    
    $pids = $result.Output -split "`n" | Where-Object { $_ -match '^\d+$' }
    return $pids
}

function Clear-SerialPort {
    <#
    .SYNOPSIS
        Kills any processes using the serial port
    .DESCRIPTION
        Checks for processes (screen, picocom, minicom, etc.) using the serial port
        and kills them to free the port for flashing.
        On Windows local, checks for processes locking COM ports.
    #>
    param(
        [ValidateSet("mac", "unraid", "bowie-phone", "local")]
        [string]$Target = "mac",
        [string]$SerialPort
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    if (-not $SerialPort) {
        $SerialPort = $targetConfig.DefaultSerialPort
    }
    
    Write-Host "   Checking if port $SerialPort is in use..." -ForegroundColor DarkGray
    
    # Handle local Windows target
    if (Test-IsLocalTarget -Target $Target) {
        # On Windows, COM ports auto-release when process closes
        # Just check if we can open the port briefly
        try {
            $port = New-Object System.IO.Ports.SerialPort $SerialPort
            $port.Open()
            $port.Close()
            Write-Host "   Port is free" -ForegroundColor DarkGray
            return $true
        }
        catch {
            Write-Warning "Port may be in use: $_"
            Write-Host "   Will attempt flash anyway (esptool can force DTR/RTS)" -ForegroundColor DarkGray
            return $true  # Proceed anyway - esptool can usually handle it
        }
    }
    
    # Check if port is busy
    $pids = Get-SerialPortPids -Target $Target -SerialPort $SerialPort
    
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
        $pids = Get-SerialPortPids -Target $Target -SerialPort $SerialPort
        if ($pids.Count -eq 0) {
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
    $pids = Get-SerialPortPids -Target $Target -SerialPort $SerialPort
    if ($pids.Count -eq 0) {
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
        [string]$BuildEnvironment = "bowie-phone-1",
        [switch]$Clean,
        [string]$BuildArgs
    )
    
    $buildDir = Join-Path $Script:Config.ProjectRoot ".pio\build\$BuildEnvironment"
    
    Write-Host "`n🔨 Building firmware for $BuildEnvironment..." -ForegroundColor Cyan
    
    Push-Location $Script:Config.ProjectRoot
    try {
        if ($Clean) {
            Write-Host "   Cleaning..." -ForegroundColor DarkGray
            & $Script:Config.PioExe run -e $BuildEnvironment -t clean 2>&1 | Out-Null
        }
        
        # Set extra build flags if provided (appends to existing build_flags)
        if ($BuildArgs) {
            Write-Host "   Extra build flags: $BuildArgs" -ForegroundColor DarkGray
            $env:PLATFORMIO_BUILD_FLAGS = $BuildArgs
        }
        
        $output = & $Script:Config.PioExe run -e $BuildEnvironment 2>&1
        
        # Clear the environment variable after build
        if ($BuildArgs) {
            Remove-Item Env:\PLATFORMIO_BUILD_FLAGS -ErrorAction SilentlyContinue
        }
        
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
        $flags = Get-BuildFlags -Environment $BuildEnvironment
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

function Test-BuildFresh {
    <#
    .SYNOPSIS
        Check whether the existing firmware build is up-to-date with source files.
    .DESCRIPTION
        Compares the last-write time of firmware.bin against all files in src/,
        include/, and platformio.ini. Returns $true when the build output exists
        and is newer than every source file.
    #>
    param(
        [string]$BuildEnvironment = "bowie-phone-1"
    )

    $buildDir  = Join-Path $Script:Config.ProjectRoot ".pio\build\$BuildEnvironment"
    $firmware  = Join-Path $buildDir "firmware.bin"

    if (-not (Test-Path $firmware)) {
        return $false
    }

    $fwTime = (Get-Item $firmware).LastWriteTimeUtc

    $sourceDirs = @(
        (Join-Path $Script:Config.ProjectRoot "src"),
        (Join-Path $Script:Config.ProjectRoot "include")
    )
    $sourceFiles = @(
        (Join-Path $Script:Config.ProjectRoot "platformio.ini")
    )
    foreach ($dir in $sourceDirs) {
        if (Test-Path $dir) {
            $sourceFiles += Get-ChildItem -Path $dir -Recurse -File | Select-Object -ExpandProperty FullName
        }
    }

    foreach ($f in $sourceFiles) {
        if ((Get-Item $f).LastWriteTimeUtc -gt $fwTime) {
            return $false
        }
    }
    return $true
}

function New-RemoteDeployScript {
    <#
    .SYNOPSIS
        Generate the bash script that runs on the remote machine
    #>
    param(
        [string]$Target,
        [string]$FlashMethod = "serial",
        [string]$SerialPort,
        [string]$DeviceIp,
        [string]$PythonCmd = "python",
        [string]$WifiSsid,
        [string]$WifiPassword,
        [switch]$ConfigureWifi,
        [switch]$NoMonitor
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    $port = if ($SerialPort) { $SerialPort } else { $targetConfig.DefaultSerialPort }
    $staging = $targetConfig.StagingDir
    $esptool = $targetConfig.EsptoolPath
    $logFile = $targetConfig.LogFile
    $apName = $Script:Config.ApName
    $apIp = $Script:Config.ApIp
    $apPassword = $targetConfig.ApPassword

    # Get OTA settings from build flags if available
    $buildFlags = Get-BuildFlags
    $otaPort = if ($buildFlags.OtaPort) { $buildFlags.OtaPort } else { "3232" }
    $otaPassword = if ($buildFlags.OtaPassword) { $buildFlags.OtaPassword } else { "" }

    # Build flash block and method name
    if ($FlashMethod -eq "ota") {
        # HTTP OTA flash using curl POST to /update endpoint
        # Works reliably over WireGuard (TCP) unlike espota.py (UDP)
        $deviceIp = if ($DeviceIp) { $DeviceIp } `
                    elseif ($buildFlags.DeviceIp) { $buildFlags.DeviceIp } `
                    elseif ($targetConfig.DefaultOtaDeviceIp) { $targetConfig.DefaultOtaDeviceIp } `
                    else { $null }
        if (-not $deviceIp) {
            throw "Device IP required for OTA flash (use -DeviceIp parameter, set WIREGUARD_LOCAL_IP in build, or add DefaultOtaDeviceIp to target config)"
        }
        $flashMethodName = "HTTP OTA to $deviceIp"
        $flashBlock = @"
log "Preparing device for OTA update..."
PREPARE_RESULT=`$(curl -s -m 120 http://$deviceIp/prepareota 2>&1) || true
log "Prepare response: `$PREPARE_RESULT"
if echo "`$PREPARE_RESULT" | grep -q "OK"; then
    log "Device prepared for OTA"
else
    log "WARNING: Prepare may have timed out - continuing anyway"
fi
sleep 3
FW_SIZE=`$(stat -c%s "`$STAGING/firmware.bin" 2>/dev/null || stat -f%z "`$STAGING/firmware.bin" 2>/dev/null)
log "Uploading firmware via HTTP OTA (`$FW_SIZE bytes)..."
log "Command: curl -f -m 600 -F firmware=@`$STAGING/firmware.bin http://$deviceIp/update?size=`$FW_SIZE"
UPLOAD_RESULT=`$(curl -f -m 600 -F "firmware=@`$STAGING/firmware.bin" "http://$deviceIp/update?size=`$FW_SIZE" 2>&1) || true
log "Upload response: `$UPLOAD_RESULT"
if echo "`$UPLOAD_RESULT" | grep -qi "OK\|success"; then
    log "Flash completed successfully!"
else
    log "ERROR: HTTP OTA upload may have failed"
    log "Response: `$UPLOAD_RESULT"
    echo "RESULT:FLASH_FAILED"
    exit 1
fi
"@
    } else {
        # Serial flash using esptool
        $flashCmd = "$esptool --chip esp32 --port $port --baud $($Script:Config.BaudRate) --before default_reset --after hard_reset write_flash -z --flash_mode $($Script:Config.FlashMode) --flash_freq $($Script:Config.FlashFreq) --flash_size detect $($Script:Config.BootloaderOffset) $staging/bootloader.bin $($Script:Config.PartitionsOffset) $staging/partitions.bin $($Script:Config.FirmwareOffset) $staging/firmware.bin"
        $flashMethodName = "Serial via $port"
        $flashBlock = @"
log "Command: $flashCmd"
if $flashCmd >> "`$LOGFILE" 2>&1; then
    log "Flash completed successfully!"
else
    log "ERROR: Flash failed!"
    echo "RESULT:FLASH_FAILED"
    exit 1
fi
"@
    }
    
    # WiFi block (optional)
    $wifiBlock = ""
    if ($ConfigureWifi -and $WifiSsid) {
        $wifiBlock = @"

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

    # Read deploy script template and substitute tokens
    $deployScript = Get-Content (Join-Path $PSScriptRoot "scripts\deploy.sh") -Raw
    $deployScript = $deployScript -replace "`r`n", "`n"
    $deployScript = $deployScript.Replace("@@TIMESTAMP@@",         (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
    $deployScript = $deployScript.Replace("@@LOGFILE@@",           $logFile)
    $deployScript = $deployScript.Replace("@@STAGING@@",           $staging)
    $deployScript = $deployScript.Replace("@@PORT@@",              $port)
    $deployScript = $deployScript.Replace("@@FLASH_METHOD@@",      $FlashMethod)
    $deployScript = $deployScript.Replace("@@FLASH_METHOD_NAME@@", $flashMethodName)
    $deployScript = $deployScript.Replace("@@FLASH_BLOCK@@",       $flashBlock)
    $deployScript = $deployScript.Replace("@@WIFI_BLOCK@@",        $wifiBlock)
    $deployScript = $deployScript -replace "`r`n", "`n"
    return $deployScript
}

function Copy-FirmwareToRemote {
    param(
        [ValidateSet("mac", "unraid", "bowie-phone", "local")]
        [string]$Target = "mac",
        [string]$BuildEnvironment = "bowie-phone-1",
        [ValidateSet("serial", "ota")]
        [string]$FlashMethod = "serial"
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    $buildDir = Join-Path $Script:Config.ProjectRoot ".pio\build\$BuildEnvironment"
    
    # Handle local target - no upload needed, just verify files exist
    if (Test-IsLocalTarget -Target $Target) {
        Write-Host "`n✅ Using local firmware files..." -ForegroundColor Cyan
        
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
            $fileName = Split-Path $file -Leaf
            $fileSize = [math]::Round((Get-Item $file).Length / 1KB, 1)
            Write-Host "   ✓ $fileName ($fileSize KB)" -ForegroundColor DarkGray
        }
        
        Write-Success "All firmware files present"
        return $true
    }
    
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
    
    # OTA mode only needs firmware.bin (HTTP upload), no extra tools needed
    
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

function Send-DeviceCommand {
    <#
    .SYNOPSIS
        Send a debug command to the device via its telnet interface
    .DESCRIPTION
        Connects to the device's telnet port (23) through the target's SSH host
        and sends the command string.  The device processes it via processDebugInput()
        in special_command_processor.cpp.  Type 'help' from the monitor or call
        Send-DeviceCommand -Command 'help' to see the full list of supported commands.
    .PARAMETER Target
        Deploy target whose SSH host and device IP to use
    .PARAMETER Command
        Command string to send (e.g. 'help', 'state', 'hook', '*#08#', '1234')
    .PARAMETER DeviceIp
        Override device IP (default: target's DefaultOtaDeviceIp)
    .PARAMETER TelnetPort
        Telnet port on the device (default: 23)
    .EXAMPLE
        Send-DeviceCommand -Target bowie-phone -Command "help"
        Send-DeviceCommand -Target bowie-phone -Command "*#08#"
        Send-DeviceCommand -Target bowie-phone -Command "state"
    #>
    param(
        [Parameter(Mandatory)]
        [ValidateSet("mac", "unraid", "bowie-phone", "local")]
        [string]$Target,
        [Parameter(Mandatory)]
        [string]$Command,
        [string]$DeviceIp,
        [int]$TelnetPort = 23
    )

    $targetConfig = $Script:Config.Targets[$Target]
    $sshHost = $targetConfig.Host
    $ip = if ($DeviceIp)                          { $DeviceIp }
          elseif ($targetConfig.DefaultOtaDeviceIp) { $targetConfig.DefaultOtaDeviceIp }
          else                                       { $null }

    if (-not $ip) {
        Write-Warning "No device IP configured for target '$Target' — cannot send command"
        return $false
    }

    Write-Host "   → $Command" -ForegroundColor Cyan

    # Fail fast when endpoint is unreachable to avoid hanging telnet clients.
    $preflightCmd = @"
python3 - <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(2.0)
rc = s.connect_ex(("$ip", $TelnetPort))
s.close()
print(rc)
sys.exit(0 if rc == 0 else 2)
PY
"@
    $preflight = Invoke-SshCommand -TargetHost $sshHost -Command $preflightCmd -Silent -Timeout 8
    if (-not $preflight.Success) {
        $probe = if ($preflight.Output) { ($preflight.Output | Select-Object -Last 1).ToString().Trim() } else { "unknown" }
        Write-Warning (("Device telnet endpoint {0}:{1} is unreachable from {2} (probe={3})" -f $ip, $TelnetPort, $sshHost, $probe))
        return $false
    }

    # Base64-encode the bash fragment to bypass SSH/shell quoting entirely.
    # Sends the command to the device's telnet port via nc, waits 2 s for any response.
    $escapedCmd = $Command -replace "'", "'\''"
    $bashScript = @'
if command -v ncat >/dev/null 2>&1; then
    printf '%s\r\n' '@@CMD@@' | ncat @@IP@@ @@PORT@@ -w2 2>/dev/null
elif command -v nc >/dev/null 2>&1; then
    printf '%s\r\n' '@@CMD@@' | nc -w2 @@IP@@ @@PORT@@ 2>/dev/null
elif command -v telnet >/dev/null 2>&1; then
    { printf '%s\r\n' '@@CMD@@'; sleep 1; } | telnet @@IP@@ @@PORT@@ 2>/dev/null
else
    echo "ERROR: nc/ncat/telnet not found on remote host"
    exit 127
fi
'@
    $bashScript = $bashScript.Replace('@@CMD@@', $escapedCmd).Replace('@@IP@@', $ip).Replace('@@PORT@@', [string]$TelnetPort)
    $bashScript = $bashScript -replace "`r`n", "`n"
    $b64 = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($bashScript))
    $result = Invoke-SshCommand -TargetHost $sshHost -Command "echo $b64 | base64 -d | bash" -Silent -Timeout 10

    if ($result.Success -and $result.Output) {
        $result.Output | ForEach-Object {
            $line = $_.ToString()
            if ($line.Trim()) {
                Write-Host "   ← $line" -ForegroundColor DarkGray
            }
        }
    }
    return $result.Success
}

function Monitor-TelnetLogs {
    <#
    .SYNOPSIS
        Show recent remote logs, then attach to live device telnet stream
    .DESCRIPTION
        For TelnetLogs targets, this prints a snapshot of recent server-side logs
        first, then opens a direct interactive telnet connection to the device.
        This keeps stdin/stdout bound to the real telnet session so typed commands
        (e.g. "help", "state", "*#08#") are handled immediately by the device.
    .PARAMETER Target
        Target whose SSH host and device ID are used
    .PARAMETER DeviceIp
        Optional explicit device IP (default: target DefaultOtaDeviceIp)
    .PARAMETER TelnetPort
        Device telnet port (default: 23)
    .PARAMETER Lines
        Number of recent server-log lines to show before attaching (default: 120)
    #>
    param(
        [ValidateSet("mac", "unraid", "bowie-phone", "local")]
        [string]$Target = "bowie-phone",
        [string]$DeviceIp,
        [int]$TelnetPort = 23,
        [int]$Lines = 120
    )

    if (Test-IsLocalTarget -Target $Target) {
        Write-Warning "Telnet log monitor requires a remote SSH target"
        return $false
    }

    $targetConfig = $Script:Config.Targets[$Target]
    $deviceId = $targetConfig.DeviceId
    $sshHost  = $targetConfig.Host
    $logBase  = $Script:Config.RemoteLogBasePath
    $logDir   = "$logBase/$deviceId"

    $ip = if ($DeviceIp) { $DeviceIp }
          elseif ($targetConfig.DefaultOtaDeviceIp) { $targetConfig.DefaultOtaDeviceIp }
          else { $null }

    if (-not $ip) {
        Write-Warning "No device IP configured for target '$Target' - cannot open telnet"
        return $false
    }

    Write-Host "`n📡 Telnet Monitor" -ForegroundColor Yellow
    Write-Host "   Device:  $deviceId" -ForegroundColor DarkGray
    Write-Host "   Via SSH: $sshHost" -ForegroundColor DarkGray
    Write-Host "   Logs:    $logDir" -ForegroundColor DarkGray
    Write-Host "   Telnet:  $ip`:$TelnetPort" -ForegroundColor DarkGray
    Write-Host ""

    # Show a quick snapshot of server-side logs before switching to live telnet.
    $historyScript = @'
latest_file=""
if [ -d "@@LOGDIR@@" ]; then
    latest_file=$(ls -1t "@@LOGDIR@@"/*.log 2>/dev/null | head -1)
fi
if [ -n "$latest_file" ] && [ -f "$latest_file" ]; then
    echo "----- BEGIN RECENT LOGS ($latest_file) -----"
    tail -n @@LINES@@ "$latest_file"
    echo "----- END RECENT LOGS -----"
else
    echo "No existing log files in @@LOGDIR@@"
fi
'@
    $historyScript = $historyScript.Replace('@@LOGDIR@@', $logDir).Replace('@@LINES@@', [string]$Lines)

    Write-Host "📜 Recent server logs" -ForegroundColor Yellow
    $history = Invoke-SshCommand -TargetHost $sshHost -Command $historyScript -Silent -Timeout 20
    if ($history.Success -and $history.Output) {
        foreach ($rawLine in $history.Output) {
            $line = $rawLine.ToString()
            if ($line -match 'ERROR|FAIL') {
                Write-Host "   $line" -ForegroundColor Red
            } elseif ($line -match 'WARN') {
                Write-Host "   $line" -ForegroundColor Yellow
            } else {
                Write-Host "   $line" -ForegroundColor DarkGray
            }
        }
    } else {
        Write-Warning "Could not read recent server logs (continuing to live telnet)"
    }

    Write-Host ""

    # Fail fast before opening an interactive telnet client.
    $preflightCmd = @"
python3 - <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(2.0)
rc = s.connect_ex(("$ip", $TelnetPort))
s.close()
print(rc)
sys.exit(0 if rc == 0 else 2)
PY
"@
    $preflight = Invoke-SshCommand -TargetHost $sshHost -Command $preflightCmd -Silent -Timeout 10
    if (-not $preflight.Success) {
        $probe = if ($preflight.Output) { ($preflight.Output | Select-Object -Last 1).ToString().Trim() } else { "unknown" }
        Write-Warning (("Cannot reach {0}:{1} from {2} (probe={3})" -f $ip, $TelnetPort, $sshHost, $probe))
        return $false
    }

    Write-Host "🔌 Connecting to live telnet stream..." -ForegroundColor Yellow
    Write-Host "   Type commands and press Enter (example: help)" -ForegroundColor DarkGray
    Write-Host "   Press Ctrl+C to stop" -ForegroundColor DarkGray
    Write-Host ""

    $connectScript = @'
if command -v telnet >/dev/null 2>&1; then
    exec telnet @@IP@@ @@PORT@@
elif command -v ncat >/dev/null 2>&1; then
    exec ncat @@IP@@ @@PORT@@
elif command -v nc >/dev/null 2>&1; then
    exec nc @@IP@@ @@PORT@@
else
    echo "ERROR: nc/ncat/telnet not found on remote host"
    exit 127
fi
'@
    $connectScript = $connectScript.Replace('@@IP@@', $ip).Replace('@@PORT@@', [string]$TelnetPort)
    $connectScript = $connectScript -replace "`r`n", "`n"
    $connectB64 = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($connectScript))

    & ssh -t $sshHost "echo '$connectB64' | base64 -d | bash"

    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 130) {
        Write-Warning "Telnet session ended with exit code $LASTEXITCODE"
        return $false
    }

    return $true
}

function Monitor-SerialOutput {
    <#
    .SYNOPSIS
        Monitor serial output from ESP32 device
    .DESCRIPTION
        Connects to the remote machine and monitors serial output via screen logging.
        Screen handles baud rate properly, logs to file, we tail that for clean output.
        Press Ctrl+C to exit.
    .PARAMETER Replay
        Replay the most recent monitoring session log instead of starting a new session
    #>
    param(
        [ValidateSet("mac", "unraid", "bowie-phone", "local")]
        [string]$Target = "mac",
        [string]$SerialPort,
        [int]$BaudRate = 115200,
        [switch]$NoReset,
        [switch]$Replay
    )
    
    # Local target: not applicable for serial monitoring via SSH
    if (Test-IsLocalTarget -Target $Target) {
        Write-Warning "Serial monitoring not supported for local target"
        Write-Host "   Use PlatformIO monitor or a serial terminal like PuTTY/TeraTerm" -ForegroundColor Yellow
        return
    }
    
    $targetConfig = $Script:Config.Targets[$Target]

    # TelnetLogs target: show recent server logs, then attach stdin/stdout to live telnet
    if ($targetConfig.TelnetLogs) {
        if ($Replay) {
            Write-Warning "Replay is not supported in telnet mode; opening live telnet session"
        }
        Monitor-TelnetLogs -Target $Target | Out-Null
        return
    }

    $port = if ($SerialPort) { $SerialPort } else { $targetConfig.DefaultSerialPort }
    $esptool = $targetConfig.EsptoolPath
    
    if ($Replay) {
        # Find and replay the most recent log file
        Write-Host ""
        Write-Host "📼 Replaying previous serial session..." -ForegroundColor Yellow
        Write-Host "   Target: $($targetConfig.Host)" -ForegroundColor DarkGray
        Write-Host ""
        
        $findLogCmd = "ls -td /tmp/serial_monitor.*/screenlog.0 2>/dev/null | head -1"
        $result = Invoke-SshCommand -TargetHost $targetConfig.Host -Command $findLogCmd -Silent -Timeout 10
        
        if (-not $result.Success -or -not $result.Output -or $result.Output.Length -eq 0) {
            Write-Failure "No previous session log found"
            return
        }
        
        $logFile = [string]$result.Output[0]
        $logFile = $logFile.Trim()
        if (-not $logFile) {
            Write-Failure "No previous session log found"
            return
        }
        
        Write-Host "   Log: $logFile" -ForegroundColor DarkGray
        
        # Check if file has content
        $sizeResult = Invoke-SshCommand -TargetHost $targetConfig.Host -Command "wc -c < '$logFile' 2>/dev/null" -Silent -Timeout 10
        if ($sizeResult.Success -and $sizeResult.Output) {
            $size = [string]$sizeResult.Output[0]
            $size = $size.Trim()
            Write-Host "   Size: $size bytes" -ForegroundColor DarkGray
        }
        
        Write-Host ""
        
        # Display the log file with less for scrolling
        & ssh -t $targetConfig.Host "less -R +G '$logFile' 2>/dev/null || cat '$logFile'"
        return
    }
    
    # Normal monitoring mode - create temp directory for this session's log
    $logDir = "/tmp/serial_monitor.$([guid]::NewGuid().ToString('N').Substring(0,8))"
    
    Write-Host ""
    Write-Host "📡 Monitoring serial output from $port" -ForegroundColor Yellow
    Write-Host "   Target: $($targetConfig.Host)" -ForegroundColor DarkGray
    Write-Host "   Baud: $BaudRate" -ForegroundColor DarkGray
    Write-Host "   Exit: Ctrl+C" -ForegroundColor DarkGray
    Write-Host "   Log: $logDir/screenlog.0" -ForegroundColor DarkGray
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
    
    # Create temp directory and run screen with logging enabled
    Write-Host "   Starting screen session..." -ForegroundColor DarkGray
    Write-Host ""
    
    try {
        # Create temp directory for this session's log, cd into it, run screen with -L
        # Screen will create screenlog.0 in the current directory
        & ssh -t $targetConfig.Host "mkdir -p '$logDir' && cd '$logDir' && screen -L '$port' $BaudRate"
    }
    finally {
        Write-Host ""
        Write-Host "   Session log: $logDir/screenlog.0" -ForegroundColor DarkGray
        Write-Host "   Replay with: Monitor-SerialOutput -Replay" -ForegroundColor DarkGray
    }
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
        [ValidateSet("mac", "unraid", "bowie-phone", "local")]
        [string]$Target = "mac",
        [ValidateSet("serial", "ota")]
        [string]$FlashMethod = "serial",
        [string]$BuildEnvironment = "bowie-phone-1",
        [string]$SerialPort,
        [string]$DeviceIp,
        [string]$PythonCmd = "python3",
        [string]$WifiSsid,
        [string]$WifiPassword,
        [string]$EsptoolCmd,
        [switch]$NoMonitor,
        [switch]$NoWait
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    
    Write-Host "`n🚀 Starting deployment..." -ForegroundColor Cyan
    
    # Handle local Windows deployment
    if (Test-IsLocalTarget -Target $Target) {
        if ($FlashMethod -eq "ota") {
            Write-Failure "Local OTA not yet implemented - use remote target for OTA"
            return $false
        }
        
        # Local serial flash
        $buildDir = Join-Path $Script:Config.ProjectRoot ".pio\build\$BuildEnvironment"
        $port = if ($SerialPort) { $SerialPort } else { $targetConfig.DefaultSerialPort }
        
        $firmwareBin = Join-Path $buildDir "firmware.bin"
        $bootloaderBin = Join-Path $buildDir "bootloader.bin"
        $partitionsBin = Join-Path $buildDir "partitions.bin"
        
        Write-Host "   Flashing to $port..." -ForegroundColor DarkGray
        Write-Host ""
        
        # Build esptool command
        $esptool = if ($EsptoolCmd) { $EsptoolCmd } else { "python -m esptool" }
        $flashCmd = "$esptool --chip esp32 --port $port --baud $($Script:Config.BaudRate) --before default_reset --after hard_reset write_flash -z --flash_mode $($Script:Config.FlashMode) --flash_freq $($Script:Config.FlashFreq) --flash_size detect $($Script:Config.BootloaderOffset) `"$bootloaderBin`" $($Script:Config.PartitionsOffset) `"$partitionsBin`" $($Script:Config.FirmwareOffset) `"$firmwareBin`""
        
        Write-Host "   Command: $flashCmd" -ForegroundColor DarkGray
        Write-Host ""
        
        # Execute esptool
        $output = Invoke-Expression $flashCmd 2>&1
        $success = $LASTEXITCODE -eq 0
        
        # Display output
        $output | ForEach-Object {
            $line = $_.ToString()
            if ($line -match "error|fail|ERROR|FAIL") {
                Write-Host "   $line" -ForegroundColor Red
            } elseif ($line -match "Writing|Wrote|Hash|Leaving") {
                Write-Host "   $line" -ForegroundColor Cyan
            } else {
                Write-Host "   $line" -ForegroundColor DarkGray
            }
        }
        
        Write-Host ""
        
        if ($success) {
            Write-Success "Flash completed successfully!"
            return $true
        } else {
            Write-Failure "Flash failed!"
            Write-Host "   Check that:" -ForegroundColor Yellow
            Write-Host "     - Device is connected to $port" -ForegroundColor Yellow
            Write-Host "     - No other program is using the port" -ForegroundColor Yellow
            Write-Host "     - USB drivers are installed" -ForegroundColor Yellow
            return $false
        }
    }
    
    # Generate the deployment script
    $configureWifi = $WifiSsid -and $WifiSsid.Length -gt 0
    $script = New-RemoteDeployScript -Target $Target -FlashMethod $FlashMethod `
        -SerialPort $SerialPort -DeviceIp $DeviceIp -PythonCmd $PythonCmd `
        -WifiSsid $WifiSsid -WifiPassword $WifiPassword `
        -ConfigureWifi:$configureWifi -NoMonitor:$NoMonitor
    
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
            Write-Failure "Deployment failed"
            
            # Fetch and display the remote log
            Write-Host "`n📋 Remote deployment log:" -ForegroundColor Yellow
            Write-Host ("=" * 70) -ForegroundColor Yellow
            
            $logCmd = "tail -50 $($targetConfig.LogFile) 2>/dev/null || echo 'Log file not found'"
            $logResult = Invoke-SshCommand -TargetHost $targetConfig.Host -Command $logCmd -Silent -Timeout 10
            
            if ($logResult.Success -and $logResult.Output) {
                $logResult.Output | ForEach-Object {
                    if ($_ -match "ERROR") {
                        Write-Host $_ -ForegroundColor Red
                    } elseif ($_ -match "WARNING") {
                        Write-Host $_ -ForegroundColor Yellow
                    } else {
                        Write-Host $_
                    }
                }
            } else {
                Write-Host "Could not fetch remote log from $($targetConfig.LogFile)" -ForegroundColor Red
            }
            
            Write-Host ("=" * 70) -ForegroundColor Yellow
            return $false
        }
        
        # If NoMonitor, tail the serial output
        if (!$NoMonitor) {
            Monitor-SerialOutput -Target $Target -SerialPort $SerialPort -NoReset
        }
        
        return $true
    }
}

function Deploy-ToDevice {
    param(
        [string]$BuildEnvironment = "bowie-phone-1",
        [ValidateSet("mac", "unraid", "bowie-phone", "local")]
        [string]$Target = "mac",
        [ValidateSet("serial", "ota")]
        [string]$FlashMethod = "serial",
        [string]$SerialPort,
        [string]$DeviceIp,
        [string]$WifiSsid,
        [string]$WifiPassword,
        [switch]$SkipBuild,
        [switch]$Clean,
        [switch]$NoMonitor,
        [switch]$NoWait,
        [string]$BuildArgs
    )
    
    # Auto-detect whether a build is needed when neither -SkipBuild nor -Clean was given
    if (-not $SkipBuild -and -not $Clean) {
        if (Test-BuildFresh -Environment $BuildEnvironment) {
            Write-Host "   Build is up-to-date — skipping build step" -ForegroundColor DarkGray
            $SkipBuild = [switch]::new($true)
        }
    }

    $totalSteps = if ($SkipBuild) { 2 } else { 3 }
    $step = 0
    
    $isLocal = Test-IsLocalTarget -Target $Target
    $deployType = if ($isLocal) { "Local" } else { "via SSH" }

    # Apply target's preferred flash method if the caller didn't explicitly override it
    $targetConfig = $Script:Config.Targets[$Target]
    if ($targetConfig.DefaultFlashMethod -and $FlashMethod -eq "serial") {
        $FlashMethod = $targetConfig.DefaultFlashMethod
    }
    
    Write-Host "`n" + ("=" * 55) -ForegroundColor Cyan
    Write-Host "🚀 ESP32 Deployment $deployType" -ForegroundColor Cyan
    Write-Host ("=" * 55) -ForegroundColor Cyan
    Write-Host "   Environment: $BuildEnvironment"
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
    
    # Ensure required dependencies are installed on remote/local
    $depsResult = Ensure-RemoteDependencies -Target $Target -FlashMethod $FlashMethod
    if ($depsResult -is [hashtable]) {
        if (-not $depsResult.Success) {
            return $false
        }
        $pythonCmd = if ($depsResult.PythonCmd) { $depsResult.PythonCmd } else { "python3" }
        $esptoolCmd = if ($depsResult.EsptoolCmd) { $depsResult.EsptoolCmd } else { $null }
    } elseif (-not $depsResult) {
        return $false
    } else {
        $pythonCmd = "python3"
        $esptoolCmd = $null
    }
    
    # Step 1: Build
    if (-not $SkipBuild) {
        $step++
        Write-Step $step $totalSteps "Building firmware"
        if (-not (Build-Firmware -Environment $BuildEnvironment -Clean:$Clean -BuildArgs $BuildArgs)) {
            return $false
        }
    }
    
    # Step 2: Upload files
    $step++
    Write-Step $step $totalSteps "Uploading to $Target"
    if (-not (Copy-FirmwareToRemote -Target $Target -BuildEnvironment $BuildEnvironment -FlashMethod $FlashMethod)) {
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
    $deployLabel = if ($isLocal) { "Flashing device" } else { "Running remote deployment" }
    Write-Step $step $totalSteps $deployLabel
    $deployParams = @{
        Target = $Target
        FlashMethod = $FlashMethod
        SerialPort = $SerialPort
        DeviceIp = $DeviceIp
        PythonCmd = $pythonCmd
        WifiSsid = $WifiSsid
        WifiPassword = $WifiPassword
        NoMonitor = $NoMonitor
        NoWait = $NoWait
    }
    if ($esptoolCmd) {
        $deployParams.EsptoolCmd = $esptoolCmd
    }
    if ($isLocal) {
        # Pass Environment for local builds
        $deployParams.Environment = $BuildEnvironment
    }
    $success = Start-RemoteDeployment @deployParams
    
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

    
<#
    .SYNOPSIS
        Increment firmware version in platformio.ini and related files
    .DESCRIPTION
        Bumps the patch version (X.Y.Z -> X.Y.Z+1) in:
        - platformio.ini (FIRMWARE_VERSION)
        - docs/firmware/manifest.json
        - docs/update.html
    .PARAMETER NewVersion
        Optional explicit version string (default: auto-increment patch)
    .EXAMPLE
        Bump-Version
        Bump-Version -NewVersion "2.0.0"
    #>
function Bump-Version {
    param(
        [string]$NewVersion
    )
    
    # Helper to get next patch version
    function Get-NextPatchVersion {
        param([string]$Current)
        $parts = $Current -split '\.'
        if ($parts.Count -lt 3) {
            throw "Version '$Current' is not in major.minor.patch format."
        }
        $parts[2] = ([int]$parts[2]) + 1
        return "$($parts[0]).$($parts[1]).$($parts[2])"
    }
    
    # Helper to replace in file
    function Replace-InFile {
        param(
            [string]$Path,
            [string]$Pattern,
            [string]$Replacement
        )
        $content = Get-Content -Path $Path -Raw
        $updated = [regex]::Replace($content, $Pattern, $Replacement)
        if ($updated -eq $content) {
            throw "Pattern '$Pattern' not found in $Path"
        }
        Set-Content -Path $Path -Value $updated
    }
    $platformioPath = Join-Path $Script:Config.ProjectRoot "platformio.ini"
    $manifestPath = Join-Path $Script:Config.ProjectRoot "docs/firmware/manifest.json"
    $updateHtmlPath = Join-Path $Script:Config.ProjectRoot "docs/update.html"
    
    # Determine target version
    if (-not $NewVersion) {
        $manifest = Get-Content -Path $manifestPath -Raw | ConvertFrom-Json
        $NewVersion = Get-NextPatchVersion -Current $manifest.version
    }
    
    Write-Host "📦 Bumping version to $NewVersion" -ForegroundColor Cyan
    
    # Apply updates
    Replace-InFile -Path $platformioPath -Pattern '(-DFIRMWARE_VERSION=\")\d+\.\d+\.\d+(\")' -Replacement "$1$NewVersion$2"
    Write-Host "   ✓ platformio.ini"
    
    Replace-InFile -Path $manifestPath -Pattern '(\"version\"\s*:\s*\")\d+\.\d+\.\d+(\")' -Replacement "$1$NewVersion$2"
    Write-Host "   ✓ manifest.json"
    
    Replace-InFile -Path $updateHtmlPath -Pattern '(id="version">)\d+\.\d+\.\d+(<)' -Replacement "$1$NewVersion$2"
    Replace-InFile -Path $updateHtmlPath -Pattern '(\|\| \'')\d+\.\d+\.\d+(\'')' -Replacement "$1$NewVersion$2"
    Write-Host "   ✓ update.html"
    
    Write-Success "Version bumped to $NewVersion"
    return $NewVersion
}

function Publish-Firmware {
    <#
    .SYNOPSIS
        Build firmware and publish to docs/firmware for web installer
    .DESCRIPTION
        Builds firmware for a specific device using PlatformIO, then copies all required
        binary files to docs/firmware/ and updates the manifest.json with the
        version from platformio.ini.
    .PARAMETER Environment
        PlatformIO environment to build (default: bowie-phone-1)
    .PARAMETER Version
        Override the version string (default: reads from platformio.ini FIRMWARE_VERSION)
    .PARAMETER SkipBuild
        Skip the build step and just copy existing binaries
    .PARAMETER Clean
        Clean before building
    .PARAMETER BuildArgs
        Extra build flags to pass to PlatformIO (e.g., "-DRUN_SD_DEBUG_FIRST")
    .EXAMPLE
        Publish-Firmware
        Publish-Firmware -Environment dream-phone-1
        Publish-Firmware -Version "1.2.3" -Environment dream-phone-1
        Publish-Firmware -SkipBuild
        Publish-Firmware -BuildArgs "-DRUN_SD_DEBUG_FIRST"
    #>
    param(
        [string]$BuildEnvironment = "bowie-phone-1",
        [string]$Version,
        [switch]$SkipBuild,
        [switch]$Clean,
        [string]$BuildArgs
    )
    
    $BuildDir = Join-Path $Script:Config.ProjectRoot ".pio\build\$BuildEnvironment"
    $FirmwareDir = Join-Path $Script:Config.ProjectRoot "docs\firmware"
    $FrameworkDir = "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32"
    
    Write-Host "`n🔧 Bowie Phone Firmware Publisher" -ForegroundColor Cyan
    Write-Host "=" * 40
    Write-Host "🎯 Environment: $BuildEnvironment" -ForegroundColor Cyan
    
    # Extract version from platformio.ini if not provided
    if (-not $Version) {
        $PioIni = Get-Content (Join-Path $Script:Config.ProjectRoot "platformio.ini") -Raw
        if ($PioIni -match 'DFIRMWARE_VERSION=\\"([^"]+)\\"') {
            $Version = $matches[1]
        } else {
            $Version = "1.0.0"
        }
    }
    Write-Host "📦 Version: $Version" -ForegroundColor Green
    
    # Auto-detect whether a build is needed when neither -SkipBuild nor -Clean was given
    if (-not $SkipBuild -and -not $Clean) {
        if (Test-BuildFresh -Environment $BuildEnvironment) {
            Write-Host "`n   Build is up-to-date — skipping build step" -ForegroundColor DarkGray
            $SkipBuild = [switch]::new($true)
        }
    }

    # Build firmware
    if (-not $SkipBuild -or -$Clean) {
        Write-Host "`n🔨 Building firmware for $BuildEnvironment..." -ForegroundColor Yellow
        if (-not (Build-Firmware -Environment $BuildEnvironment -Clean:$Clean -BuildArgs $BuildArgs)) {
            Write-Failure "Build failed"
            return $false
        }
        Write-Success "Build successful!"
    } else {
        Write-Host "`n⏭️  Skipping build (using existing binaries)" -ForegroundColor Yellow
    }
    
    # Verify build outputs exist
    $FirmwareBin = Join-Path $BuildDir "firmware.bin"
    $PartitionsBin = Join-Path $BuildDir "partitions.bin"
    $BootloaderBin = Join-Path $BuildDir "bootloader.bin"
    
    if (-not (Test-Path $FirmwareBin)) {
        Write-Failure "firmware.bin not found at $FirmwareBin - run build first"
        return $false
    }
    if (-not (Test-Path $PartitionsBin)) {
        Write-Failure "partitions.bin not found at $PartitionsBin - run build first"
        return $false
    }
    if (-not (Test-Path $BootloaderBin)) {
        Write-Failure "bootloader.bin not found at $BootloaderBin - run build first"
        return $false
    }
    
    # Locate boot_app0 from PlatformIO framework
    $BootApp0Src = Join-Path $FrameworkDir "tools\partitions\boot_app0.bin"
    if (-not (Test-Path $BootApp0Src)) {
        Write-Failure "boot_app0.bin not found at $BootApp0Src"
        return $false
    }
    
    # Create firmware directory
    if (-not (Test-Path $FirmwareDir)) {
        New-Item -ItemType Directory -Path $FirmwareDir -Force | Out-Null
    }
    
    Write-Host "`n📁 Copying binaries to docs/firmware/..." -ForegroundColor Yellow
    
    # Copy all binaries
    Copy-Item $BootloaderBin (Join-Path $FirmwareDir "bootloader.bin") -Force
    Write-Host "   ✓ bootloader.bin"
    
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
    
    Write-Success "Firmware published successfully!"
    Write-Host "   Open docs/update.html in a browser to flash devices." -ForegroundColor Gray
    
    return $true
}

function Watch-RemoteLogs {
    <#
    .SYNOPSIS
        Stream live device logs from the remote log server
    .DESCRIPTION
        The device posts batches of log lines to http://REMOTE_LOG_SERVER/logs every few seconds over WireGuard.
        This function polls that HTTP endpoint and prints new lines as they arrive, simulating tail -f.
        Server is read from platformio.ini REMOTE_LOG_SERVER (default: http://10.253.0.1:3000).
        Device ID is read from platformio.ini REMOTE_LOG_DEVICE_ID (default: bowie-phone-1).
    .PARAMETER DeviceId
        Device ID to filter logs for (default: bowie-phone-1)
    .PARAMETER ServerUrl
        Remote log server base URL (default: read from build flags, then http://10.253.0.1:3000)
    .PARAMETER PollIntervalSecs
        How often to poll the server in seconds (default: 2)
    .PARAMETER SshHost
        If set, all HTTP requests are proxied through this SSH host (e.g. root@192.168.1.216).
        Use when the log server is on a WireGuard network only reachable via that host.
    .PARAMETER Lines
        Number of historical lines to show on start (default: 50)
    .EXAMPLE
        Watch-RemoteLogs
        Watch-RemoteLogs -DeviceId bowie-phone-1
        Watch-RemoteLogs -ServerUrl http://10.253.0.1:3000
        Watch-RemoteLogs -SshHost root@192.168.1.216
    #>
    param(
        [string]$DeviceId,
        [string]$ServerUrl,
        [string]$SshHost,
        [int]$PollIntervalSecs = 2,
        [int]$Lines = 50
    )

    # Resolve device ID and server URL from build flags if not specified
    $buildFlags = Get-BuildFlags

    if (-not $DeviceId) {
        $DeviceId = if ($buildFlags.ContainsKey('RemoteLogDeviceId')) { $buildFlags.RemoteLogDeviceId } else { 'bowie-phone-1' }
    }
    if (-not $ServerUrl) {
        # Extract base URL from REMOTE_LOG_SERVER flag (strip trailing /logs path)
        $flagServer = $null
        $pioPath = Join-Path $Script:Config.ProjectRoot 'platformio.ini'
        if (Test-Path $pioPath) {
            $pioContent = Get-Content $pioPath -Raw
            if ($pioContent -match 'REMOTE_LOG_SERVER=\\?"([^"\\]+)\\?"') {
                $flagServer = $matches[1] -replace '/logs$', ''
            }
        }
        $ServerUrl = if ($flagServer) { $flagServer } else { 'http://10.253.0.1:3000' }
    }

    $logsUrl  = "$ServerUrl/logs?device=$DeviceId&lines=$Lines"
    $streamUrl = "$ServerUrl/logs/stream?device=$DeviceId"

    Write-Host "`n📡 Remote Log Monitor" -ForegroundColor Yellow
    Write-Host "   Device:  $DeviceId" -ForegroundColor DarkGray
    Write-Host "   Server:  $ServerUrl" -ForegroundColor DarkGray
    Write-Host "   Press Ctrl+C to stop" -ForegroundColor DarkGray
    Write-Host ""

    if ($SshHost) {
        Write-Host "   Via SSH: $SshHost" -ForegroundColor DarkGray
    }

    # --- Try SSE / streaming endpoint first ---
    $streamWorked = $false
    if ($SshHost) {
        $testCode = & ssh $SshHost "curl -s -o /dev/null -w '%{http_code}' -m 3 '$streamUrl' 2>/dev/null"
        $streamWorked = ($testCode -eq '200')
    } else {
        try {
            $wr = [System.Net.WebRequest]::Create($streamUrl)
            $wr.Timeout = 3000
            $wr.Method = 'GET'
            $resp = $wr.GetResponse()
            $resp.Close()
            $streamWorked = $true
        } catch { }
    }

    if ($streamWorked) {
        Write-Host "   Mode: streaming" -ForegroundColor DarkGray
        Write-Host ""
        try {
            if ($SshHost) {
                & ssh $SshHost "curl -sN '$streamUrl'" | ForEach-Object {
                    $line = $_ -replace '^data:\s*', ''
                    if ($line.Trim()) { Write-Host "   $line" }
                }
            } else {
                & curl -sN $streamUrl | ForEach-Object {
                    $line = $_ -replace '^data:\s*', ''
                    if ($line.Trim()) { Write-Host "   $line" }
                }
            }
        } catch {
            Write-Warning "Stream ended: $_"
        }
        return
    }

    # --- Fallback: poll /logs endpoint ---
    Write-Host "   Mode: polling every $PollIntervalSecs s" -ForegroundColor DarkGray
    Write-Host ""

    $seen = [System.Collections.Generic.HashSet[string]]::new()

    while ($true) {
        try {
            if ($SshHost) {
                $json = (& ssh $SshHost "curl -s -m 5 '$logsUrl' 2>/dev/null") -join ''
                $resp = if ($json) { $json | ConvertFrom-Json } else { $null }
            } else {
                $resp = Invoke-RestMethod -Uri $logsUrl -TimeoutSec 5 -ErrorAction Stop
            }

            # Accept both JSON {logs:[...]} and plain-text response
            $entries = if ($resp -and $resp.logs) { $resp.logs } `
                       elseif ($resp -is [string]) { $resp -split "`n" } `
                       else { @() }

            foreach ($entry in $entries) {
                $entry = $entry.ToString().Trim()
                if ($entry -and -not $seen.Contains($entry)) {
                    $seen.Add($entry) | Out-Null
                    if ($entry -match 'ERROR|FAIL') {
                        Write-Host "   $entry" -ForegroundColor Red
                    } elseif ($entry -match 'WARN') {
                        Write-Host "   $entry" -ForegroundColor Yellow
                    } else {
                        Write-Host "   $entry" -ForegroundColor DarkGray
                    }
                }
            }
        } catch {
            if ($_.Exception.Message -notmatch 'Unable to connect|timed out|canceled|Timeout') {
                Write-Warning "Poll error: $($_.Exception.Message)"
            }
        }
        Start-Sleep -Seconds $PollIntervalSecs
    }
}

function Get-RemoteDeployLog {
    <#
    .SYNOPSIS
        Retrieve deployment log from remote machine
    #>
    param(
        [ValidateSet("mac", "unraid", "local")]
        [string]$Target = "mac",
        [switch]$Follow
    )
    
    $targetConfig = $Script:Config.Targets[$Target]
    
    # Local target: use local log file
    if (Test-IsLocalTarget -Target $Target) {
        if (Test-Path $targetConfig.LogFile) {
            if ($Follow) {
                Get-Content $targetConfig.LogFile -Wait -Tail 50
            } else {
                Get-Content $targetConfig.LogFile
            }
        } else {
            Write-Warning "Log file not found: $($targetConfig.LogFile)"
        }
        return
    }
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

Write-Host "🚀 Bowie Phone Utilities Loaded" -ForegroundColor Cyan
Write-Host "   Available functions:" -ForegroundColor DarkGray
Write-Host "     • Build-Firmware" -ForegroundColor Green
Write-Host "     • Deploy-ToDevice" -ForegroundColor Green
Write-Host "     • Bump-Version" -ForegroundColor Green
Write-Host "     • Publish-Firmware" -ForegroundColor Green
Write-Host "     • Monitor-SerialOutput" -ForegroundColor Green
Write-Host "     • Monitor-TelnetLogs" -ForegroundColor Green
Write-Host "     • Send-DeviceCommand" -ForegroundColor Green
Write-Host "     • Watch-RemoteLogs" -ForegroundColor Green
Write-Host "     • Get-RemoteDeployLog" -ForegroundColor Green
Write-Host "`n   Type Get-Help <function-name> for details" -ForegroundColor DarkGray
