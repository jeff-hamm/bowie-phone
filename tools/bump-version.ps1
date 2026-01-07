param(
    [string]$NewVersion
)

$root = Split-Path -Parent $PSScriptRoot
$platformioPath = Join-Path $root "platformio.ini"
$manifestPath = Join-Path $root "docs/firmware/manifest.json"
$updateHtmlPath = Join-Path $root "docs/update.html"

function Get-NextPatchVersion {
    param([string]$Current)
    $parts = $Current -split '\.'
    if ($parts.Count -lt 3) {
        throw "Version '$Current' is not in major.minor.patch format."
    }
    $parts[2] = ([int]$parts[2]) + 1
    return "$($parts[0]).$($parts[1]).$($parts[2])"
}

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

# Determine target version
if (-not $NewVersion) {
    $manifest = Get-Content -Path $manifestPath -Raw | ConvertFrom-Json
    $NewVersion = Get-NextPatchVersion -Current $manifest.version
}

# Apply updates
Replace-InFile -Path $platformioPath -Pattern '(-DFIRMWARE_VERSION=\")\d+\.\d+\.\d+(\")' -Replacement "$1$NewVersion$2"
Replace-InFile -Path $manifestPath   -Pattern '(\"version\"\s*:\s*\")\d+\.\d+\.\d+(\")' -Replacement "$1$NewVersion$2"
Replace-InFile -Path $updateHtmlPath -Pattern '(id="version">)\d+\.\d+\.\d+(<)' -Replacement "$1$NewVersion$2"
Replace-InFile -Path $updateHtmlPath -Pattern '(\|\| \')\d+\.\d+\.\d+(\')' -Replacement "$1$NewVersion$2"

Write-Host "Updated version to $NewVersion"