<#
.SYNOPSIS
  Removes the ac-dlssg test build from Assetto Corsa.

.DESCRIPTION
  Part of the test package built by tools\make-test-package.ps1, where it is
  scripts\uninstall.ps1 and tools\uninstall.bat starts it. install.ps1 also
  puts a copy into <game>\ac-dlssg\scripts, which <game>\ac-dlssg\uninstall.bat
  starts with -InstalledCopy, so that the unpacked package is not needed any
  more. It asks nothing. When this account may not write into the game
  folder (a game under C:\Program Files (x86)), it starts itself again with
  administrator rights, once, and that window goes on; the Windows UAC
  prompt is the only question.

  It runs dev-uninstall.ps1, which undoes what install.ps1 did, using
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
  version.dll is left. The uninstaller and the log collector in
  <game>\ac-dlssg go too (see below). It refuses while acs.exe is running.

  The copy in the game folder (-InstalledCopy) works on the game folder it
  is in, without a Steam lookup, and removes itself last: after a successful
  uninstall and the Enter, it deletes uninstall.bat, collect-logs.bat and
  scripts\ (each file only while it is the version installed; with
  -RemoveData all of <game>\ac-dlssg), then <game>\ac-dlssg when that is
  empty. uninstall.bat ends itself without reading its file again (see
  there). When the run starts again with administrator rights, the elevated
  run does all of that in its window while the first one waits for it; the
  first one then only reports the exit code and touches no file.

  The last line is "Press Enter to exit", so that a double-clicked window
  stays open; not with -NoPause or when the input is redirected.

.PARAMETER NoElevate
  For tests: when the game folder is not writable, refuse instead of
  starting again with administrator rights.

.PARAMETER Elevated
  Set by the script itself on the elevated run, so that it restarts only once.

.PARAMETER InstalledCopy
  Set by <game>\ac-dlssg\uninstall.bat: this script is the copy in
  <game>\ac-dlssg\scripts (see above). A -GameDir that is not that game
  folder is refused.

.PARAMETER FakeElevation
  For tests: act as if the game folder needed administrator rights, and
  start the second run without asking Windows for them (no UAC prompt), as
  another Windows PowerShell in this console.

.EXAMPLE
  tools\uninstall.bat

.EXAMPLE
  tools\uninstall.bat -RemoveData

.EXAMPLE
  <game>\ac-dlssg\uninstall.bat -RemoveData
#>
param(
    [string]$GameDir,
    [switch]$RemoveData,
    [switch]$NoPause,
    [switch]$NoElevate,
    [switch]$Elevated,
    [switch]$InstalledCopy,
    [switch]$FakeElevation
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

# The copy in <game>\ac-dlssg\scripts: the game folder is the one it is in.
function Get-OwnGameDir {
    $dataDir = Split-Path -Parent $PSScriptRoot
    if ((Split-Path -Leaf $PSScriptRoot) -ne $script:AcdbToolsScriptsDirName -or (Split-Path -Leaf $dataDir) -ne $script:AcdbDataDirName) {
        Stop-Refused "-InstalledCopy is for the copy in <game>\$($script:AcdbDataDirName)\$($script:AcdbToolsScriptsDirName); this script is in $PSScriptRoot."
    }
    return Get-NormalizedPath (Split-Path -Parent $dataDir)
}

# The tools the manifest records, read before dev-uninstall.ps1 deletes it.
function Get-ToolRecords([string]$Game) {
    $path = Join-Path $Game "$($script:AcdbDataDirName)\install\dev-manifest.json"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return @() }
    $manifest = Read-Manifest $path
    if (-not $manifest.PSObject.Properties['tools'] -or -not $manifest.tools) { return @() }
    return @($manifest.tools.files | Where-Object { Test-ToolRelPath ([string]$_.path) })
}

# After the Enter: the uninstaller and the log collector, each only while it
# is the version installed (with -RemoveData all of <game>\ac-dlssg), then
# scripts\ and <game>\ac-dlssg when they are empty. The uninstall itself
# succeeded, so a problem here is a warning.
function Remove-InstalledTools([string]$Game, $Records) {
    $dataDir = Join-Path $Game $script:AcdbDataDirName
    try {
        if ($RemoveData) {
            if (Test-Path -LiteralPath $dataDir) { Remove-Item -LiteralPath $dataDir -Recurse -Force }
            Say "deleted $dataDir with the uninstaller in it (-RemoveData)"
            return
        }
        foreach ($rec in @($Records)) {
            $path = Join-Path $Game ([string]$rec.path)
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
            if ((Get-Sha256OfFile $path) -eq [string]$rec.sha256) {
                Remove-Item -LiteralPath $path -Force
            } else {
                Say "WARNING: $path was changed since the install; left in place. Delete it by hand if you no longer need it."
            }
        }
        foreach ($dir in @((Join-Path $dataDir $script:AcdbToolsScriptsDirName), $dataDir)) {
            if ((Test-Path -LiteralPath $dir -PathType Container) -and @(Get-ChildItem -LiteralPath $dir -Force).Count -eq 0) {
                Remove-Item -LiteralPath $dir -Force
            }
        }
        Say "removed the uninstaller and the log collector from $dataDir"
    } catch {
        Say "WARNING: the uninstaller could not remove itself completely ($($_.Exception.Message)). Delete uninstall.bat, collect-logs.bat and the scripts folder in $dataDir by hand."
    }
}

$code = 1
$game = $null
$tools = @()
try {
    if ($InstalledCopy) {
        $own = Get-OwnGameDir
        if ($GameDir -and -not (Test-SamePath $GameDir $own)) {
            Stop-Refused ("this uninstaller belongs to $own, not to $GameDir. Run the uninstall.bat in the " +
                "$($script:AcdbDataDirName) folder of that game folder instead. Nothing was changed.")
        }
        $GameDir = $own
    }
    $game = Resolve-GameDir $GameDir
    $fake = $FakeElevation -and -not $Elevated
    if ($fake -or -not (Test-DirWritable $game)) {
        if (-not $fake -and ($Elevated -or (Test-IsAdministrator))) {
            Stop-Refused "Windows does not let even an administrator change $game. Check that folder's permissions. Nothing was changed."
        }
        if ($NoElevate) {
            Stop-Refused "this account may not change $game, and -NoElevate keeps the uninstaller from starting again with administrator rights. Nothing was changed."
        }
        Say "this account may not change $game. The uninstaller starts again with administrator rights: confirm the Windows prompt, and follow the new window."
        try {
            $forward = Get-ForwardArguments $PSBoundParameters @{ GameDir = $game; Elevated = $true; FakeElevation = $false }
            $code = Invoke-ElevatedScript $PSCommandPath $forward -WithoutUac:$fake
        } catch {
            Stop-Refused "the uninstaller did not get administrator rights ($($_.Exception.Message)). Nothing was changed."
        }
        Say "the uninstaller with administrator rights ended with exit code $code."
        exit $code
    }
    if ($InstalledCopy) { $tools = @(Get-ToolRecords $game) }
    # -Force: a dxgi.dll that is not the recorded bridge stays, the rest goes.
    # -KeepTools: this copy is one of the tools; it removes them at its end.
    $uninstallArgs = @{ GameDir = $game; Force = $true }
    if ($RemoveData) { $uninstallArgs.RemoveData = $true }
    if ($InstalledCopy) { $uninstallArgs.KeepTools = $true }
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
# The copy in the game folder removes itself only now, after a successful
# uninstall and the Enter.
if ($InstalledCopy -and $code -eq 0) {
    Remove-InstalledTools $game $tools
}
exit $code
