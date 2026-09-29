<#
.SYNOPSIS
  Collects the ac-dlssg logs and a system report into one zip to send back.

.DESCRIPTION
  Read-only: nothing in the game folder or on the system is changed. It only
  writes ac-dlssg-logs-<yyyyMMdd-HHmmss>.zip into -OutDir, by default next to
  this script (the test package's tools\collect-logs.bat passes its own
  folder), and collect-sysinfo.ps1 writes its report folder next to itself.
  The zip holds:
    ac-dlssg\logs\...        the bridge's logs (bridge.log, bridge.prev.log) and
                             the Streamline logs it writes into the same folder
    ac-dlssg\ac-dlssg.ini    the bridge's settings
    ac-dlssg\install\dev-manifest.json
                             what install.ps1 installed (mode, file hashes)
    game-files.txt           the game folder's files with versions, and SHA-256
                             of its DLLs, of ac-dlssg\sl, of the CSP Lua app in
                             apps\lua\AcDlssg and of dlssg_sm86
    dlssg_sm86\logs\...      the dlssg_for_sm86 spoof's own logs (loader_<pid>.jsonl,
                             backend_<pid>.jsonl), the newest 20, when the spoof
                             is installed
    documents\...            Assetto Corsa's log.txt and CSP's
                             custom_shaders_patch.log of the last run, and
                             acdlssg-lua-app.txt: the lines of that CSP log from or
                             about the Lua app (CSP writes a Lua app's ac.log,
                             ac.warn and ac.error lines there, tagged
                             "[Lua: App: AC DLSS-G Camera]", and its loading lines
                             name apps\lua\AcDlssg)
    crash-events.txt         Windows Application log entries that name acs.exe,
                             from the last 7 days
    sysinfo\...              collect-sysinfo.ps1's report and dxdiag.txt (not
                             with -SkipSysinfo; dxdiag takes up to a minute)
    collect-logs.txt         what was found and what was missing
  The game is found through Steam's libraryfolders.vdf unless -GameDir is
  given; the Assetto Corsa documents folder is Documents\Assetto Corsa unless
  -AcDocsDir is given. Works from the repository's tools folder and from the test package
  (which keeps the helper scripts in scripts\). The last line is "Press Enter
  to exit", so that a double-clicked window stays open; not with -NoPause or
  when the input is redirected.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\collect-logs.ps1
#>
param(
    [string]$GameDir,
    [string]$AcDocsDir,
    [switch]$SkipSysinfo,
    [switch]$NoPause,
    # Where the zip goes; default: next to this script.
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Find-Helper([string]$Name) {
    foreach ($dir in @($PSScriptRoot, (Join-Path $PSScriptRoot 'scripts'))) {
        $path = Join-Path $dir $Name
        if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    }
    return $null
}

$notes = New-Object System.Collections.Generic.List[string]
function Note([string]$Line) { $notes.Add($Line); Write-Host "  $Line" }

# Copies a file even while another process has it open for writing.
function Copy-Shared([string]$Source, [string]$Target) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Target) | Out-Null
    $in = [System.IO.File]::Open($Source, 'Open', 'Read', 'ReadWrite, Delete')
    try {
        $out = [System.IO.File]::Create($Target)
        try { $in.CopyTo($out) } finally { $out.Dispose() }
    } finally {
        $in.Dispose()
    }
}

# The Lua app's lines in CSP's log: its own messages ("AcDlssg: ..."), CSP's
# "[Lua: App: AC DLSS-G Camera]" tag (the NAME in its manifest.ini), and the
# loading lines that name its folder.
$LuaAppLinePattern = 'AcDlssg|\[Lua: App: AC DLSS-G Camera\]'
$MaxSpoofLogs = 20

# All lines of a text file (CSP's log is UTF-8), even while the game has it open.
function Read-SharedLines([string]$Path) {
    $stream = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite, Delete')
    try {
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
        try { return @($reader.ReadToEnd() -split "`r?`n" | Where-Object { $_ -ne '' }) } finally { $reader.Dispose() }
    } finally {
        $stream.Dispose()
    }
}

function Add-File([string]$Source, [string]$Rel, [string]$Staging) {
    if (Test-Path -LiteralPath $Source -PathType Leaf) {
        try {
            Copy-Shared $Source (Join-Path $Staging $Rel)
            Note "collected $Rel ($((Get-Item -LiteralPath $Source).Length) bytes, from $Source)"
        } catch {
            Note "could not read $($Source): $($_.Exception.Message)"
        }
    } else {
        Note "missing: $Source"
    }
}

function Get-FileLine([System.IO.FileInfo]$File, [bool]$Hash) {
    $line = '{0,-36} {1,12} {2:yyyy-MM-dd HH:mm}' -f $File.Name, $File.Length, $File.LastWriteTime
    if ($File.Extension -match '^\.(dll|exe|addon64)$') {
        $vi = $File.VersionInfo
        $line += "  [$($vi.ProductName) $($vi.FileVersion)]"
    }
    if ($Hash) {
        try { $line += '  SHA-256 ' + (Get-FileHash -Algorithm SHA256 -LiteralPath $File.FullName).Hash.ToLowerInvariant() } catch { }
    }
    return $line
}

$code = 0
$staging = $null
try {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    if (-not $OutDir) { $OutDir = $PSScriptRoot }
    $OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
    $staging = Join-Path $OutDir "ac-dlssg-logs-$stamp"
    $zip = "$staging.zip"
    New-Item -ItemType Directory -Path $staging | Out-Null
    Write-Host "collect-logs: collecting into $zip"
    Note "collect-logs.ps1 at $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz') from $PSScriptRoot"

    $game = $null
    $common = Find-Helper 'dev-common.ps1'
    if (-not $common) {
        Note 'dev-common.ps1 not found next to this script or in scripts\; the game folder must be given with -GameDir'
        if ($GameDir -and (Test-Path -LiteralPath $GameDir -PathType Container)) { $game = (Resolve-Path -LiteralPath $GameDir).ProviderPath }
    } else {
        . $common
        try {
            $game = Resolve-GameDir $GameDir
        } catch {
            Note "game folder not found: $($_.Exception.Message)"
        }
    }

    if ($game) {
        Note "game folder: $game"
        $data = Join-Path $game 'ac-dlssg'
        $logs = Join-Path $data 'logs'
        if (Test-Path -LiteralPath $logs -PathType Container) {
            foreach ($f in @(Get-ChildItem -LiteralPath $logs -Recurse -File)) {
                Add-File $f.FullName ('ac-dlssg\logs\' + $f.FullName.Substring($logs.Length + 1)) $staging
            }
        } else {
            Note "missing: $logs"
        }
        Add-File (Join-Path $data 'ac-dlssg.ini') 'ac-dlssg\ac-dlssg.ini' $staging
        Add-File (Join-Path $data 'install\dev-manifest.json') 'ac-dlssg\install\dev-manifest.json' $staging

        $listing = New-Object System.Collections.Generic.List[string]
        $listing.Add("game folder $game")
        foreach ($f in @(Get-ChildItem -LiteralPath $game -File -Force | Sort-Object Name)) {
            $listing.Add((Get-FileLine $f ($f.Extension -ieq '.dll')))
        }
        foreach ($sub in @('ac-dlssg\sl', 'apps\lua\AcDlssg', 'dlssg_sm86')) {
            $dir = Join-Path $game $sub
            if (-not (Test-Path -LiteralPath $dir -PathType Container)) { $listing.Add("($sub not present)"); continue }
            $listing.Add('')
            $listing.Add("$sub\")
            foreach ($f in @(Get-ChildItem -LiteralPath $dir -File -Force | Sort-Object Name)) { $listing.Add((Get-FileLine $f $true)) }
        }
        [System.IO.File]::WriteAllLines((Join-Path $staging 'game-files.txt'), [string[]]$listing.ToArray())
        Note 'wrote game-files.txt'

        # The dlssg_for_sm86 spoof writes one loader and one backend log per run.
        $spoofLogs = Join-Path $game 'dlssg_sm86\logs'
        if (Test-Path -LiteralPath $spoofLogs -PathType Container) {
            $jsonl = @(Get-ChildItem -LiteralPath $spoofLogs -File -Filter '*.jsonl' | Sort-Object LastWriteTime -Descending)
            foreach ($f in @($jsonl | Select-Object -First $MaxSpoofLogs)) { Add-File $f.FullName "dlssg_sm86\logs\$($f.Name)" $staging }
            if ($jsonl.Count -gt $MaxSpoofLogs) { Note "dlssg_sm86\logs: $($jsonl.Count - $MaxSpoofLogs) older .jsonl files not collected" }
            if ($jsonl.Count -eq 0) { Note "no .jsonl files in $spoofLogs" }
        } else {
            Note "$spoofLogs not present (no dlssg_for_sm86 spoof)"
        }
    }

    if (-not $AcDocsDir) { $AcDocsDir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Assetto Corsa' }
    $docs = Join-Path $AcDocsDir 'logs'
    foreach ($f in @('log.txt', 'custom_shaders_patch.log')) {
        Add-File (Join-Path $docs $f) "documents\$f" $staging
    }
    # CSP writes the Lua app's lines into its own log; they are also collected on their own.
    $cspLog = Join-Path $docs 'custom_shaders_patch.log'
    if (Test-Path -LiteralPath $cspLog -PathType Leaf) {
        try {
            $appLines = @(Read-SharedLines $cspLog | Where-Object { $_ -match $LuaAppLinePattern })
            New-Item -ItemType Directory -Force -Path (Join-Path $staging 'documents') | Out-Null
            $out = @("# lines of $cspLog matching '$LuaAppLinePattern' (the CSP Lua app AcDlssg)") + $appLines
            [System.IO.File]::WriteAllLines((Join-Path $staging 'documents\acdlssg-lua-app.txt'), [string[]]$out, (New-Object System.Text.UTF8Encoding $false))
            Note "wrote documents\acdlssg-lua-app.txt ($($appLines.Count) lines of the Lua app from $cspLog)"
        } catch {
            Note "could not read $($cspLog): $($_.Exception.Message)"
        }
    }

    try {
        $events = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = @(1000, 1001, 1002); StartTime = (Get-Date).AddDays(-7) } -ErrorAction Stop |
                Where-Object { $_.Message -match '(?i)acs\.exe' })
    } catch {
        $events = @()
    }
    $eventText = @($events | ForEach-Object { "=== $($_.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss')) event $($_.Id) $($_.ProviderName)"; $_.Message; '' })
    if ($eventText.Count -eq 0) { $eventText = @('no Application Error / Windows Error Reporting / hang events naming acs.exe in the last 7 days') }
    [System.IO.File]::WriteAllLines((Join-Path $staging 'crash-events.txt'), [string[]]$eventText)
    Note "wrote crash-events.txt ($($events.Count) events)"

    if ($SkipSysinfo) {
        Note 'system report skipped (-SkipSysinfo)'
    } else {
        $sysinfo = Find-Helper 'collect-sysinfo.ps1'
        if (-not $sysinfo) {
            Note 'collect-sysinfo.ps1 not found; no system report'
        } else {
            Write-Host 'collect-logs: running collect-sysinfo.ps1 (dxdiag takes up to a minute)'
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $sysinfo | Out-Host
            $report = Join-Path (Split-Path -Parent $sysinfo) 'ac-dlssg-sysinfo'
            foreach ($f in @('ac-dlssg-sysinfo.txt', 'dxdiag.txt')) {
                Add-File (Join-Path $report $f) "sysinfo\$f" $staging
            }
        }
    }

    [System.IO.File]::WriteAllLines((Join-Path $staging 'collect-logs.txt'), [string[]]$notes.ToArray())
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($staging, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
    Write-Host ''
    Write-Host "collect-logs: done. Send this file: $zip"
} catch {
    $code = 1
    Write-Host "collect-logs: FAILED: $($_.Exception.Message)"
} finally {
    if ($staging -and (Test-Path -LiteralPath $staging)) { Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue }
}
# The last line: a double-clicked window stays open until Enter.
if (-not $NoPause) {
    $redirected = $true
    try { $redirected = [Console]::IsInputRedirected } catch { }
    if (-not $redirected) {
        try { [void](Read-Host 'Press Enter to exit') } catch { }
    }
}
exit $code
