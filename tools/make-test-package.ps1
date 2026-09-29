<#
.SYNOPSIS
  Builds the test package for trying the bridge on another PC (standalone
  mode, no ReShade needed).

.DESCRIPTION
  Writes <OutDir>\ac-dlssg-<Version>-test\ and a zip of that folder next to it.
  The package root holds only install.bat; everything else is in subfolders:
    install.bat          double-click to install: runs scripts\install.ps1
    files\ac-dlssg.dll   the bridge build (-Dll)
    files\apps\lua\AcDlssg\
                         the CSP Lua app that publishes the camera (from the
                         repository's apps\lua\AcDlssg); dev-install.ps1 puts it
                         into <game>\apps\lua\AcDlssg
    scripts\install.ps1  tools\package\install.ps1: finds the game through
                         Steam, starts itself again with administrator rights
                         when the game folder needs them, gets Streamline with
                         fetch-deps.ps1 into files\deps unless -StreamlineDir is
                         given, on an RTX 30 also dlssg_for_sm86 0.3.5 (unless
                         -NoSpoof), runs dev-install.ps1 -AutoUpgrade (default
                         -Mode Auto); it asks nothing
    scripts\uninstall.ps1
                         tools\package\uninstall.ps1: runs dev-uninstall.ps1
    scripts\collect-logs.ps1
                         read-only: zips the bridge's logs, config, manifest,
                         a game folder listing, CSP's log with the Lua app's
                         lines, the dlssg_for_sm86 logs and a system report
    scripts\            also dev-common.ps1, dev-install.ps1, dev-uninstall.ps1,
                         fetch-deps.ps1, collect-sysinfo.ps1
    tools\uninstall.bat, tools\collect-logs.bat
                         double-click to uninstall, or to collect the logs
                         into a zip next to collect-logs.bat
    tools\README-test.txt
                         steps for the tester, in Russian (UTF-8 with BOM), with
                         the version, the git commit and the DLL's SHA-256
  The package works without the repository or any build tool. The script
  fails when the root would hold anything but install.bat.

  It never contains an NVIDIA DLL or a dlssg_for_sm86 file (spec 13): the
  tester's install.ps1 downloads Streamline from NVIDIA's GitHub release and
  checks its SHA-256 and NVIDIA's signatures, and on an RTX 30 downloads
  dlssg_for_sm86 from its author's repository and checks its pinned hashes.
  The script fails when the package would hold any DLL other than
  files\ac-dlssg.dll.

  An existing package folder and zip of the same version are replaced.

.PARAMETER Dll
  The bridge DLL. Default: build\Release\ac-dlssg.dll.

.PARAMETER OutDir
  Default: build\package.

.PARAMETER Version
  Default: the project VERSION in CMakeLists.txt.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-test-package.ps1
#>
param(
    [string]$Dll,
    [string]$OutDir,
    [string]$Version
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot 'dev-common.ps1')

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Dll) { $Dll = Join-Path $repo 'build\Release\ac-dlssg.dll' }
if (-not $OutDir) { $OutDir = Join-Path $repo 'build\package' }

try {
    if (-not (Test-Path -LiteralPath $Dll -PathType Leaf)) {
        throw "bridge DLL not found: $Dll. Build it first: cmake --build build --config Release"
    }
    $Dll = Get-NormalizedPath (Resolve-Path -LiteralPath $Dll).ProviderPath
    $problems = @(Test-BridgeDll $Dll)
    if ($problems.Count -gt 0) { throw "$Dll cannot be packaged: $($problems -join '; ')" }

    if (-not $Version) {
        $cmake = [System.IO.File]::ReadAllText((Join-Path $repo 'CMakeLists.txt'))
        $m = [regex]::Match($cmake, 'project\s*\(\s*\S+\s+VERSION\s+([0-9][0-9.]*)')
        if (-not $m.Success) { throw 'no project VERSION in CMakeLists.txt; pass -Version' }
        $Version = $m.Groups[1].Value
    }
    if ($Version -notmatch '^[0-9A-Za-z.\-]+$') { throw "unusable -Version '$Version'" }

    $commit = 'unknown'
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) {
        $eap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $head = & git -C $repo rev-parse --short HEAD 2>$null
            if ($LASTEXITCODE -eq 0 -and $head) {
                $commit = "$head".Trim()
                $dirty = @(& git -C $repo status --porcelain --untracked-files=no 2>$null)
                if ($dirty.Count -gt 0) { $commit += ' (with uncommitted changes)' }
            }
        } finally {
            $ErrorActionPreference = $eap
        }
    }

    $OutDir = [System.IO.Path]::GetFullPath($OutDir)
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $name = "ac-dlssg-$Version-test"
    $pkg = Join-Path $OutDir $name
    $zip = "$pkg.zip"
    if (Test-Path -LiteralPath $pkg) { Remove-Item -LiteralPath $pkg -Recurse -Force }
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    $filesDir = Join-Path $pkg 'files'
    $scripts = Join-Path $pkg 'scripts'
    $toolsDir = Join-Path $pkg 'tools'
    foreach ($d in @($filesDir, $scripts, $toolsDir)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }

    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'package\install.bat') -Destination (Join-Path $pkg 'install.bat')
    Copy-Item -LiteralPath $Dll -Destination (Join-Path $filesDir 'ac-dlssg.dll')
    foreach ($f in @('install.ps1', 'uninstall.ps1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot "package\$f") -Destination (Join-Path $scripts $f)
    }
    foreach ($f in @('collect-logs.ps1', 'dev-common.ps1', 'dev-install.ps1', 'dev-uninstall.ps1', 'fetch-deps.ps1', 'collect-sysinfo.ps1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $f) -Destination (Join-Path $scripts $f)
    }
    foreach ($f in @('uninstall.bat', 'collect-logs.bat')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot "package\$f") -Destination (Join-Path $toolsDir $f)
    }
    # The CSP Lua app; scripts\install.ps1 passes files\apps\lua\AcDlssg to dev-install.ps1.
    $luaSource = Join-Path $repo 'apps\lua\AcDlssg'
    if (-not (Test-Path -LiteralPath (Join-Path $luaSource 'manifest.ini') -PathType Leaf)) { throw "the CSP Lua app is missing: $luaSource" }
    $luaTarget = Join-Path $filesDir 'apps\lua\AcDlssg'
    New-Item -ItemType Directory -Force -Path $luaTarget | Out-Null
    Copy-Item -Path (Join-Path $luaSource '*') -Destination $luaTarget -Recurse

    $dllHash = Get-Sha256OfFile $Dll
    $readme = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot 'package\README-test.txt'), $script:Utf8NoBom)
    $readme = $readme.Replace('{VERSION}', $Version).Replace('{COMMIT}', $commit).Replace('{DLL_SHA256}', $dllHash).
        Replace('{DATE}', [DateTime]::UtcNow.ToString('yyyy-MM-dd HH:mm') + ' UTC')
    $readme = ($readme -replace "`r?`n", "`r`n")
    [System.IO.File]::WriteAllText((Join-Path $toolsDir 'README-test.txt'), $readme, (New-Object System.Text.UTF8Encoding($true)))

    # The root holds only install.bat.
    $rootFiles = @(Get-ChildItem -LiteralPath $pkg -File -Force | ForEach-Object { $_.Name })
    if (($rootFiles -join '|') -ne 'install.bat') { throw "the package root would hold more than install.bat: $($rootFiles -join ', ')" }

    # Project rule: no NVIDIA DLL and no dlssg_for_sm86 file is ever re-hosted.
    $files = @(Get-ChildItem -LiteralPath $pkg -Recurse -File)
    $bad = @($files | Where-Object {
            ($_.Extension -ieq '.dll' -and $_.FullName -ne (Join-Path $filesDir 'ac-dlssg.dll')) -or
            $_.Name -match '(?i)^(nvngx|sl\.|dlssg_sm86)'
        })
    if ($bad.Count -gt 0) { throw "the package would hold files it must not: $(($bad | ForEach-Object { $_.FullName }) -join ', ')" }

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($pkg, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)

    Write-Host "make-test-package: $pkg"
    foreach ($f in $files | Sort-Object FullName) {
        Write-Host ('  {0,-40} {1,10:N0} bytes' -f $f.FullName.Substring($pkg.Length + 1), $f.Length)
    }
    Write-Host "  ac-dlssg.dll SHA-256 $dllHash, version $Version, commit $commit"
    Write-Host "make-test-package: $zip ($('{0:N0}' -f (Get-Item -LiteralPath $zip).Length) bytes)"
    exit 0
} catch {
    Write-Host "make-test-package: FAILED: $($_.Exception.Message)"
    exit 1
}
