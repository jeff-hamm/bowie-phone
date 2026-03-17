#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Add this machine as a WireGuard peer in the bowie-phone subnet.

.DESCRIPTION
    SSHes to the Unraid server, calls wg-add-peer, and saves the resulting
    WireGuard client config. If wg.exe is available, the keypair is generated
    locally so the private key never leaves this machine.

.PARAMETER Server
    Server IP or hostname (default: 100.86.189.46).

.PARAMETER Name
    Friendly name for this peer (default: this computer's hostname).

.PARAMETER IP
    Specific IP to assign, e.g. 10.253.0.10. Auto-assigned if omitted.

.PARAMETER SshKey
    SSH private key for authentication (default: ~/.ssh/id_rsa).

.PARAMETER OutDir
    Directory to save the .conf file (default: ~/Documents/WireGuard).

.EXAMPLE
    .\tools\wg-join.ps1
    .\tools\wg-join.ps1 -Name jumpbox -IP 10.253.0.10
#>
[CmdletBinding()]
param(
    [string]$Server = "100.86.189.46",
    [string]$Name   = $env:COMPUTERNAME.ToLower(),
    [string]$IP     = "",
    [string]$SshKey = (Join-Path $env:USERPROFILE ".ssh\id_rsa"),
    [string]$OutDir = (Join-Path $env:USERPROFILE "Documents\WireGuard")
)

$ErrorActionPreference = 'Stop'

# --- locate wg.exe for local keypair generation ---
$wgExe = (Get-Command "wg" -ErrorAction SilentlyContinue)?.Source ??
         ((Test-Path "C:\Program Files\WireGuard\wg.exe") ? "C:\Program Files\WireGuard\wg.exe" : $null)

# --- build remote command (prepend .local/bin since zsh SSH sessions skip it) ---
$remoteCmd = "wg-add-peer --name '$Name'"
if ($IP) { $remoteCmd += " --ip '$IP'" }
$remoteCmd = 'PATH=/mnt/pool/appdata/home/.local/bin:$PATH ' + $remoteCmd

$privKey = $null
if ($wgExe) {
    $privKey = & $wgExe genkey
    $pubKey  = $privKey | & $wgExe pubkey
    $remoteCmd += " --pubkey '$pubKey'"
    Write-Host "Keypair generated locally (private key stays on this machine)."
} else {
    Write-Host "wg.exe not found — server will generate keypair."
}

# --- call server ---
Write-Host "Connecting to $Server..."
[string[]]$lines = ssh -i $SshKey "root@$Server" $remoteCmd
if ($LASTEXITCODE -ne 0) { throw "wg-add-peer failed on server (exit $LASTEXITCODE)." }

# --- inject locally-generated PrivateKey after [Interface] ---
if ($privKey) {
    $out = [System.Collections.Generic.List[string]]::new()
    foreach ($line in $lines) {
        $out.Add($line)
        if ($line -match '^\[Interface\]') { $out.Add("PrivateKey = $privKey") }
    }
    $lines = $out.ToArray()
}

# --- save config ---
$null = New-Item -ItemType Directory -Force -Path $OutDir
$confPath = Join-Path $OutDir "$Name.conf"
$lines | Set-Content $confPath -Encoding UTF8
Write-Host "Config saved: $confPath"

# --- firewall: allow inbound TCP from WireGuard subnet (espota callback) ---
$fwRule = Get-NetFirewallRule -DisplayName "espota-inbound" -ErrorAction SilentlyContinue
if (-not $fwRule) {
    $null = Start-Process powershell -Verb RunAs -Wait -ArgumentList (
        "-NoProfile -Command New-NetFirewallRule " +
        "-DisplayName 'espota-inbound' " +
        "-Direction Inbound -Protocol TCP -LocalPort 1024-65535 " +
        "-RemoteAddress 10.253.0.0/24 -Action Allow -Profile Any"
    )
    Write-Host "Firewall rule 'espota-inbound' created (allows callback from WireGuard subnet)."
} else {
    Write-Host "Firewall rule 'espota-inbound' already exists."
}

# --- optionally install tunnel service ---
$wgApp = "C:\Program Files\WireGuard\wireguard.exe"
if (Test-Path $wgApp) {
    $answer = Read-Host "Install and activate tunnel '$Name'? [Y/n]"
    if ($answer -notin @('n', 'N')) {
        Start-Process $wgApp -ArgumentList "/installtunnelservice `"$confPath`"" -Verb RunAs -Wait
        Write-Host "Tunnel service installed. It will start automatically on boot."
    }
} else {
    Write-Host ""
    Write-Host "WireGuard not installed. Get it from: https://www.wireguard.com/install/"
    Write-Host "Then import: $confPath"
}
