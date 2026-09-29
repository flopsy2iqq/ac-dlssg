<#
.SYNOPSIS
  Removes the ac-dlssg test build from Assetto Corsa.

.DESCRIPTION
  Part of the test package built by tools\make-test-package.ps1, where it is
  scripts\uninstall.ps1 and tools\uninstall.bat starts it. It asks nothing.
  When this account may not write into the game folder (a game under
  C:\Program Files (x86)), it starts itself again with administrator rights,
  once, and that window goes on; the Windows UAC prompt is the only question.

  It runs scripts\dev-uninstall.ps1, which undoes what install.ps1 did, using
  the manifest in <game>\ac-dlssg\install: in standalone mode it deletes
  <game>\dxgi.dll (only if it is the bridge that was installed; any other
  dxgi.dll, such as ReShade installed over it, stays and the rest is removed),
  the Streamline files in <game>\ac-dlssg\sl, the CSP Lua app in
  <game>\apps\lua\AcDlssg, and the dlssg_for_sm86 files (version.dll,
  dlssg_sm86.ini) that install.ps1 put next to acs.exe, each only while it is
  the file installed. dlssg_for_sm86 files that were there before the install
  stay. The logs and ac-dlssg.ini stay in <game>\ac-dlssg unless -RemoveData
  is given; -RemoveData also deletes dlssg_for_sm86's data (<game>\dlssg_sm86
  and %LOCALAPPDATA%\DlssgSm86) when install.ps1 installed it and no
  version.dll is left. It refuses while acs.exe is running.

  The last line is "Press Enter to exit", so that a double-clicked window
  stays open; not with -NoPause or when the input is redirected.

.PARAMETER NoElevate
  For tests: when the game folder is not writable, refuse instead of
  starting again with administrator rights.

.PARAMETER Elevated
  Set by the script itself on the elevated run, so that it restarts only once.

.EXAMPLE
  tools\uninstall.bat

.EXAMPLE
  tools\uninstall.bat -RemoveData
#>
param(
    [string]$GameDir,
    [switch]$RemoveData,
    [switch]$NoPause,
    [switch]$NoElevate,
    [switch]$Elevated
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'dev-common.ps1')

function Say([string]$Message) { Write-Host "uninstall: $Message" }

# The last line: a double-clicked window stays open until Enter. Never with
# -NoPause or with redirected input.
function Wait-BeforeClose {
    if ($NoPause) { return }
    try { if ([Console]::IsInputRedirected) { return } } catch { return }
    try { [void](Read-Host 'Press Enter to exit') } catch { }
}

$code = 1
try {
    $game = Resolve-GameDir $GameDir
    if (-not (Test-DirWritable $game)) {
        if ($Elevated -or (Test-IsAdministrator)) {
            Stop-Refused "Windows does not let even an administrator change $game. Check that folder's permissions. Nothing was changed."
        }
        if ($NoElevate) {
            Stop-Refused "this account may not change $game, and -NoElevate keeps the uninstaller from starting again with administrator rights. Nothing was changed."
        }
        Say "this account may not change $game. The uninstaller starts again with administrator rights: confirm the Windows prompt, and follow the new window."
        try {
            $code = Invoke-ElevatedScript $PSCommandPath (Get-ForwardArguments $PSBoundParameters @{ GameDir = $game; Elevated = $true })
        } catch {
            Stop-Refused "the uninstaller did not get administrator rights ($($_.Exception.Message)). Nothing was changed."
        }
        Say "the uninstaller with administrator rights ended with exit code $code."
        exit $code
    }
    # -Force: a dxgi.dll that is not the recorded bridge stays, the rest goes.
    $uninstallArgs = @{ GameDir = $game; Force = $true }
    if ($RemoveData) { $uninstallArgs.RemoveData = $true }
    & (Join-Path $PSScriptRoot 'dev-uninstall.ps1') @uninstallArgs
    $code = $LASTEXITCODE
} catch {
    $code = 1
    if (Test-IsRefusal $_) {
        Say "REFUSED: $($_.Exception.Message)"
    } else {
        Say "FAILED: $($_.Exception.Message)"
        if (Test-AccessDenied $_) { Write-AccessDeniedHint $null }
    }
}
Wait-BeforeClose
exit $code
