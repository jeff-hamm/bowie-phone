<#
.SYNOPSIS
    Build and release a new firmware version for Bowie Phone
.DESCRIPTION
    This script:
    1. Bumps the version number in platformio.ini, manifest.json, and update.json
    2. Builds the firmware with PlatformIO
    3. Copies firmware files to docs/firmware for ESP Web Tools
    4. Commits and pushes the release
.PARAMETER Major
    Bump the major version (X.0.0)
.PARAMETER Minor
    Bump the minor version (x.Y.0)
.PARAMETER Patch
    Bump the patch version (x.y.Z) - default
.PARAMETER ReleaseNotes
    Release notes for this version
.PARAMETER NoPush
    Don't push to remote after committing
.PARAMETER NoBump
    Don't bump version - just rebuild and update files
.PARAMETER DryRun
    Show what would be done without making changes
.EXAMPLE
    .\release.ps1                          # Bump patch version
    .\release.ps1 -Minor                   # Bump minor version
    .\release.ps1 -Major                   # Bump major version
    .\release.ps1 -ReleaseNotes "Fixed OTA updates"
    .\release.ps1 -NoBump                  # Rebuild without bumping version
#>

param(
    [switch]$Major,
    [switch]$Minor,
    [switch]$Patch,
    [string]$ReleaseNotes = "",
    [switch]$NoPush,
    [switch]$NoBump,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot
$PioExe = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"

# Files to update
$PlatformioIni = Join-Path $ProjectRoot "platformio.ini"
$ManifestJson = Join-Path $ProjectRoot "docs\firmware\manifest.json"
$UpdateJson = Join-Path $ProjectRoot "docs\firmware\update.json"
$FirmwareDir = Join-Path $ProjectRoot "docs\firmware"
$BuildDir = Join-Path $ProjectRoot ".pio\build\esp32dev"

# Get current version from manifest.json
function Get-CurrentVersion {
    $manifest = Get-Content $ManifestJson | ConvertFrom-Json
    return $manifest.version
}

# Parse version string into parts
function Parse-Version {
    param([string]$Version)
    $parts = $Version -split '\.'
    return @{
        Major = [int]$parts[0]
        Minor = [int]$parts[1]
        Patch = [int]$parts[2]
    }
}

# Bump version based on flags
function Get-NewVersion {
    param([string]$CurrentVersion)
    
    $v = Parse-Version $CurrentVersion
    
    if ($Major) {
        $v.Major++
        $v.Minor = 0
        $v.Patch = 0
    } elseif ($Minor) {
        $v.Minor++
        $v.Patch = 0
    } else {
        # Default to patch
        $v.Patch++
    }
    
    return "$($v.Major).$($v.Minor).$($v.Patch)"
}

# Update version in platformio.ini
function Update-PlatformioVersion {
    param([string]$NewVersion)
    
    $content = Get-Content $PlatformioIni -Raw
    $content = $content -replace '-DFIRMWARE_VERSION=\\"[0-9]+\.[0-9]+\.[0-9]+\\"', "-DFIRMWARE_VERSION=\`"$NewVersion\`""
    
    if (-not $DryRun) {
        Set-Content $PlatformioIni $content -NoNewline
    }
    Write-Host "  Updated platformio.ini" -ForegroundColor Gray
}

# Update version in manifest.json
function Update-ManifestVersion {
    param([string]$NewVersion)
    
    $manifest = Get-Content $ManifestJson | ConvertFrom-Json
    $manifest.version = $NewVersion
    
    if (-not $DryRun) {
        $manifest | ConvertTo-Json -Depth 10 | Set-Content $ManifestJson
    }
    Write-Host "  Updated manifest.json" -ForegroundColor Gray
}

# Update version in update.json
function Update-UpdateJsonVersion {
    param([string]$NewVersion, [string]$Notes)
    
    $update = Get-Content $UpdateJson | ConvertFrom-Json
    $update.version = $NewVersion
    if ($Notes) {
        $update.release_notes = $Notes
    }
    
    if (-not $DryRun) {
        $update | ConvertTo-Json -Depth 10 | Set-Content $UpdateJson
    }
    Write-Host "  Updated update.json" -ForegroundColor Gray
}

# Build firmware
function Build-Firmware {
    Write-Host "`n📦 Building firmware..." -ForegroundColor Cyan
    
    if ($DryRun) {
        Write-Host "  [DRY RUN] Would run: pio run" -ForegroundColor Yellow
        return $true
    }
    
    Push-Location $ProjectRoot
    try {
        & $PioExe run
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed with exit code $LASTEXITCODE"
        }
        Write-Host "  ✅ Build successful" -ForegroundColor Green
        return $true
    } finally {
        Pop-Location
    }
}

# Copy firmware files to docs/firmware
function Copy-FirmwareFiles {
    Write-Host "`n📋 Copying firmware files..." -ForegroundColor Cyan
    
    $frameworkDir = "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32"
    
    $files = @(
        @{ Source = "firmware.bin"; Dest = "firmware.bin"; Fallback = $null }
        @{ Source = "bootloader.bin"; Dest = "bootloader.bin"; Fallback = "$frameworkDir\tools\sdk\esp32\bin\bootloader_dio_40m.bin" }
        @{ Source = "partitions.bin"; Dest = "partitions.bin"; Fallback = $null }
    )
    
    # boot_app0.bin is in a different location
    $bootApp0Source = "$frameworkDir\tools\partitions\boot_app0.bin"
    
    foreach ($file in $files) {
        $src = Join-Path $BuildDir $file.Source
        $dst = Join-Path $FirmwareDir $file.Dest
        
        # Try build directory first, then fallback
        if (Test-Path $src) {
            if (-not $DryRun) {
                Copy-Item $src $dst -Force
            }
            $size = (Get-Item $src).Length / 1KB
            Write-Host "  Copied $($file.Source) ($([math]::Round($size, 1)) KB)" -ForegroundColor Gray
        } elseif ($file.Fallback -and (Test-Path $file.Fallback)) {
            if (-not $DryRun) {
                Copy-Item $file.Fallback $dst -Force
            }
            $size = (Get-Item $file.Fallback).Length / 1KB
            Write-Host "  Copied $($file.Dest) from framework ($([math]::Round($size, 1)) KB)" -ForegroundColor Gray
        } else {
            # Keep existing file if present
            if (Test-Path $dst) {
                Write-Host "  Using existing $($file.Dest)" -ForegroundColor Gray
            } else {
                Write-Host "  ⚠️ File not found: $($file.Source)" -ForegroundColor Yellow
            }
        }
    }
    
    # Copy boot_app0.bin from framework
    if (Test-Path $bootApp0Source) {
        if (-not $DryRun) {
            Copy-Item $bootApp0Source (Join-Path $FirmwareDir "boot_app0.bin") -Force
        }
        Write-Host "  Copied boot_app0.bin (from framework)" -ForegroundColor Gray
    }
    
    Write-Host "  ✅ Firmware files updated" -ForegroundColor Green
}

# Git commit and push
function Commit-Release {
    param([string]$Version)
    
    Write-Host "`n📤 Committing release..." -ForegroundColor Cyan
    
    Push-Location $ProjectRoot
    try {
        if ($DryRun) {
            Write-Host "  [DRY RUN] Would commit: v$Version" -ForegroundColor Yellow
            return
        }
        
        # Stage firmware files and version changes
        git add platformio.ini
        git add "docs/firmware/*"
        
        # Commit
        $commitMsg = "Release v$Version"
        if ($ReleaseNotes) {
            $commitMsg += "`n`n$ReleaseNotes"
        }
        git commit -m $commitMsg
        
        Write-Host "  ✅ Committed: v$Version" -ForegroundColor Green
        
        if (-not $NoPush) {
            Write-Host "`n🚀 Pushing to remote..." -ForegroundColor Cyan
            git push
            Write-Host "  ✅ Pushed to remote" -ForegroundColor Green
        }
    } finally {
        Pop-Location
    }
}

# Main script
Write-Host "`n🔖 Bowie Phone Release Tool" -ForegroundColor Magenta
Write-Host "=============================" -ForegroundColor Magenta

if ($DryRun) {
    Write-Host "🔍 DRY RUN MODE - No changes will be made" -ForegroundColor Yellow
}

# Get versions
$currentVersion = Get-CurrentVersion
if ($NoBump) {
    $newVersion = $currentVersion
    Write-Host "`n📌 Version: $currentVersion (no bump)" -ForegroundColor Cyan
} else {
    $newVersion = Get-NewVersion $currentVersion
    Write-Host "`n📌 Version: $currentVersion → $newVersion" -ForegroundColor Cyan
}

if ($ReleaseNotes) {
    Write-Host "📝 Notes: $ReleaseNotes" -ForegroundColor Gray
}

# Confirm
if (-not $DryRun) {
    $confirm = Read-Host "`nProceed with release? (y/N)"
    if ($confirm -ne 'y' -and $confirm -ne 'Y') {
        Write-Host "Cancelled." -ForegroundColor Yellow
        exit 0
    }
}

# Update version in all files (skip if NoBump)
if (-not $NoBump) {
    Write-Host "`n✏️ Updating version numbers..." -ForegroundColor Cyan
    Update-PlatformioVersion $newVersion
    Update-ManifestVersion $newVersion
    Update-UpdateJsonVersion $newVersion $ReleaseNotes
} elseif ($ReleaseNotes) {
    Write-Host "`n✏️ Updating release notes..." -ForegroundColor Cyan
    Update-UpdateJsonVersion $newVersion $ReleaseNotes
}

# Build
Build-Firmware

# Copy files
Copy-FirmwareFiles

# Commit and push
Commit-Release $newVersion

Write-Host "`n🎉 Release v$newVersion complete!" -ForegroundColor Green
Write-Host ""
