<#
.SYNOPSIS
  Writes the GitHub release notes of a version: its CHANGELOG.md section,
  the SHA-256 of the release zip and of ac-dlssg.dll, and how to verify
  their attestations.

.DESCRIPTION
  The notes are the body of the "## [<Version>] - <date>" section of
  CHANGELOG.md, without its heading, followed by a checksum table and the
  line "Verify with: gh attestation verify <file> --repo <Repo>".

  The zip must be named ac-dlssg-<Version>.zip. The DLL's SHA-256 is that of
  <folder>/files/ac-dlssg.dll inside the zip, the file the installer puts
  into the game; with -Dll, that file must have the same SHA-256 (the
  release workflow attests the build's ac-dlssg.dll next to the zip).

  Without -Zip it only checks that the section exists and is not empty, and
  prints it; CI runs it that way on every push. With -Tag the tag must be
  v<Version>.

  Exit code 0 on success, 1 on any failure, and then nothing is written.

.PARAMETER Version
  Default: the project VERSION in -CMakeLists.

.PARAMETER Tag
  The git tag being released (the release workflow passes GITHUB_REF_NAME).

.PARAMETER Zip
  The release zip built by make-test-package.ps1.

.PARAMETER Dll
  The bridge DLL that is attested next to the zip.

.PARAMETER OutFile
  Where the notes go (UTF-8 without BOM). Default:
  build\release-notes-<Version>.md.

.PARAMETER Changelog
  Default: CHANGELOG.md at the repository root.

.PARAMETER CMakeLists
  Default: CMakeLists.txt at the repository root.

.PARAMETER Repo
  The GitHub repository named in the verify line. Default: flopsy2iqq/ac-dlssg.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\release-notes.ps1

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\release-notes.ps1 -Tag v1.0.0 -Zip build\package\ac-dlssg-1.0.0.zip -Dll build\Release\ac-dlssg.dll
#>
param(
    [string]$Version,
    [string]$Tag,
    [string]$Zip,
    [string]$Dll,
    [string]$OutFile,
    [string]$Changelog,
    [string]$CMakeLists,
    [string]$Repo = 'flopsy2iqq/ac-dlssg'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Changelog) { $Changelog = Join-Path $repoRoot 'CHANGELOG.md' }
if (-not $CMakeLists) { $CMakeLists = Join-Path $repoRoot 'CMakeLists.txt' }
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Get-Sha256OfStream([IO.Stream]$Stream) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return (($sha.ComputeHash($Stream) | ForEach-Object { $_.ToString('x2') }) -join '')
    } finally {
        $sha.Dispose()
    }
}

try {
    if (-not $Version) {
        if (-not (Test-Path -LiteralPath $CMakeLists -PathType Leaf)) { throw "$CMakeLists not found; pass -Version" }
        $m = [regex]::Match([IO.File]::ReadAllText($CMakeLists), 'project\s*\(\s*\S+\s+VERSION\s+([0-9][0-9.]*)')
        if (-not $m.Success) { throw "no project VERSION in $CMakeLists; pass -Version" }
        $Version = $m.Groups[1].Value
    }
    if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw "unusable version '$Version' (expected major.minor.patch)" }
    if ($Tag -and $Tag -ne "v$Version") { throw "the tag $Tag does not match the project version $Version (expected v$Version)" }

    # The section: from its "## [<Version>]" heading to the next "## " heading.
    if (-not (Test-Path -LiteralPath $Changelog -PathType Leaf)) { throw "$Changelog not found" }
    $lines = [IO.File]::ReadAllLines($Changelog, $utf8)
    $heading = '^##\s+\[?' + [regex]::Escape($Version) + '\]?(\s|$)'
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match $heading) { $start = $i; break } }
    if ($start -lt 0) { throw "$Changelog has no section for $Version (a heading '## [$Version] - <date>')" }
    $end = $lines.Count
    for ($i = $start + 1; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^##\s') { $end = $i; break } }
    $body = @(if ($end - $start -gt 1) { $lines[($start + 1)..($end - 1)] })
    $first = 0
    while ($first -lt $body.Count -and -not $body[$first].Trim()) { $first++ }
    $last = $body.Count - 1
    while ($last -ge $first -and -not $body[$last].Trim()) { $last-- }
    if ($last -lt $first) { throw "the $Version section of $Changelog is empty" }
    $section = ($body[$first..$last] -join "`n")

    if (-not $Zip) {
        Write-Host "release-notes: $Changelog has the $Version section ($($last - $first + 1) lines):"
        Write-Host $section
        exit 0
    }

    if (-not (Test-Path -LiteralPath $Zip -PathType Leaf)) { throw "zip not found: $Zip" }
    $Zip = (Resolve-Path -LiteralPath $Zip).ProviderPath
    $zipName = Split-Path -Leaf $Zip
    if ($zipName -ne "ac-dlssg-$Version.zip") { throw "the zip is $zipName; the release zip of $Version is ac-dlssg-$Version.zip" }
    $zipSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $Zip).Hash.ToLowerInvariant()

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $entryName = "ac-dlssg-$Version/files/ac-dlssg.dll"
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $entry = @($archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -eq $entryName })
        if ($entry.Count -ne 1) { throw "$zipName holds no $entryName" }
        $stream = $entry[0].Open()
        try { $dllSha = Get-Sha256OfStream $stream } finally { $stream.Dispose() }
    } finally {
        $archive.Dispose()
    }
    if ($Dll) {
        if (-not (Test-Path -LiteralPath $Dll -PathType Leaf)) { throw "DLL not found: $Dll" }
        $given = (Get-FileHash -Algorithm SHA256 -LiteralPath $Dll).Hash.ToLowerInvariant()
        if ($given -ne $dllSha) { throw "$Dll (SHA-256 $given) differs from $entryName in the zip ($dllSha)" }
    }

    $notes = @(
        $section
        ''
        '## Checksums'
        ''
        '| File | SHA-256 |'
        '|---|---|'
        "| ``$zipName`` | ``$zipSha`` |"
        "| ``ac-dlssg.dll`` (``files\ac-dlssg.dll`` in the zip) | ``$dllSha`` |"
        ''
        'Both files have a GitHub build provenance attestation from the release workflow of this repository.'
        'The installed bridge is the same file, so it verifies too: <game>\ac-dlssg.dll in ReShade mode, <game>\dxgi.dll in standalone mode.'
        ''
        "Verify with: ``gh attestation verify <file> --repo $Repo``"
        ''
    ) -join "`n"

    if (-not $OutFile) { $OutFile = Join-Path $repoRoot "build\release-notes-$Version.md" }
    $OutFile = [IO.Path]::GetFullPath($OutFile)
    $outDir = Split-Path -Parent $OutFile
    if (-not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }
    [IO.File]::WriteAllText($OutFile, $notes, $utf8)
    Write-Host "release-notes: $OutFile"
    Write-Host $notes
    exit 0
} catch {
    Write-Host "release-notes: FAILED: $($_.Exception.Message)"
    exit 1
}
