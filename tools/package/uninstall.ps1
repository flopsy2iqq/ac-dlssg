<#
.SYNOPSIS
  Removes the ac-dlssg test build from Assetto Corsa.

.DESCRIPTION
  Part of the test package built by tools\make-test-package.ps1. Runs
  scripts\dev-uninstall.ps1, which undoes what install.ps1 did, using the
  manifest in <game>\ac-dlssg\install: in standalone mode it deletes
  <game>\dxgi.dll (only if it is the bridge that was installed), the
  Streamline files in <game>\ac-dlssg\sl and the CSP Lua app in
  <game>\apps\lua\AcDlssg. The dlssg_for_sm86 files, which the user installs
  himself, are not touched. The logs and ac-dlssg.ini stay in
  <game>\ac-dlssg unless -RemoveData is given. It refuses while acs.exe is
  running. When Windows denies changing the game folder (a game under
  C:\Program Files (x86)), run this from an elevated PowerShell.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
#>
param(
    [string]$GameDir,
    [switch]$RemoveData,
    [switch]$Force,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
$uninstallArgs = @{}
if ($GameDir) { $uninstallArgs.GameDir = $GameDir }
if ($RemoveData) { $uninstallArgs.RemoveData = $true }
if ($Force) { $uninstallArgs.Force = $true }
$code = 1
try {
    & (Join-Path $PSScriptRoot 'scripts\dev-uninstall.ps1') @uninstallArgs
    $code = $LASTEXITCODE
} catch {
    Write-Host "uninstall: FAILED: $($_.Exception.Message)"
}
if (-not $NoPause) {
    try { [void](Read-Host 'Press Enter to close this window') } catch { }
}
exit $code
