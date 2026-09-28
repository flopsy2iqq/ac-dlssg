<#
.SYNOPSIS
  Developer install of ac-dlssg.dll for the in-game milestone tests.

.DESCRIPTION
  A stand-in for the real installer (spec section 12) until it exists. It:
    - refuses to run while acs.exe is running;
    - requires ReShade as <game>\dxgi.dll (version resource ProductName
      "ReShade") and ReShade's ReShade.ini;
    - resolves ReShade's base path like ReShade 6.8.0 ([INSTALL] BasePath in
      <game>\ReShade.ini, else RESHADE_BASE_PATH_OVERRIDE, else the game
      folder) and edits <base>\ReShade.ini;
    - refuses when [PROXY] ProxyLibrary names another DLL;
    - records the old EnableProxyLibrary/ProxyLibrary lines and the file
      hashes in <game>\ac-dlssg\install\dev-manifest.json before
      changing anything;
    - copies the DLL to <game>\ac-dlssg.dll through a .new file, a hash
      check and a rename;
    - sets EnableProxyLibrary=1 and ProxyLibrary in place (adding them under
      [PROXY] only when missing), keeping every other byte of ReShade.ini;
    - writes <game>\ac-dlssg\ac-dlssg.ini with the defaults and
      log_level=debug when it does not exist.

  Running it again is an upgrade: the DLL is replaced, and the values recorded
  by the first run stay the ones dev-uninstall.ps1 restores. The DLL being
  replaced is copied to <game>\ac-dlssg\install\backup first, so that a
  failed upgrade puts it back; after a successful upgrade the copy is deleted
  when it was the build recorded in the manifest. A DLL whose hash matches
  neither that build nor the new one was changed outside this script: it is
  replaced only with -Force, and its copy is kept.

.PARAMETER GameDir
  The Assetto Corsa folder (the one with acs.exe). Default: found through
  Steam's libraryfolders.vdf.

.PARAMETER Dll
  The bridge DLL to install. Default: build\Release\ac-dlssg.dll in the
  work tree this script belongs to.

.PARAMETER Force
  Replace an installed ac-dlssg.dll that was changed outside this script.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev-install.ps1
#>
param(
    [string]$GameDir,
    [string]$Dll,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'dev-common.ps1')

function Say([string]$Message) { Write-Host "dev-install: $Message" }
function Step([string]$Message) { Write-Host "  $Message" }

function Get-RawLineBase64([string]$RawText) { return [Convert]::ToBase64String($script:Latin1.GetBytes($RawText)) }

# Sets both [PROXY] keys in $Doc. Returns what was there before and what was
# changed, or refuses when the file is ambiguous.
function Set-ProxyKeys($Doc, [System.Collections.Specialized.OrderedDictionary]$Values) {
    $blocks = @(Get-IniSections $Doc 'PROXY')
    if ($blocks.Count -gt 1) {
        Stop-Refused "ReShade.ini has $($blocks.Count) [PROXY] sections. Merge them into one first."
    }
    $record = [ordered]@{ sectionAdded = $false; blankLineAdded = $false; keys = [ordered]@{} }
    $actions = @()
    $missing = @()
    foreach ($key in $Values.Keys) {
        $lines = @()
        if ($blocks.Count -eq 1) { $lines = @(Find-IniKeyLines $Doc $blocks[0] $key) }
        if ($lines.Count -gt 1) {
            Stop-Refused "ReShade.ini has $($lines.Count) $key lines under [PROXY]. Keep one first."
        }
        if ($lines.Count -eq 1) {
            $index = $lines[0]
            $oldRaw = $Doc.Lines[$index].Text
            $record.keys[$key] = [ordered]@{
                present    = $true
                value      = Get-IniValueAt $Doc $index
                line       = ConvertFrom-RawText $oldRaw
                lineBase64 = Get-RawLineBase64 $oldRaw
                written    = $Values[$key]
            }
            $newRaw = ConvertTo-RawText "$key=$($Values[$key])"
            if ($oldRaw -cne $newRaw) {
                $Doc.Lines[$index].Text = $newRaw
                $actions += "line $($index + 1): '$(ConvertFrom-RawText $oldRaw)' -> '$key=$($Values[$key])'"
            } else {
                $actions += "line $($index + 1): '$key=$($Values[$key])' already set"
            }
        } else {
            $record.keys[$key] = [ordered]@{ present = $false; value = $null; line = $null; lineBase64 = $null; written = $Values[$key] }
            $missing += $key
        }
    }
    if ($missing.Count -gt 0) {
        $texts = @($missing | ForEach-Object { ConvertTo-RawText "$_=$($Values[$_])" })
        if ($blocks.Count -eq 1) {
            $at = $blocks[0].Header
            for ($i = $blocks[0].Header + 1; $i -lt $blocks[0].End; $i++) {
                if ((Get-IniLineInfo $Doc.Lines[$i].Text).Kind -ne 'blank') { $at = $i }
            }
            Add-IniLines $Doc ($at + 1) $texts
            foreach ($key in $missing) { $actions += "added '$key=$($Values[$key])' under [PROXY]" }
        } else {
            $block = @()
            if ($Doc.Lines.Count -gt 0 -and (Get-IniLineInfo $Doc.Lines[$Doc.Lines.Count - 1].Text).Kind -ne 'blank') {
                $block += ''
                $record.blankLineAdded = $true
            }
            $block += '[PROXY]'
            $block += $texts
            Add-IniLines $Doc $Doc.Lines.Count $block
            $record.sectionAdded = $true
            $actions += "added a [PROXY] section at the end with $(($missing | ForEach-Object { "$_=$($Values[$_])" }) -join ' and ')"
        }
    }
    return [pscustomobject]@{ Record = $record; Actions = $actions }
}

function Get-DefaultConfigText {
    $lines = @(
        '; ac-dlssg settings. Written by tools\dev-install.ps1; edit freely.',
        '[bridge]',
        'enabled=1',
        'start_with_fg=1',
        'hotkey=ctrl+f10',
        '; max_frame_latency=1..16 overrides the value CSP sets; unset by default.',
        'log_level=debug'
    )
    return ($lines -join "`r`n") + "`r`n"
}

function Write-Manifest([string]$Path, $Manifest) {
    Write-Utf8NoBom $Path ($Manifest | ConvertTo-Json -Depth 8)
}

$exitCode = 0
$rollback = @()
$tempFiles = @()
$iniCommitted = $false
try {
    Say 'ac-dlssg developer install'

    $running = @(Get-RunningGame)
    if ($running.Count -gt 0) {
        Stop-Refused "$($script:AcdbGameExe) is running (pid $(($running | ForEach-Object { $_.Id }) -join ', ')). Close Assetto Corsa first."
    }

    $game = Resolve-GameDir $GameDir
    Step "game folder: $game"

    if (-not $Dll) { $Dll = Join-Path $PSScriptRoot "..\build\Release\$($script:AcdbDllName)" }
    if (-not (Test-Path -LiteralPath $Dll -PathType Leaf)) {
        Stop-Refused "bridge DLL not found: $Dll. Build it first: cmake --build build --config Release --target ac-dlssg"
    }
    $source = Get-NormalizedPath (Resolve-Path -LiteralPath $Dll).ProviderPath
    $dllProblems = @(Test-BridgeDll $source)
    if ($dllProblems.Count -gt 0) {
        Stop-Refused ("$source cannot be installed: " + ($dllProblems -join '; '))
    }
    $sourceHash = Get-Sha256OfFile $source
    Step "bridge DLL: $source (SHA-256 $sourceHash)"

    $dxgi = Join-Path $game 'dxgi.dll'
    if (-not (Test-Path -LiteralPath $dxgi -PathType Leaf)) {
        Stop-Refused "ReShade is not installed as $dxgi. Install ReShade with add-on support for DirectX 10/11/12 into the game folder first."
    }
    $vi = (Get-Item -LiteralPath $dxgi).VersionInfo
    if (-not $vi.ProductName -or $vi.ProductName.Trim() -ne 'ReShade') {
        Stop-Refused "$dxgi is not ReShade (version resource ProductName '$($vi.ProductName)'). The bridge is loaded by ReShade as its ProxyLibrary."
    }
    $reshadeVersion = $vi.ProductVersion
    Step "ReShade: $dxgi, version $reshadeVersion"

    $base = Resolve-ReShadeBase $game
    $ini = $base.Ini
    Step "ReShade base path: $($base.BasePath) (from $($base.Source))"
    if (-not (Test-Path -LiteralPath $ini -PathType Leaf)) {
        Stop-Refused "ReShade.ini not found at $ini. Start the game once with ReShade so that it writes ReShade.ini, then run this again."
    }
    Step "ReShade.ini: $ini"

    $target = Join-Path $game $script:AcdbDllName
    $dataDir = Join-Path $game $script:AcdbDataDirName
    $installDir = Join-Path $dataDir 'install'
    $manifestPath = Join-Path $installDir 'dev-manifest.json'
    $config = Join-Path $dataDir 'ac-dlssg.ini'

    if (Test-Path -LiteralPath (Join-Path $installDir 'manifest.json')) {
        Stop-Refused "$installDir\manifest.json exists: the release installer manages this game. Use its uninstaller before a developer install."
    }
    $manifest = $null
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
        if (-not (Test-SamePath $manifest.reshade.ini $ini)) {
            Stop-Refused "the first install edited $($manifest.reshade.ini), but ReShade now uses $ini. Run dev-uninstall.ps1 first."
        }
        Step "existing developer install found (since $($manifest.installedUtc)); this run is an upgrade"
    } elseif (Test-Path -LiteralPath $target) {
        Stop-Refused "$target exists but there is no $manifestPath. Remove the DLL, or set EnableProxyLibrary=0 in ReShade.ini and delete the DLL, then run this again."
    }
    $targetHash = $null
    if (Test-Path -LiteralPath $target -PathType Leaf) { $targetHash = Get-Sha256OfFile $target }
    # Changed outside this script: replaced only with consent (spec 12).
    $foreignDll = [bool]($manifest -and $targetHash -and $targetHash -ne $sourceHash -and
        $targetHash -ne [string]$manifest.dll.sha256)
    if ($foreignDll -and -not $Force) {
        Stop-Refused ("$target was changed outside this script: its SHA-256 is $targetHash, the recorded build is " +
            "$($manifest.dll.sha256). Run again with -Force to replace it; a copy is kept in $installDir\backup.")
    }

    $iniBytes = [System.IO.File]::ReadAllBytes($ini)
    $iniHashBefore = Get-Sha256OfBytes $iniBytes
    $doc = ConvertFrom-IniBytes $iniBytes
    foreach ($value in @((Get-ProxyKeyValues $doc).Proxy)) {
        if ($value.Trim() -ne '' -and -not (Test-NamesOurDll $value)) {
            Stop-Refused "[PROXY] ProxyLibrary=$value in $($ini): another DLL is already chained behind ReShade. The bridge cannot be installed next to it; remove that DLL from ReShade.ini first if you no longer use it."
        }
    }

    $proxyValue = $script:AcdbDllName
    if (-not (Test-SamePath $base.BasePath $game)) { $proxyValue = $target }
    if ($proxyValue.Contains(',')) {
        Stop-Refused "the DLL path '$proxyValue' contains a comma, which ReShade's ini parser splits on."
    }
    $values = [ordered]@{ EnableProxyLibrary = '1'; ProxyLibrary = $proxyValue }
    $edit = Set-ProxyKeys $doc $values
    $newIniBytes = ConvertTo-IniBytes $doc
    $iniHashAfter = Get-Sha256OfBytes $newIniBytes

    $now = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    $configExisted = Test-Path -LiteralPath $config -PathType Leaf
    if ($manifest) {
        $installedUtc = $manifest.installedUtc
        $reshadeRecord = $manifest.reshade
        $reshadeRecord.iniSha256After = $iniHashAfter
        $configCreated = [bool]$manifest.config.created
    } else {
        $installedUtc = $now
        $reshadeRecord = [ordered]@{
            productVersion  = $reshadeVersion
            basePath        = $base.BasePath
            basePathSource  = $base.Source
            ini             = $ini
            iniSha256Before = $iniHashBefore
            iniSha256After  = $iniHashAfter
            hadBom          = $doc.Bom
            sectionAdded    = $edit.Record.sectionAdded
            blankLineAdded  = $edit.Record.blankLineAdded
            keys            = $edit.Record.keys
        }
        $configCreated = -not $configExisted
    }
    $newManifest = [ordered]@{
        schema       = 1
        tool         = 'tools\dev-install.ps1'
        state        = 'installing'
        installedUtc = $installedUtc
        updatedUtc   = $now
        gameDir      = $game
        reshade      = $reshadeRecord
        dll          = [ordered]@{ path = $script:AcdbDllName; source = $source; sha256 = $sourceHash }
        config       = [ordered]@{ path = "$($script:AcdbDataDirName)\ac-dlssg.ini"; created = $configCreated }
    }

    # Changes start here. The manifest goes first so that an interrupted
    # install can still be undone by dev-uninstall.ps1.
    if (-not (Test-Path -LiteralPath $installDir)) {
        if (-not (Test-Path -LiteralPath $dataDir)) { $rollback += @{ Kind = 'dir'; Path = $dataDir } }
        New-Item -ItemType Directory -Path $installDir -Force | Out-Null
        $rollback += @{ Kind = 'dir'; Path = $installDir }
    }
    if ($manifest) {
        $rollback += @{ Kind = 'restore'; Path = $manifestPath; Text = [System.IO.File]::ReadAllText($manifestPath) }
    } else {
        $rollback += @{ Kind = 'file'; Path = $manifestPath }
    }
    Write-Manifest $manifestPath $newManifest
    Step "wrote $manifestPath"

    $backupToDrop = $null
    if ($targetHash -eq $sourceHash) {
        Step "$target is already this build"
    } else {
        if ($targetHash) {
            # A copy for the rollback below; kept afterwards only for a DLL
            # that was changed outside this script.
            $backupDir = Join-Path $installDir 'backup'
            if (-not (Test-Path -LiteralPath $backupDir)) {
                New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
                $rollback += @{ Kind = 'dir'; Path = $backupDir }
            }
            $backup = Join-Path $backupDir ("$($script:AcdbDllName).{0}" -f [DateTime]::UtcNow.ToString('yyyyMMddHHmmss'))
            Copy-Item -LiteralPath $target -Destination $backup -Force
            if ((Get-Sha256OfFile $backup) -ne $targetHash) { throw "hash check of the backup $backup failed" }
            $rollback += @{ Kind = 'restorefile'; Path = $target; Backup = $backup }
            if ($foreignDll) {
                Step "note: $target was changed outside this script (-Force); the replaced DLL is kept as $backup"
            } else {
                $backupToDrop = $backup
            }
        } else {
            $rollback += @{ Kind = 'file'; Path = $target }
        }
        $tempFiles += "$target.new"
        [void](Copy-FileViaTemp $source $target)
        Step "copied the DLL to $target (hash verified)"
    }

    if ($configExisted) {
        Step "kept the existing $config"
    } else {
        $rollback += @{ Kind = 'file'; Path = $config }
        Write-Utf8NoBom $config (Get-DefaultConfigText)
        Step "wrote $config (defaults, log_level=debug)"
    }

    foreach ($action in $edit.Actions) { Step "ReShade.ini $action" }
    if ($iniHashAfter -eq $iniHashBefore) {
        Step 'ReShade.ini already loads the bridge; not rewritten'
    } else {
        $tempFiles += "$ini.new"
        [void](Write-BytesViaTemp $ini $newIniBytes)
        $iniCommitted = $true
        Step "wrote $ini (hash verified)"
    }

    $newManifest.state = 'installed'
    Write-Manifest $manifestPath $newManifest
    if ($backupToDrop) {
        # The previous build of ours: only the rollback needed it.
        Remove-Item -LiteralPath $backupToDrop -Force
        $backupDir = Split-Path -Parent $backupToDrop
        if (@(Get-ChildItem -LiteralPath $backupDir -Force).Count -eq 0) { Remove-Item -LiteralPath $backupDir -Force }
    }
    Say "done. ReShade now loads $proxyValue. Undo with tools\dev-uninstall.ps1."
} catch {
    $exitCode = 1
    if (Test-IsRefusal $_) {
        Say "REFUSED: $($_.Exception.Message)"
    } else {
        Say "FAILED: $($_.Exception.Message)"
        Write-Host "  at $($_.InvocationInfo.ScriptName):$($_.InvocationInfo.ScriptLineNumber)"
    }
    foreach ($temp in @($tempFiles)) {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue }
    }
    if ($rollback.Count -gt 0 -and -not $iniCommitted) {
        [array]::Reverse($rollback)
        foreach ($item in $rollback) {
            try {
                if ($item.Kind -eq 'restore') {
                    Write-Utf8NoBom $item.Path $item.Text
                    Write-Host "  rolled back: restored $($item.Path)"
                } elseif ($item.Kind -eq 'restorefile') {
                    [void](Copy-FileViaTemp $item.Backup $item.Path)
                    Remove-Item -LiteralPath $item.Backup -Force
                    Write-Host "  rolled back: restored $($item.Path) from its backup"
                } elseif ($item.Kind -eq 'file' -and (Test-Path -LiteralPath $item.Path)) {
                    Remove-Item -LiteralPath $item.Path -Force
                    Write-Host "  rolled back: removed $($item.Path)"
                } elseif ($item.Kind -eq 'dir' -and (Test-Path -LiteralPath $item.Path) -and
                    @(Get-ChildItem -LiteralPath $item.Path -Force).Count -eq 0) {
                    Remove-Item -LiteralPath $item.Path -Force
                    Write-Host "  rolled back: removed $($item.Path)"
                }
            } catch {
                Write-Host "  rollback of $($item.Path) failed: $($_.Exception.Message)"
            }
        }
    } elseif ($iniCommitted) {
        Write-Host '  ReShade.ini was already changed; run tools\dev-uninstall.ps1 to undo.'
    }
}
exit $exitCode
