<#
.SYNOPSIS
  Installs this ac-dlssg test build into Assetto Corsa.

.DESCRIPTION
  Part of the test package built by tools\make-test-package.ps1; it runs from
  the package folder and needs neither the repository nor any build tool.

  1. Refuses while acs.exe is running, finds the game through Steam's
     libraryfolders.vdf (or -GameDir), and refuses when Windows Smart App
     Control is on, because it blocks the unsigned bridge DLL (in evaluation
     mode it only warns).
  2. Without -StreamlineDir it gets NVIDIA Streamline 2.14.1 with
     scripts\fetch-deps.ps1 into deps\ next to this script: downloaded from
     NVIDIA's GitHub release only when not already there, SHA-256 and NVIDIA
     signatures verified. It lists NVIDIA's license files and asks to accept
     them, unless -AcceptNvidiaLicenses is given.
  3. Runs scripts\dev-install.ps1 with ac-dlssg.dll and the CSP Lua app
     apps\lua\AcDlssg from this folder. -Mode Auto (the default) installs
     standalone (the bridge as <game>\dxgi.dll) when the game has no dxgi.dll,
     and next to ReShade when ReShade is the game's dxgi.dll; it refuses any
     other dxgi.dll. The Lua app goes to <game>\apps\lua\AcDlssg; a folder of
     that name that the install did not create is refused.
  Undo with uninstall.ps1. When Windows denies writing into the game folder
  (a game under C:\Program Files (x86)), run this from an elevated PowerShell.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
#>
param(
    [string]$GameDir,
    [ValidateSet('Auto', 'ReShade', 'Standalone')]
    [string]$Mode = 'Auto',
    [string]$StreamlineDir,
    [switch]$AcceptNvidiaLicenses,
    [switch]$Force,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$scripts = Join-Path $PSScriptRoot 'scripts'
. (Join-Path $scripts 'dev-common.ps1')

function Say([string]$Message) { Write-Host "install: $Message" }

# "Run with PowerShell" closes the window when the script ends.
function Wait-BeforeClose {
    if ($NoPause) { return }
    try { [void](Read-Host 'Press Enter to close this window') } catch { }
}

$code = 1
try {
    Say "ac-dlssg test build from $PSScriptRoot"
    $running = @(Get-RunningGame)
    if ($running.Count -gt 0) {
        Stop-Refused "$($script:AcdbGameExe) is running (pid $(($running | ForEach-Object { $_.Id }) -join ', ')). Close Assetto Corsa first."
    }
    $game = Resolve-GameDir $GameDir
    Say "game folder: $game"

    # Smart App Control blocks unsigned DLLs; a blocked dxgi.dll stops the game from starting.
    $sac = $null
    try {
        $sac = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -Name 'VerifiedAndReputablePolicyState' -ErrorAction Stop).VerifiedAndReputablePolicyState
    } catch { }
    if ($sac -eq 1) {
        Stop-Refused 'Windows Smart App Control is on. It blocks the unsigned ac-dlssg.dll, and the game would not start. Nothing was installed.'
    } elseif ($sac -eq 2) {
        Say 'WARNING: Windows Smart App Control is in evaluation mode. If Windows switches it on later, it blocks the bridge and the game no longer starts; then run uninstall.ps1.'
    }

    if (-not $StreamlineDir) {
        $deps = Join-Path $PSScriptRoot 'deps'
        Say "NVIDIA Streamline 2.14.1 goes to $deps (a download of about 276 MB from github.com/NVIDIA-RTX/Streamline the first time; this can take several minutes without progress output)"
        & (Join-Path $scripts 'fetch-deps.ps1') -Only Streamline -DepsDir $deps
        if ($LASTEXITCODE -ne 0) { Stop-Refused 'getting Streamline failed (see above). Nothing was installed.' }
        $slRoot = Join-Path $deps 'streamline-2.14.1'
        $StreamlineDir = Join-Path $slRoot 'bin\x64'
        Say 'The Streamline DLLs are NVIDIA software under these licenses:'
        foreach ($license in @('license.txt', 'bin\x64\nvngx_dlss.license.txt', 'bin\x64\reflex.license.txt')) {
            Write-Host "  $(Join-Path $slRoot $license)"
        }
        if (-not $AcceptNvidiaLicenses) {
            $answer = Read-Host 'Type y and press Enter to accept them and install; anything else stops here'
            if ("$answer".Trim() -notmatch '^(?i)(y|yes)$') { Stop-Refused 'the NVIDIA licenses were not accepted. Nothing was installed.' }
        }
    }

    $installArgs = @{
        GameDir       = $game
        Dll           = (Join-Path $PSScriptRoot 'ac-dlssg.dll')
        LuaApp        = (Join-Path $PSScriptRoot 'apps\lua\AcDlssg')
        StreamlineDir = $StreamlineDir
        Mode          = $Mode
    }
    if ($Force) { $installArgs.Force = $true }
    & (Join-Path $scripts 'dev-install.ps1') @installArgs
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        Say 'installed. Start the game as usual, drive a few minutes, close it, then run collect-logs.ps1 and send the zip it writes. To undo, run uninstall.ps1.'
    } else {
        Say 'nothing was installed, or it was rolled back (see the lines above).'
    }
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
