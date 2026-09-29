<#
.SYNOPSIS
  Undoes tools\dev-install.ps1.

.DESCRIPTION
  Uses <game>\ac-dlssg\install\dev-manifest.json and undoes the mode it
  records. In both modes it refuses to run while acs.exe is running, and when
  Windows denies changing the game folder (a game under Program Files) it
  says to run it again from an elevated PowerShell.

  Standalone mode (the bridge is <game>\dxgi.dll):
    - deletes <game>\dxgi.dll only if its SHA-256 is the one the install
      recorded. Any other dxgi.dll (ReShade installed over it, another build
      copied by hand) makes it refuse before anything changes; with -Force it
      leaves that dxgi.dll in place and removes the rest;
    - then the Streamline files, the Lua app, the manifest and optionally
      the data, as in ReShade mode below.

  ReShade mode (a manifest without a mode is one):
    - reverts EnableProxyLibrary and ProxyLibrary in the ReShade.ini that the
      install edited, and only while they still hold the values the install
      wrote: ProxyLibrary when it is still ours, EnableProxyLibrary only when
      ProxyLibrary is also still ours. Keys it added are removed again; keys
      that existed get their original line back byte for byte;
    - re-reads ReShade.ini (and the one ReShade would use now, if that is a
      different file) and stops, keeping the DLL, while ReShade would still
      load ac-dlssg.dll;
    - then deletes <game>\ac-dlssg.dll, the files in <game>\ac-dlssg\sl and
      the CSP Lua app's files in <game>\apps\lua\AcDlssg (each only if it is
      the version the install copied), the folders the install created for
      the app when they are empty, and the manifest;
    - keeps the logs, ac-dlssg.ini and install\backup (DLLs that
      dev-install.ps1 -Force replaced) unless -RemoveData is given, which
      deletes the whole <game>\ac-dlssg folder.

  In both modes the dlssg_for_sm86 files next to acs.exe that the manifest
  records as installed are deleted, each only if it is still the version
  installed; files it records as found (they were there before) stay. With
  -RemoveData it also deletes dlssg_for_sm86's own data, <game>\dlssg_sm86
  and %LOCALAPPDATA%\DlssgSm86, but only when the install put
  dlssg_for_sm86 there and no version.dll is left in the game folder.

.PARAMETER GameDir
  The Assetto Corsa folder (the one with acs.exe). Default: found through
  Steam's libraryfolders.vdf.

.PARAMETER RemoveData
  Also delete <game>\ac-dlssg, including logs and the config, and the data
  of a dlssg_for_sm86 that the install put there (see above).

.PARAMETER Force
  Standalone mode: uninstall even when <game>\dxgi.dll is not the recorded
  bridge, leaving that dxgi.dll in place.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev-uninstall.ps1
#>
param(
    [string]$GameDir,
    [switch]$RemoveData,
    [switch]$Force
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
$game = $null
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
    $mode = 'reshade'
    if ($manifest.PSObject.Properties['mode'] -and $manifest.mode) { $mode = [string]$manifest.mode }
    Step "manifest: $manifestPath (installed $($manifest.installedUtc), state $($manifest.state), mode $mode)"

    if ($mode -eq 'standalone') {
        # 1-3 (standalone). The bridge is <game>\dxgi.dll: check it before
        # changing anything, then delete it first, so that the game stops
        # loading it even if a later step fails.
        $dxgi = Join-Path $game 'dxgi.dll'
        $keepDxgi = $false
        if (Test-Path -LiteralPath $dxgi -PathType Leaf) {
            $hash = Get-Sha256OfFile $dxgi
            if ($hash -ne [string]$manifest.dll.sha256) {
                $product = Get-DxgiProductName $dxgi
                $what = "$dxgi is not the bridge dev-install.ps1 installed (SHA-256 $hash, version resource ProductName '$product'; the install recorded $([string]$manifest.dll.sha256))"
                if (-not $Force) {
                    Stop-Refused "$what. Nothing was changed. Run again with -Force to uninstall the rest and leave that dxgi.dll in place."
                }
                Step "WARNING: $what; left in place (-Force)"
                $keepDxgi = $true
            }
        }
        if (Test-Path -LiteralPath "$dxgi.new") {
            Remove-Item -LiteralPath "$dxgi.new" -Force
            Step "deleted a leftover $dxgi.new"
        }
        if (-not $keepDxgi) {
            if (Test-Path -LiteralPath $dxgi -PathType Leaf) {
                Remove-Item -LiteralPath $dxgi -Force
                Step "deleted $dxgi"
            } else {
                Step "$dxgi is already gone"
            }
        }
    } else {
        $ini = [string]$manifest.reshade.ini

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
    }

    # 4. The Streamline files, each only while it is the version installed.
    if ($manifest.PSObject.Properties['streamline'] -and $manifest.streamline) {
        $slDir = Join-Path $dataDir $script:AcdbSlDirName
        foreach ($f in @($manifest.streamline.files)) {
            $path = Join-Path $game ([string]$f.path)
            if (-not (Test-SamePath (Split-Path -Parent $path) $slDir)) {
                Step "WARNING: the manifest names $path, which is outside $slDir; left alone"
                continue
            }
            if (Test-Path -LiteralPath "$path.new") { Remove-Item -LiteralPath "$path.new" -Force }
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Step "$path is already gone"; continue }
            $hash = Get-Sha256OfFile $path
            if ($hash -eq [string]$f.sha256) {
                Remove-Item -LiteralPath $path -Force
                Step "deleted $path"
            } else {
                Step "WARNING: $path is not the file dev-install.ps1 copied (SHA-256 $hash); left in place. Delete it by hand if it is yours."
            }
        }
        if ((Test-Path -LiteralPath $slDir) -and @(Get-ChildItem -LiteralPath $slDir -Force).Count -eq 0) {
            Remove-Item -LiteralPath $slDir -Force
        }
    }

    # 4b. The CSP Lua app, each file only while it is the version installed,
    #     then the folders the install created, deepest first, when empty.
    if ($manifest.PSObject.Properties['luaApp'] -and $manifest.luaApp) {
        $luaDir = Join-Path $game 'apps\lua\AcDlssg'
        foreach ($f in @($manifest.luaApp.files)) {
            $path = Join-Path $game ([string]$f.path)
            if (-not (Get-NormalizedPath $path).StartsWith((Get-NormalizedPath $luaDir) + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
                Step "WARNING: the manifest names $path, which is outside $luaDir; left alone"
                continue
            }
            if (Test-Path -LiteralPath "$path.new") { Remove-Item -LiteralPath "$path.new" -Force }
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Step "$path is already gone"; continue }
            $hash = Get-Sha256OfFile $path
            if ($hash -eq [string]$f.sha256) {
                Remove-Item -LiteralPath $path -Force
                Step "deleted $path"
            } else {
                Step "WARNING: $path is not the file dev-install.ps1 copied (SHA-256 $hash); left in place. Delete it by hand if it is yours."
            }
        }
        if (Test-Path -LiteralPath $luaDir -PathType Container) {
            $subDirs = @(Get-ChildItem -LiteralPath $luaDir -Directory -Recurse -Force | Sort-Object { $_.FullName.Length } -Descending)
            foreach ($d in $subDirs) {
                if (@(Get-ChildItem -LiteralPath $d.FullName -Force).Count -eq 0) { Remove-Item -LiteralPath $d.FullName -Force }
            }
        }
        foreach ($rel in @($manifest.luaApp.createdDirs)) {
            $dir = Join-Path $game ([string]$rel)
            if (-not (Test-SamePath $dir $luaDir) -and -not (Test-SamePath $dir (Join-Path $game 'apps\lua')) -and
                -not (Test-SamePath $dir (Join-Path $game 'apps'))) {
                Step "WARNING: the manifest names the folder $dir, which the install cannot have created; left alone"
                continue
            }
            if ((Test-Path -LiteralPath $dir -PathType Container) -and @(Get-ChildItem -LiteralPath $dir -Force).Count -eq 0) {
                Remove-Item -LiteralPath $dir -Force
                Step "deleted the empty folder $dir"
            }
        }
    }

    # 4c. dlssg_for_sm86 next to acs.exe: the files the install put there, each
    #     only while it is the version installed; found files stay.
    $spoofOurs = $false
    if ($manifest.PSObject.Properties['spoof'] -and $manifest.spoof) {
        foreach ($f in @($manifest.spoof.files)) {
            $name = [string]$f.path
            if ($script:AcdbSpoofNames -notcontains $name) {
                Step "WARNING: the manifest names $name as a dlssg_for_sm86 file, which it is not; left alone"
                continue
            }
            $path = Join-Path $game $name
            if ([string]$f.origin -ne 'installed') {
                if (Test-Path -LiteralPath $path) { Step "kept $path (it was there before the install)" }
                continue
            }
            $spoofOurs = $true
            if (Test-Path -LiteralPath "$path.new") { Remove-Item -LiteralPath "$path.new" -Force }
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Step "$path is already gone"; continue }
            $hash = Get-Sha256OfFile $path
            if ($hash -eq [string]$f.sha256) {
                Remove-Item -LiteralPath $path -Force
                Step "deleted $path"
            } else {
                Step "WARNING: $path is not the file dev-install.ps1 copied (SHA-256 $hash); left in place. Delete it by hand if you no longer use it."
            }
        }
    }

    # 5. Data. dlssg_for_sm86 keeps its logs in <game>\dlssg_sm86 and its data
    #    in %LOCALAPPDATA%\DlssgSm86; they go only when the install put the
    #    spoof there and its version.dll is gone.
    if ($RemoveData) {
        $spoofData = @(Join-Path $game 'dlssg_sm86')
        if ($env:LOCALAPPDATA) { $spoofData += Join-Path $env:LOCALAPPDATA 'DlssgSm86' }
        $spoofGone = -not (Test-Path -LiteralPath (Join-Path $game 'version.dll'))
        foreach ($d in @($spoofData | Where-Object { Test-Path -LiteralPath $_ })) {
            if ($spoofOurs -and $spoofGone) {
                Remove-Item -LiteralPath $d -Recurse -Force
                Step "deleted $d (-RemoveData)"
            } elseif ($spoofOurs) {
                Step "kept $d, the data of dlssg_for_sm86: a version.dll is still in the game folder"
            } else {
                Step "kept $d, the data of dlssg_for_sm86, which the install did not put there"
            }
        }
    }
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
        if (Test-AccessDenied $_) { Write-AccessDeniedHint $game }
    }
}
exit $exitCode
