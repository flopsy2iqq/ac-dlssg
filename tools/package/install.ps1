<#
.SYNOPSIS
  Installs this ac-dlssg test build into Assetto Corsa.

.DESCRIPTION
  Part of the test package built by tools\make-test-package.ps1, where it is
  scripts\install.ps1 and install.bat in the package root starts it; it needs
  neither the repository nor any build tool. It asks nothing: it prints what
  it finds and decides, and runs to the end. The package layout:
    install.bat          starts this script
    files\               ac-dlssg.dll, apps\lua\AcDlssg, ac-dlssg\ (the
                         uninstaller and the log collector for the game
                         folder), and deps\ for the downloads
    scripts\             this script and the ones it runs
    tools\               uninstall.bat, collect-logs.bat
    docs\                READMEs, LICENSE, EXCEPTIONS.md, THIRD_PARTY_NOTICES.txt,
                         README-test.txt

  1. Refuses while acs.exe is running and finds the game through Steam's
     libraryfolders.vdf (or -GameDir). When this account may not write into
     the game folder (a game under C:\Program Files (x86)), it starts itself
     again with administrator rights, once, and that window goes on; the
     Windows UAC prompt is the only question. It refuses when Windows Smart
     App Control is on, because that blocks the unsigned bridge DLL (in
     evaluation mode it only warns).
  2. Without -StreamlineDir it gets NVIDIA Streamline 2.14.1. An earlier
     install's copy in <game>\ac-dlssg\sl is used again when each file the
     install copies is there with its pinned SHA-256 and each DLL carries
     NVIDIA's signature (the checks a download gets; the pins are in
     scripts\dev-common.ps1): the files are copied into
     files\deps\streamline-2.14.1-installed, checked again there, installed
     from there (they are already this version, so nothing in the game
     folder is rewritten), and that copy is deleted afterwards; it says
     "Streamline 2.14.1 is already installed and verified; not downloaded
     again". Otherwise scripts\fetch-deps.ps1 gets it into files\deps\:
     downloaded from NVIDIA's GitHub release only when not already there,
     SHA-256 and NVIDIA signatures verified. It prints where NVIDIA's license
     files are; installing means accepting them.
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
         uninstall.ps1 leaves it (one an earlier install put there stays
         recorded as installed). Any other version.dll (another mod, another
         dlssg_for_sm86 version), also one put over the version.dll an
         earlier install put there, is left untouched with a warning, and
         the spoof is not installed (the earlier record stays). A download
         that fails its checks is deleted and stops the install with nothing
         changed.
       - RTX 40 and RTX 50: no spoof is needed (RTX 40 only: see 3b).
       - RTX 20: frame generation is not supported there; no spoof.
     3b. MFG-Unlocker, unless -NoMfgUnlocker is given, only when every NVIDIA
     adapter is an RTX 40 (Ada; no RTX 30 and no RTX 50 next to it). NVIDIA
     allows 2X there and 3X/4X only on RTX 50; MFG-Unlocker lifts that. It
     prints what MFG-Unlocker is, then gets the pinned release (the asset
     MFGUnlocker-v1.5.0.zip of tag v1.5.0 of github.com/dashdogy/MFG-Unlocker)
     with scripts\fetch-deps.ps1 into files\deps\, checks the zip's size and
     SHA-256 and the SHA-256 of MFGUnlocker.dll in it, and has dev-install.ps1
     put that DLL into <game>\ac-dlssg\mfg\version.dll (never next to
     acs.exe), where the bridge loads it. A copy that is already there as the
     pinned version is not downloaded again. A download that fails or does
     not match is deleted with a warning, and everything else is installed
     (frame generation stays at 2X). It is not installed, with a warning, next
     to dlssg_for_sm86's version.dll, next to an MFG-Unlocker the user
     installed (a proxy name next to acs.exe that exports
     MfgUnlockSingleModuleQuery, or the file names of its builds), or over a
     file in ac-dlssg\mfg that no install put there. Next to another mod's
     version.dll it is installed with a warning: when Windows loads that
     file (Windows 10 does), the bridge does not load MFG-Unlocker. On any
     other PC, with -NoMfgUnlocker, or next to a user's copy, a copy an
     earlier install put into ac-dlssg\mfg is removed (only while it is the
     file installed).
  4. Runs scripts\dev-install.ps1 -AutoUpgrade with files\ac-dlssg.dll, the
     CSP Lua app files\apps\lua\AcDlssg and, as -Tools, files\ac-dlssg: the
     uninstaller and the log collector go into <game>\ac-dlssg
     (uninstall.bat, collect-logs.bat, and the scripts they run in
     scripts\), so that the unpacked package and its zip can be deleted
     after the install. -Mode Auto (the default) installs
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
  Undo with <game>\ac-dlssg\uninstall.bat (or this package's
  tools\uninstall.bat).

  The last line is "Press Enter to exit", so that a double-clicked window
  stays open; not with -NoPause or when the input is redirected.

.PARAMETER NoSpoof
  Skip step 3: no GPU check, no dlssg_for_sm86 download or install. Spoof
  files an earlier run installed stay, and uninstall.ps1 still removes them.

.PARAMETER NoMfgUnlocker
  Skip step 3b: no MFG-Unlocker download or install, and the copy an earlier
  run put into <game>\ac-dlssg\mfg is removed.

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

.PARAMETER MfgSourceDir
  For tests: a folder whose MFGUnlocker-v1.5.0.zip stands in for the
  download; it goes through the same checks.

.PARAMETER MfgPins
  For tests: a JSON file with the pins of that zip (see Get-MfgPins in
  scripts\dev-common.ps1).

.EXAMPLE
  install.bat

.EXAMPLE
  install.bat -NoSpoof

.EXAMPLE
  install.bat -NoMfgUnlocker
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
    [string]$SpoofPins,
    [switch]$NoMfgUnlocker,
    [string]$MfgSourceDir,
    [string]$MfgPins
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

# The uninstaller and the log collector that the install puts into the game folder.
function Get-InstalledTool([string]$Game, [string]$Name) { return Join-Path (Join-Path $Game $script:AcdbDataDirName) $Name }

# The installed Streamline of an earlier install, copied into $To and
# checked there with the checks a download gets: the folder to install
# from, or the reason it cannot be used (then $To is gone again). The copy
# keeps dev-install.ps1 from reading and writing the same files.
function Copy-InstalledStreamline([string]$Game, [string]$To) {
    if (Test-Path -LiteralPath $To) { Remove-Item -LiteralPath $To -Recurse -Force }
    $data = Join-Path $Game $script:AcdbDataDirName
    $from = Join-Path $data $script:AcdbSlDirName
    $problem = ''
    if (-not (Test-Path -LiteralPath (Join-Path $data 'install\dev-manifest.json') -PathType Leaf)) { $problem = 'there is no install to take them from' }
    if (-not $problem) { $problem = Get-StreamlineRuntimeProblem $from }
    if (-not $problem) {
        New-Item -ItemType Directory -Path $To -Force | Out-Null
        foreach ($name in $script:AcdbSlRuntimeSha256.Keys) { Copy-Item -LiteralPath (Join-Path $from $name) -Destination (Join-Path $To $name) }
        $problem = Get-StreamlineRuntimeProblem $To
        if ($problem) { $problem = "their copy in $To`: $problem" }
    }
    if ($problem) {
        if (Test-Path -LiteralPath $To) { Remove-Item -LiteralPath $To -Recurse -Force }
        return [pscustomobject]@{ Dir = $null; From = $from; Problem = $problem }
    }
    return [pscustomobject]@{ Dir = $To; From = $from; Problem = '' }
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
function Write-SpoofNotice([string]$Game) {
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
    Write-Host "    to it (CSP sees that too). If the game misbehaves, $(Get-InstalledTool $Game 'uninstall.bat') removes it again."
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
        Say "WARNING: the GPUs could not be read ($($_.Exception.Message)), so dlssg_for_sm86 is not installed. On an RTX 30, run install.bat again."
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
            "$what instead, remove version.dll and dlssg_sm86.ini from the game folder yourself and run install.bat again.")
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
        Write-SpoofNotice $Game
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

# Printed before the MFG-Unlocker download; the install goes on without a question.
function Write-MfgNotice([string]$Game) {
    $v = $script:AcdbMfgPin.Version
    Say ('This PC has RTX 40 GPUs only. NVIDIA allows DLSS Frame Generation 2X on RTX 40 and enables 3X/4X only on ' +
        'RTX 50. MFG-Unlocker lifts that limit; the installer now downloads it and puts it into the ac-dlssg folder:')
    Write-Host "  - What: MFG-Unlocker $v, the file MFGUnlocker.dll of the release asset $(Get-MfgAsset) of"
    Write-Host "    https://github.com/$($script:AcdbMfgRepository) (tag $(Get-MfgTag)), checked against its pinned SHA-256 before"
    Write-Host "    anything is installed. It goes into $(Join-Path $Game $script:AcdbMfgRel), not next to acs.exe;"
    Write-Host '    the bridge loads it.'
    Write-Host '  - Who: a third-party project by dashdogy. It is not part of ac-dlssg and not an NVIDIA project.'
    Write-Host '  - License: its code is under the MIT license; it also contains data derived from Intel XeSS, which'
    Write-Host '    stays under Intel''s terms.'
    Write-Host '  - NVIDIA: it lifts NVIDIA''s restriction of multi frame generation (3X/4X) to RTX 50. That circumvents'
    Write-Host '    a technical limitation, which section 4.d of the NVIDIA RTX SDKs License forbids, and you are that'
    Write-Host '    license''s licensee.'
    Write-Host '  - How: it patches NVIDIA''s DLSS-G code (sl.dlss_g.dll, nvngx_dlssg.dll) in memory while the game runs;'
    Write-Host '    no file on disk is changed.'
    Write-Host '  - Its own menu (Backspace) should stay on "Follow game", so that the 2X/3X/4X buttons of the ac-dlssg'
    Write-Host '    window decide.'
    Write-Host '  - The game may take longer to load with it.'
    Write-Host '  - To install without it: install.bat -NoMfgUnlocker (that also removes it again). mfg_unlocker=0 in'
    Write-Host "    $(Join-Path $Game "$($script:AcdbDataDirName)\ac-dlssg.ini") keeps it from loading."
}

# The SHA-256 of the MFG-Unlocker copy an earlier install put into
# ac-dlssg\mfg, as its manifest records it; $null when there is none.
function Get-RecordedMfgHash([string]$Game) {
    $path = Join-Path $Game "$($script:AcdbDataDirName)\install\dev-manifest.json"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    try { $m = Read-Manifest $path } catch { return $null }
    if (-not $m.PSObject.Properties['mfgUnlocker'] -or -not $m.mfgUnlocker) { return $null }
    $rec = @($m.mfgUnlocker.files | Where-Object { [string]$_.path -ieq $script:AcdbMfgRel -and [string]$_.origin -eq 'installed' })
    if ($rec.Count -ne 1) { return $null }
    return [string]$rec[0].sha256
}

# Step 3b: the dev-install.ps1 arguments for MFG-Unlocker. MfgRemove takes
# out the copy an earlier install put there; a failed download is a warning.
function Get-MfgInstallArgs([string]$Game) {
    $none = @{}
    $remove = @{ MfgRemove = $true }
    $recorded = Get-RecordedMfgHash $Game
    $ours = if ($recorded) { $remove } else { $none }
    $removed = if ($recorded) { ' The copy an earlier install put into ac-dlssg\mfg is removed.' } else { '' }
    if ($NoMfgUnlocker) {
        Say "MFG-Unlocker: skipped (-NoMfgUnlocker).$removed"
        return $remove
    }
    try {
        $adapters = @(Get-GpuAdapters $GpuDeviceIds)
    } catch {
        if (Test-IsRefusal $_) { throw }
        Say "WARNING: the GPUs could not be read ($($_.Exception.Message)), so MFG-Unlocker is not installed. On an RTX 40, run install.bat again."
        return $none
    }
    $nvidia = @($adapters | Where-Object { $_.VendorId -eq $script:AcdbNvidiaVendorId })
    $ada = @($nvidia | Where-Object { $_.Arch -eq 'Ada' })
    if ($nvidia.Count -eq 0 -or $ada.Count -ne $nvidia.Count) {
        if ($nvidia.Count -eq 0) {
            $why = 'there is no NVIDIA GPU'
        } elseif ($ada.Count -eq 0 -and @($nvidia | Where-Object { $_.Arch -eq 'Blackwell' }).Count -eq $nvidia.Count) {
            $why = 'RTX 50 runs 3X/4X natively, so it is not needed'
        } elseif ($ada.Count -gt 0) {
            $others = @($nvidia | Where-Object { $_.Arch -ne 'Ada' } | ForEach-Object { "$($_.Id) $(Get-GpuArchText $_)" })
            $why = "it is installed only when every NVIDIA GPU is an RTX 40, and this PC also has $($others -join ', ')"
        } else {
            $why = 'it is for PCs whose NVIDIA GPUs are all RTX 40'
        }
        Say "MFG-Unlocker: not installed ($why).$removed"
        return $ours
    }
    # Never next to dlssg_for_sm86 (a PC whose RTX 30 was swapped for an RTX 40).
    $rootVersion = Join-Path $Game 'version.dll'
    if ((Test-Path -LiteralPath $rootVersion -PathType Leaf) -and -not (Get-SpoofFileProblem $rootVersion (Get-SpoofPins $SpoofPins)['version.dll'])) {
        Say ("WARNING: $rootVersion is dlssg_for_sm86, which is for RTX 30, and only one of the two may run, so MFG-Unlocker " +
            'is not installed. If this PC has no RTX 30 any more, remove version.dll and dlssg_sm86.ini from the game folder ' +
            "(the uninstaller removes them when this installer put them there), then run install.bat again.$removed")
        return $ours
    }
    $user = @(Find-UserMfgCopies $Game)
    if ($user.Count -gt 0) {
        Say ("WARNING: MFG-Unlocker is already in the game folder outside ac-dlssg ($($user -join ', ')). It was left untouched, " +
            "and ac-dlssg does not install its own copy next to it: only one copy may run.$removed")
        return $ours
    }
    # Another mod's version.dll (neither dlssg_for_sm86 nor MFG-Unlocker):
    # Windows 10 loads it with acs.exe, and the bridge then takes it for a
    # spoof and never loads MFG-Unlocker next to it. Windows 11 25H2 binds
    # System32's version.dll instead, so ours is installed anyway.
    if (Test-Path -LiteralPath $rootVersion -PathType Leaf) {
        Say ("WARNING: $rootVersion is another mod's version.dll (SHA-256 $(Get-Sha256OfFile $rootVersion)); it is left " +
            'untouched. When Windows loads it with the game (Windows 10 does), the bridge takes it for a dlssg_for_sm86 ' +
            'build and does not load MFG-Unlocker next to it, so frame generation stays at 2X; bridge.log says so. Remove ' +
            'that version.dll if you do not need it.')
    }
    $what = "$($script:AcdbMfgProject) $($script:AcdbMfgPin.Version)"
    $pins = Get-MfgPins $MfgPins
    $target = Join-Path $Game $script:AcdbMfgRel
    $staged = Join-Path (Join-Path $depsDir (Get-MfgStageDirName)) $script:AcdbMfgZipDll
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $hash = Get-Sha256OfFile $target
        if ($hash -eq $pins.DllSha256) {
            # Installed from there again: dev-install.ps1 sees the same file and rewrites nothing.
            New-Item -ItemType Directory -Path (Split-Path -Parent $staged) -Force | Out-Null
            Copy-Item -LiteralPath $target -Destination $staged -Force
            if ((Get-Sha256OfFile $staged) -eq $pins.DllSha256) {
                Say "$what is already installed and verified ($target); not downloaded again."
                return @{ MfgInstall = $staged }
            }
        } elseif ($hash -ne $recorded) {
            Say ("WARNING: $target (SHA-256 $hash) is neither the pinned $what nor the copy an earlier install put there. It " +
                'was left untouched, and MFG-Unlocker was not installed; the bridge loads only the pinned copy. To get it, ' +
                'delete that file and run install.bat again.')
            return $none
        }
    }
    Write-MfgNotice $Game
    $fetchArgs = @{ Only = 'Mfg'; DepsDir = $depsDir }
    if ($MfgSourceDir) { $fetchArgs.MfgSourceDir = $MfgSourceDir }
    if ($MfgPins) { $fetchArgs.MfgPins = $MfgPins }
    & (Join-Path $scripts 'fetch-deps.ps1') @fetchArgs
    $fetched = $LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $staged -PathType Leaf) -and (Get-Sha256OfFile $staged) -eq $pins.DllSha256
    if (-not $fetched) {
        Say ("WARNING: getting $what failed (see above), so it is not installed, and frame generation stays at 2X on this " +
            'RTX 40. Everything else is installed; run install.bat again later to try once more.')
        if ($recorded -and (Test-Path -LiteralPath $target -PathType Leaf) -and (Get-Sha256OfFile $target) -eq $recorded) {
            Say 'The copy an earlier install put into ac-dlssg\mfg is not the pinned one any more and is removed.'
            return $remove
        }
        return $none
    }
    return @{ MfgInstall = $staged }
}

$code = 1
$reusedSl = $null
try {
    Say "ac-dlssg from $packageRoot"
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
        Say "WARNING: Windows Smart App Control is in evaluation mode. If Windows switches it on later, it blocks the bridge and the game no longer starts; then run $(Get-InstalledTool $game 'uninstall.bat')."
    }

    $licenses = @()
    if (-not $StreamlineDir) {
        $deps = $depsDir
        $v = $script:AcdbSlVersion
        $reuse = Copy-InstalledStreamline $game (Join-Path $deps "streamline-$v-installed")
        if ($reuse.Dir) {
            $reusedSl = $reuse.Dir
            $StreamlineDir = $reuse.Dir
            Say "Streamline $v is already installed and verified; not downloaded again ($($reuse.From))."
            $licenses = @($script:AcdbSlRuntimeSha256.Keys | Where-Object { $_ -like '*license*' } | Sort-Object | ForEach-Object { Join-Path $reuse.From $_ })
        } else {
            if (Test-Path -LiteralPath $reuse.From -PathType Container) {
                Say "the Streamline $v files in $($reuse.From) cannot be used again ($($reuse.Problem)); Streamline is downloaded again."
            }
            Say "NVIDIA Streamline $v goes to $deps (a download of about 276 MB from github.com/NVIDIA-RTX/Streamline the first time; this can take several minutes without progress output)"
            & (Join-Path $scripts 'fetch-deps.ps1') -Only Streamline -DepsDir $deps
            if ($LASTEXITCODE -ne 0) { Stop-Refused 'getting Streamline failed (see above). Nothing was installed.' }
            $slRoot = Join-Path $deps "streamline-$v"
            $StreamlineDir = Join-Path $slRoot 'bin\x64'
            $licenses = @(foreach ($license in @('license.txt', 'bin\x64\nvngx_dlss.license.txt', 'bin\x64\reflex.license.txt')) { Join-Path $slRoot $license })
        }
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
        Tools         = (Join-Path $filesDir $script:AcdbDataDirName)
        Mode          = $Mode
        AutoUpgrade   = $true
    }
    $spoofArgs = Get-SpoofInstallArgs $game
    foreach ($k in @($spoofArgs.Keys)) { $installArgs[$k] = $spoofArgs[$k] }
    $mfgArgs = Get-MfgInstallArgs $game
    foreach ($k in @($mfgArgs.Keys)) { $installArgs[$k] = $mfgArgs[$k] }
    & (Join-Path $scripts 'dev-install.ps1') @installArgs
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        Say 'installed. Nothing in the game folder needs this unpacked folder: it and the zip can be deleted now.'
        Say "Start the game as usual, drive a few minutes, close it, then run $(Get-InstalledTool $game 'collect-logs.bat') and send the zip it writes next to it."
        Say "To undo, run $(Get-InstalledTool $game 'uninstall.bat')."
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
} finally {
    # The copy of the installed Streamline was only the source of this run.
    if ($reusedSl -and (Test-Path -LiteralPath $reusedSl)) {
        Remove-Item -LiteralPath $reusedSl -Recurse -Force -ErrorAction SilentlyContinue
        if ((Test-Path -LiteralPath $depsDir) -and @(Get-ChildItem -LiteralPath $depsDir -Force).Count -eq 0) {
            Remove-Item -LiteralPath $depsDir -Force -ErrorAction SilentlyContinue
        }
    }
}
Wait-BeforeClose
exit $code
