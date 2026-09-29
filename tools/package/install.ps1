<#
.SYNOPSIS
  Installs this ac-dlssg test build into Assetto Corsa.

.DESCRIPTION
  Part of the test package built by tools\make-test-package.ps1, where it is
  scripts\install.ps1 and install.bat in the package root starts it; it needs
  neither the repository nor any build tool. It asks nothing: it prints what
  it finds and decides, and runs to the end. The package layout:
    install.bat          starts this script
    files\               ac-dlssg.dll, apps\lua\AcDlssg, and deps\ for the
                         downloads
    scripts\             this script and the ones it runs
    tools\               uninstall.bat, collect-logs.bat, README-test.txt

  1. Refuses while acs.exe is running and finds the game through Steam's
     libraryfolders.vdf (or -GameDir). When this account may not write into
     the game folder (a game under C:\Program Files (x86)), it starts itself
     again with administrator rights, once, and that window goes on; the
     Windows UAC prompt is the only question. It refuses when Windows Smart
     App Control is on, because that blocks the unsigned bridge DLL (in
     evaluation mode it only warns).
  2. Without -StreamlineDir it gets NVIDIA Streamline 2.14.1 with
     scripts\fetch-deps.ps1 into files\deps\: downloaded from NVIDIA's GitHub
     release only when not already there, SHA-256 and NVIDIA signatures
     verified. It prints where NVIDIA's license files are; installing means
     accepting them.
  3. dlssg_for_sm86 (spec 10), unless -NoSpoof is given. It reads every
     display adapter (the PNPDeviceID of each Win32_VideoController) and
     looks only at the NVIDIA ones:
       - RTX 30 (Ampere SM86, desktop or laptop): it prints what
         dlssg_for_sm86 is, then gets dlssg_for_sm86 0.3.5 (version.dll and
         dlssg_sm86.ini of commit 9621db5 of github.com/sdli1995/dlssg_for_sm86)
         with scripts\fetch-deps.ps1 into files\deps\, checks their git blob SHA-1
         (version.dll also its size and SHA-256), and has dev-install.ps1 put
         them next to acs.exe. A pinned 0.3.5 file that is already in the game
         folder is not downloaded again; it is recorded as found, and
         uninstall.ps1 leaves it. Any other version.dll (another mod, another
         dlssg_for_sm86 version) is left untouched with a warning, and the
         spoof is not installed. A download that fails its checks is deleted
         and stops the install with nothing changed.
       - RTX 40 and RTX 50: nothing is needed.
       - RTX 20: frame generation is not supported there; no spoof.
  4. Runs scripts\dev-install.ps1 -AutoUpgrade with files\ac-dlssg.dll and the
     CSP Lua app files\apps\lua\AcDlssg. -Mode Auto (the default) installs
     standalone (the bridge as <game>\dxgi.dll) when the game has no dxgi.dll,
     and next to ReShade when ReShade is the game's dxgi.dll; it refuses any
     other dxgi.dll. The Lua app goes to <game>\apps\lua\AcDlssg; a folder of
     that name that no install of this project created is refused.
     Over an earlier install (any older package, any older manifest) it is an
     upgrade that needs nothing from the user: files the manifest records as
     ours are replaced even when they were changed (a copy of a changed one
     stays in <game>\ac-dlssg\install\backup), files the new build no longer
     ships are removed, and when the mode has to change (ReShade was
     installed or removed since) the old mode's files are undone and the new
     mode installed in the same run.
  Undo with tools\uninstall.bat.

  The last line is "Press Enter to exit", so that a double-clicked window
  stays open; not with -NoPause or when the input is redirected.

.PARAMETER NoSpoof
  Skip step 3: no GPU check, no dlssg_for_sm86 download or install. Spoof
  files an earlier run installed stay, and uninstall.ps1 still removes them.

.PARAMETER NoElevate
  For tests: when the game folder is not writable, refuse instead of
  starting again with administrator rights.

.PARAMETER Elevated
  Set by the script itself on the elevated run, so that it restarts only once.

.PARAMETER GpuDeviceIds
  For tests: the display adapters as PCI vendor:device IDs in hex (such as
  10DE:2206, or "8086:9A49,10DE:25A0" for two) instead of the ones Windows
  reports.

.PARAMETER SpoofSourceDir
  For tests: a folder whose version.dll and dlssg_sm86.ini stand in for the
  download; they go through the same checks.

.PARAMETER SpoofPins
  For tests: a JSON file with the pins of the -SpoofSourceDir files (see
  Get-SpoofPins in scripts\dev-common.ps1).

.EXAMPLE
  install.bat

.EXAMPLE
  install.bat -NoSpoof
#>
param(
    [string]$GameDir,
    [ValidateSet('Auto', 'ReShade', 'Standalone')]
    [string]$Mode = 'Auto',
    [string]$StreamlineDir,
    [switch]$NoSpoof,
    [switch]$NoPause,
    [switch]$NoElevate,
    [switch]$Elevated,
    [string[]]$GpuDeviceIds,
    [string]$SpoofSourceDir,
    [string]$SpoofPins
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$scripts = $PSScriptRoot
$packageRoot = Split-Path -Parent $PSScriptRoot
$filesDir = Join-Path $packageRoot 'files'
$depsDir = Join-Path $filesDir 'deps'
. (Join-Path $scripts 'dev-common.ps1')

function Say([string]$Message) { Write-Host "install: $Message" }

# The last line: a double-clicked window stays open until Enter. Never with
# -NoPause or with redirected input.
function Wait-BeforeClose {
    if ($NoPause) { return }
    try { if ([Console]::IsInputRedirected) { return } } catch { return }
    try { [void](Read-Host 'Press Enter to exit') } catch { }
}

function Get-GpuArchText($Adapter) {
    switch ($Adapter.Arch) {
        'Ampere' { if ($Adapter.Sm86) { return 'RTX 30, Ampere SM86' } else { return 'Ampere SM80 (A100)' } }
        'Ada' { return 'RTX 40, Ada' }
        'Blackwell' { return 'RTX 50, Blackwell' }
        'Turing' { return 'RTX 20 or GTX 16, Turing' }
        'OlderNvidia' { return 'older than RTX 20' }
    }
    return 'not in this installer''s GPU table'
}

# Printed before the download, from spec 10; the install goes on without a question.
function Write-SpoofNotice {
    $v = $script:AcdbSpoofVersion
    Say ('This PC has an RTX 30 GPU. NVIDIA allows DLSS Frame Generation only on RTX 40 and newer; on RTX 30 it runs ' +
        "through dlssg_for_sm86, which the installer now downloads and puts next to acs.exe:")
    Write-Host "  - What: dlssg_for_sm86 $v, the files version.dll and dlssg_sm86.ini of commit $($script:AcdbSpoofCommit)"
    Write-Host "    (tag $v) of https://github.com/$($script:AcdbSpoofRepository), checked against their pinned hashes"
    Write-Host '    before anything is installed.'
    Write-Host '  - Who: a third-party project by sdli1995, which includes Coldwood1026''s RTX 20 work. It is not part of'
    Write-Host '    ac-dlssg and not NVIDIA software.'
    Write-Host '  - License: the repository has no LICENSE file; its README says the project source is GPLv3, but no source'
    Write-Host '    is published.'
    Write-Host '  - NVIDIA: the binary embeds NVIDIA''s nvngx_dlssg.dll, which is not relicensed. Running it on RTX 30'
    Write-Host '    circumvents a technical limitation, which section 4.d of the NVIDIA RTX SDKs License forbids, and you'
    Write-Host '    are that license''s licensee.'
    Write-Host '  - Risk: it runs inside the game and changes the GPU architecture that NVIDIA''s driver interface reports'
    Write-Host '    to it (CSP sees that too). If the game misbehaves, tools\uninstall.bat removes it again.'
    Write-Host '  - To install without it: install.bat -NoSpoof.'
}

# Step 3: the dev-install.ps1 arguments for dlssg_for_sm86 (none when there
# is nothing to install or record). Downloads what is missing; refuses when
# a download fails its checks.
function Get-SpoofInstallArgs([string]$Game) {
    $none = @{}
    if ($NoSpoof) {
        Say 'dlssg_for_sm86: skipped (-NoSpoof).'
        return $none
    }
    try {
        $adapters = @(Get-GpuAdapters $GpuDeviceIds)
    } catch {
        if (Test-IsRefusal $_) { throw }
        Say "WARNING: the GPUs could not be read ($($_.Exception.Message)), so dlssg_for_sm86 is not installed. On an RTX 30, run install.ps1 again."
        return $none
    }
    $nvidia = @($adapters | Where-Object { $_.VendorId -eq $script:AcdbNvidiaVendorId })
    foreach ($a in $nvidia) { Say "GPU: $($a.Name) (PCI $($a.Id), $(Get-GpuArchText $a))" }
    if ($nvidia.Count -eq 0) {
        Say 'no NVIDIA GPU found. DLSS Frame Generation needs an NVIDIA RTX GPU; dlssg_for_sm86 is not installed.'
        return $none
    }
    if (@($nvidia | Where-Object { $_.Arch -eq 'Ada' -or $_.Arch -eq 'Blackwell' }).Count -gt 0) {
        Say 'RTX 40 or newer: DLSS Frame Generation runs on it natively; dlssg_for_sm86 is not needed and not installed.'
        return $none
    }
    if (@($nvidia | Where-Object { $_.Sm86 }).Count -eq 0) {
        if (@($nvidia | Where-Object { $_.Arch -eq 'Turing' }).Count -gt 0) {
            Say ('RTX 20 is not supported: frame generation does not run on it, and dlssg_for_sm86 is not installed. ' +
                'The bridge is installed anyway; without frame generation the game runs as it does without it.')
        } else {
            Say 'no RTX 30 (Ampere SM86) GPU: dlssg_for_sm86 is not installed.'
        }
        return $none
    }

    $what = "$($script:AcdbSpoofProject) $($script:AcdbSpoofVersion)"
    $pins = Get-SpoofPins $SpoofPins
    $state = @{}
    foreach ($n in $script:AcdbSpoofNames) {
        $path = Join-Path $Game $n
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $state[$n] = 'absent' }
        elseif (-not (Get-SpoofFileProblem $path $pins[$n])) { $state[$n] = 'pinned' }
        else { $state[$n] = 'other' }
    }
    $versionDll = Join-Path $Game 'version.dll'
    if ($state['version.dll'] -eq 'other') {
        Say ("WARNING: $versionDll is not $what (SHA-256 $(Get-Sha256OfFile $versionDll)); it is another mod's " +
            'version.dll or another dlssg_for_sm86 version. It was left untouched, and dlssg_for_sm86 was not installed. ' +
            'Frame generation runs on this RTX 30 only if that file is a dlssg_for_sm86 that supports it. To get ' +
            "$what instead, remove version.dll and dlssg_sm86.ini from the game folder yourself and run install.ps1 again.")
        return $none
    }
    if ($state['dlssg_sm86.ini'] -eq 'other') {
        Say "note: $Game\dlssg_sm86.ini differs from the $what default; it is kept as it is."
    }
    $result = @{}
    $found = @($script:AcdbSpoofNames | Where-Object { $state[$_] -eq 'pinned' })
    if ($found.Count -gt 0) {
        Say "$what`: $($found -join ' and ') already in the game folder as the pinned version; not downloaded and not replaced."
        $result.SpoofFound = $found
    }
    $fetch = @($script:AcdbSpoofNames | Where-Object { $state[$_] -eq 'absent' })
    if ($fetch.Count -gt 0) {
        Write-SpoofNotice
        $deps = $depsDir
        $fetchArgs = @{ Only = 'Spoof'; SpoofFiles = $fetch; DepsDir = $deps }
        if ($SpoofSourceDir) { $fetchArgs.SpoofSourceDir = $SpoofSourceDir }
        if ($SpoofPins) { $fetchArgs.SpoofPins = $SpoofPins }
        & (Join-Path $scripts 'fetch-deps.ps1') @fetchArgs
        if ($LASTEXITCODE -ne 0) { Stop-Refused "getting $what failed (see above). Nothing was installed." }
        $staged = Join-Path $deps $script:AcdbSpoofDirName
        $files = @()
        foreach ($n in $fetch) {
            $file = Join-Path $staged $n
            $problem = Get-SpoofFileProblem $file $pins[$n]
            if ($problem) { Stop-Refused "the staged $problem. Nothing was installed." }
            $files += $file
        }
        $result.SpoofInstall = $files
    }
    return $result
}

$code = 1
try {
    Say "ac-dlssg test build from $packageRoot"
    $running = @(Get-RunningGame)
    if ($running.Count -gt 0) {
        Stop-Refused "$($script:AcdbGameExe) is running (pid $(($running | ForEach-Object { $_.Id }) -join ', ')). Close Assetto Corsa first."
    }
    $game = Resolve-GameDir $GameDir
    Say "game folder: $game"

    # A game under Program Files: start again with administrator rights, once;
    # that window shows the result and waits for Enter.
    if (-not (Test-DirWritable $game)) {
        if ($Elevated -or (Test-IsAdministrator)) {
            Stop-Refused "Windows does not let even an administrator write into $game. Check that folder's permissions. Nothing was installed."
        }
        if ($NoElevate) {
            Stop-Refused "this account may not write into $game, and -NoElevate keeps the installer from starting again with administrator rights. Nothing was installed."
        }
        Say "this account may not write into $game. The installer starts again with administrator rights: confirm the Windows prompt, and follow the new window."
        try {
            $code = Invoke-ElevatedScript $PSCommandPath (Get-ForwardArguments $PSBoundParameters @{ GameDir = $game; Elevated = $true })
        } catch {
            Stop-Refused "the installer did not get administrator rights ($($_.Exception.Message)). Nothing was installed."
        }
        Say "the installer with administrator rights ended with exit code $code."
        exit $code
    }

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

    $licenses = @()
    if (-not $StreamlineDir) {
        $deps = $depsDir
        Say "NVIDIA Streamline 2.14.1 goes to $deps (a download of about 276 MB from github.com/NVIDIA-RTX/Streamline the first time; this can take several minutes without progress output)"
        & (Join-Path $scripts 'fetch-deps.ps1') -Only Streamline -DepsDir $deps
        if ($LASTEXITCODE -ne 0) { Stop-Refused 'getting Streamline failed (see above). Nothing was installed.' }
        $slRoot = Join-Path $deps 'streamline-2.14.1'
        $StreamlineDir = Join-Path $slRoot 'bin\x64'
        $licenses = @(foreach ($license in @('license.txt', 'bin\x64\nvngx_dlss.license.txt', 'bin\x64\reflex.license.txt')) { Join-Path $slRoot $license })
    } elseif (Test-Path -LiteralPath $StreamlineDir -PathType Container) {
        $licenses = @(Get-ChildItem -LiteralPath $StreamlineDir -File -Filter '*license*' | Sort-Object Name | ForEach-Object { $_.FullName })
    }
    if ($licenses.Count -gt 0) {
        Say 'The Streamline DLLs are NVIDIA software under the licenses in these files; installing them means you accept these licenses:'
        foreach ($license in $licenses) { Write-Host "  $license" }
    }

    # -AutoUpgrade: an earlier install is upgraded without questions (see 4.).
    $installArgs = @{
        GameDir       = $game
        Dll           = (Join-Path $filesDir 'ac-dlssg.dll')
        LuaApp        = (Join-Path $filesDir 'apps\lua\AcDlssg')
        StreamlineDir = $StreamlineDir
        Mode          = $Mode
        AutoUpgrade   = $true
    }
    $spoofArgs = Get-SpoofInstallArgs $game
    foreach ($k in @($spoofArgs.Keys)) { $installArgs[$k] = $spoofArgs[$k] }
    & (Join-Path $scripts 'dev-install.ps1') @installArgs
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        Say 'installed. Start the game as usual, drive a few minutes, close it, then run tools\collect-logs.bat and send the zip it writes. To undo, run tools\uninstall.bat.'
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
