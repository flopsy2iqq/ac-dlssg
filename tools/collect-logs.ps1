<#
.SYNOPSIS
  Collects the ac-dlssg logs and a system report into one zip to send back.

.DESCRIPTION
  Read-only: nothing in the game folder or on the system is changed. It only
  writes ac-dlssg-logs-<yyyyMMdd-HHmmss>.zip next to this script (and
  collect-sysinfo.ps1 writes its report folder next to itself). The zip holds:
    ac-dlssg\logs\...        the bridge's logs (bridge.log, bridge.prev.log) and
                             the Streamline logs it writes into the same folder
    ac-dlssg\ac-dlssg.ini    the bridge's settings
    ac-dlssg\install\dev-manifest.json
                             what install.ps1 installed (mode, file hashes)
    game-files.txt           the game folder's files with versions, and SHA-256
                             of its DLLs and of ac-dlssg\sl
    documents\...            Assetto Corsa's log.txt and CSP's
                             custom_shaders_patch.log of the last run
    crash-events.txt         Windows Application log entries that name acs.exe,
                             from the last 7 days
    sysinfo\...              collect-sysinfo.ps1's report and dxdiag.txt (not
                             with -SkipSysinfo; dxdiag takes up to a minute)
    collect-logs.txt         what was found and what was missing
  The game is found through Steam's libraryfolders.vdf unless -GameDir is
  given. Works from the repository's tools folder and from the test package
  (which keeps the helper scripts in scripts\).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\collect-logs.ps1
#>
param(
    [string]$GameDir,
    [switch]$SkipSysinfo,
    [switch]$NoPause
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
    $staging = Join-Path $PSScriptRoot "ac-dlssg-logs-$stamp"
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
        foreach ($sub in @('ac-dlssg\sl', 'dlssg_sm86')) {
            $dir = Join-Path $game $sub
            if (-not (Test-Path -LiteralPath $dir -PathType Container)) { $listing.Add("($sub not present)"); continue }
            $listing.Add('')
            $listing.Add("$sub\")
            foreach ($f in @(Get-ChildItem -LiteralPath $dir -File -Force | Sort-Object Name)) { $listing.Add((Get-FileLine $f $true)) }
        }
        [System.IO.File]::WriteAllLines((Join-Path $staging 'game-files.txt'), [string[]]$listing.ToArray())
        Note 'wrote game-files.txt'
    }

    $docs = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Assetto Corsa\logs'
    foreach ($f in @('log.txt', 'custom_shaders_patch.log')) {
        Add-File (Join-Path $docs $f) "documents\$f" $staging
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
if (-not $NoPause) {
    try { [void](Read-Host 'Press Enter to close this window') } catch { }
}
exit $code
