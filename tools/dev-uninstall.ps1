<#
.SYNOPSIS
  Undoes tools\dev-install.ps1.

.DESCRIPTION
  Uses <game>\ac-dlssg\install\dev-manifest.json. It:
    - refuses to run while acs.exe is running;
    - reverts EnableProxyLibrary and ProxyLibrary in the ReShade.ini that the
      install edited, and only while they still hold the values the install
      wrote: ProxyLibrary when it is still ours, EnableProxyLibrary only when
      ProxyLibrary is also still ours. Keys it added are removed again; keys
      that existed get their original line back byte for byte;
    - re-reads ReShade.ini (and the one ReShade would use now, if that is a
      different file) and stops, keeping the DLL, while ReShade would still
      load ac-dlssg.dll;
    - then deletes <game>\ac-dlssg.dll (only if it is the build the
      install copied) and the manifest;
    - keeps the logs, ac-dlssg.ini and install\backup (DLLs that
      dev-install.ps1 -Force replaced) unless -RemoveData is given, which
      deletes the whole <game>\ac-dlssg folder.

.PARAMETER GameDir
  The Assetto Corsa folder (the one with acs.exe). Default: found through
  Steam's libraryfolders.vdf.

.PARAMETER RemoveData
  Also delete <game>\ac-dlssg, including logs and the config.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev-uninstall.ps1
#>
param(
    [string]$GameDir,
    [switch]$RemoveData
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'dev-common.ps1')

function Say([string]$Message) { Write-Host "dev-uninstall: $Message" }
function Step([string]$Message) { Write-Host "  $Message" }

# Reverts the [PROXY] keys recorded in $Reshade (the manifest's "reshade"
# object). Returns the lines to report.
function Restore-ProxyKeys($Doc, $Reshade) {
    $report = @()
    $blocks = @(Get-IniSections $Doc 'PROXY')
    if ($blocks.Count -ne 1) {
        return @("ReShade.ini has $($blocks.Count) [PROXY] sections now; left unchanged")
    }
    $block = $blocks[0]
    $proxyLines = @(Find-IniKeyLines $Doc $block 'ProxyLibrary')
    $enableLines = @(Find-IniKeyLines $Doc $block 'EnableProxyLibrary')
    $proxyWritten = [string]$Reshade.keys.ProxyLibrary.written
    $enableWritten = [string]$Reshade.keys.EnableProxyLibrary.written

    $proxyOurs = $proxyLines.Count -eq 1 -and
        [string]::Equals((Get-IniValueAt $Doc $proxyLines[0]).Trim(), $proxyWritten, [System.StringComparison]::OrdinalIgnoreCase)
    $enableOurs = $enableLines.Count -eq 1 -and (Get-IniValueAt $Doc $enableLines[0]).Trim() -eq $enableWritten

    $restore = @()
    if ($proxyOurs) {
        $restore += [pscustomobject]@{ Key = 'ProxyLibrary'; Index = $proxyLines[0] }
        if ($enableOurs) {
            $restore += [pscustomobject]@{ Key = 'EnableProxyLibrary'; Index = $enableLines[0] }
        } else {
            $report += "EnableProxyLibrary is now $(Format-KeyLines $Doc $enableLines), not '$enableWritten'; left unchanged"
        }
    } else {
        $report += "ProxyLibrary is now $(Format-KeyLines $Doc $proxyLines), not '$proxyWritten'; both keys left unchanged"
    }

    # Bottom-up, so that removing a line does not move the ones still to do.
    foreach ($item in @($restore | Sort-Object Index -Descending)) {
        $rec = $Reshade.keys.($item.Key)
        if ($rec.present) {
            $Doc.Lines[$item.Index].Text = $script:Latin1.GetString([Convert]::FromBase64String($rec.lineBase64))
            $report += "line $($item.Index + 1): restored '$($rec.line)'"
        } else {
            Remove-IniLine $Doc $item.Index
            $report += "line $($item.Index + 1): removed '$($item.Key)=$([string]$rec.written)' (it was not there before)"
        }
    }

    if ($Reshade.sectionAdded -and $restore.Count -gt 0) {
        $blocks = @(Get-IniSections $Doc 'PROXY')
        if ($blocks.Count -eq 1 -and $blocks[0].End -eq $blocks[0].Header + 1) {
            $header = $blocks[0].Header
            Remove-IniLine $Doc $header
            $report += "removed the empty [PROXY] section the install added"
            if ($Reshade.blankLineAdded -and $header -gt 0 -and $header -le $Doc.Lines.Count -and
                (Get-IniLineInfo $Doc.Lines[$header - 1].Text).Kind -eq 'blank') {
                Remove-IniLine $Doc ($header - 1)
            }
        }
    }
    return $report
}

function Format-KeyLines($Doc, [int[]]$Indexes) {
    if ($Indexes.Count -eq 0) { return '(missing)' }
    return (($Indexes | ForEach-Object { "'$(Get-IniValueAt $Doc $_)'" }) -join ', ')
}

$exitCode = 0
try {
    Say 'ac-dlssg developer uninstall'

    $running = @(Get-RunningGame)
    if ($running.Count -gt 0) {
        Stop-Refused "$($script:AcdbGameExe) is running (pid $(($running | ForEach-Object { $_.Id }) -join ', ')). Close Assetto Corsa first."
    }

    $game = Resolve-GameDir $GameDir
    Step "game folder: $game"

    $target = Join-Path $game $script:AcdbDllName
    $dataDir = Join-Path $game $script:AcdbDataDirName
    $installDir = Join-Path $dataDir 'install'
    $manifestPath = Join-Path $installDir 'dev-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        $hint = ''
        if (Test-Path -LiteralPath $target) { $hint = " $target exists but was not installed by dev-install.ps1; remove it by hand after setting EnableProxyLibrary=0 in ReShade.ini." }
        Stop-Refused "no $manifestPath, so there is no developer install to undo.$hint"
    }
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    $ini = [string]$manifest.reshade.ini
    Step "manifest: $manifestPath (installed $($manifest.installedUtc), state $($manifest.state))"

    # 1. Revert the two keys.
    if (Test-Path -LiteralPath $ini -PathType Leaf) {
        $bytes = [System.IO.File]::ReadAllBytes($ini)
        $doc = ConvertFrom-IniBytes $bytes
        $report = @(Restore-ProxyKeys $doc $manifest.reshade)
        foreach ($line in $report) { Step "ReShade.ini $line" }
        $newBytes = ConvertTo-IniBytes $doc
        if ((Get-Sha256OfBytes $newBytes) -ne (Get-Sha256OfBytes $bytes)) {
            try {
                [void](Write-BytesViaTemp $ini $newBytes)
            } finally {
                if (Test-Path -LiteralPath "$ini.new") { Remove-Item -LiteralPath "$ini.new" -Force -ErrorAction SilentlyContinue }
            }
            Step "wrote $ini (hash verified)"
            if ((Get-Sha256OfBytes $newBytes) -eq [string]$manifest.reshade.iniSha256Before) {
                Step 'ReShade.ini is byte-identical to the state before the first install'
            }
        } else {
            Step "ReShade.ini unchanged"
        }
    } else {
        Step "$ini no longer exists; nothing to revert there"
    }

    # 2. Re-read and make sure ReShade no longer loads our DLL, both from the
    #    file the install edited and from the one ReShade would read now.
    $check = @($ini)
    try {
        $current = (Resolve-ReShadeBase $game).Ini
        if (-not (Test-SamePath $current $ini)) { $check += $current }
    } catch {
        Stop-Refused "cannot tell which ReShade.ini ReShade uses now: $($_.Exception.Message) The DLL is kept."
    }
    foreach ($path in $check) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $doc = Read-IniDoc $path
        if (Test-DocLoadsOurDll $doc) {
            Stop-Refused ("ReShade would still load $($script:AcdbDllName) through $path ($(Format-ProxyKeys $doc)). " +
                'Set EnableProxyLibrary=0 there (or clear ProxyLibrary) and run this again. The DLL and the manifest are kept.')
        }
        Step "verified: $path does not load $($script:AcdbDllName) ($(Format-ProxyKeys $doc))"
    }

    # 3. The DLL.
    if (Test-Path -LiteralPath "$target.new") {
        Remove-Item -LiteralPath "$target.new" -Force
        Step "deleted a leftover $target.new"
    }
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $hash = Get-Sha256OfFile $target
        if ($hash -eq [string]$manifest.dll.sha256) {
            Remove-Item -LiteralPath $target -Force
            Step "deleted $target"
        } else {
            Step "WARNING: $target is not the build dev-install.ps1 copied (SHA-256 $hash); left in place. Delete it by hand if it is yours."
        }
    } else {
        Step "$target is already gone"
    }

    # 4. Data.
    if ($RemoveData) {
        if (Test-Path -LiteralPath $dataDir) {
            Remove-Item -LiteralPath $dataDir -Recurse -Force
            Step "deleted $dataDir (-RemoveData)"
        }
    } else {
        Remove-Item -LiteralPath $manifestPath -Force
        Step "deleted $manifestPath"
        if (@(Get-ChildItem -LiteralPath $installDir -Force).Count -eq 0) { Remove-Item -LiteralPath $installDir -Force }
        $kept = @(Get-ChildItem -LiteralPath $dataDir -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
        if ($kept.Count -gt 0) { Step "kept $dataDir ($($kept -join ', ')); pass -RemoveData to delete it" }
    }
    Say 'done.'
} catch {
    $exitCode = 1
    if (Test-IsRefusal $_) {
        Say "REFUSED: $($_.Exception.Message)"
    } else {
        Say "FAILED: $($_.Exception.Message)"
        Write-Host "  at $($_.InvocationInfo.ScriptName):$($_.InvocationInfo.ScriptLineNumber)"
    }
}
exit $exitCode
