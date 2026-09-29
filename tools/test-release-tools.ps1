<#
.SYNOPSIS
  Tests the release helpers release-notes.ps1 and run-ci-tests.ps1 against
  fixtures, and checks that CHANGELOG.md has a section for the project
  version.

.DESCRIPTION
  release-notes.ps1 runs against fixture changelogs, CMakeLists.txt files and
  zips built here. run-ci-tests.ps1 runs against fixture test sources and a
  fake test runner (a .cmd that prints what acdb_tests.exe prints for a
  filter), so no GPU is needed. The last cases run release-notes.ps1 on the
  repository's own CHANGELOG.md and CMakeLists.txt, and make-test-package.ps1
  without -Version on the Release build (-Dll), whose zip must be named
  after the project VERSION.

  Everything is written under -WorkRoot (default build\release-tools-test),
  which is deleted and re-created on every run, and deleted again after a
  passing run unless -Keep is given. It is only deleted when it holds the
  marker file this script writes. TEMP and TMP point into it.

  Exits 0 when every check passes, 1 otherwise.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\test-release-tools.ps1
#>
param(
    [string]$WorkRoot,
    # The bridge build that the package case packs. Default: build\Release\ac-dlssg.dll.
    [string]$Dll,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$tools = $PSScriptRoot
$repo = Split-Path -Parent $tools
$notesScript = Join-Path $tools 'release-notes.ps1'
$ciTestsScript = Join-Path $tools 'run-ci-tests.ps1'

if (-not $WorkRoot) { $WorkRoot = Join-Path $repo 'build\release-tools-test' }
if (-not $Dll) { $Dll = Join-Path $repo 'build\Release\ac-dlssg.dll' }
$Dll = [IO.Path]::GetFullPath($Dll)
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot).TrimEnd('\')
$markerName = '.acdb-release-tools-test-root'

function Remove-WorkRoot {
    if (-not (Test-Path -LiteralPath $WorkRoot)) { return $true }
    if (-not (Test-Path -LiteralPath (Join-Path $WorkRoot $markerName) -PathType Leaf)) {
        Write-Host "test-release-tools: $WorkRoot exists but has no $markerName marker; refusing to delete it"
        return $false
    }
    Remove-Item -LiteralPath $WorkRoot -Recurse -Force
    return $true
}

if (-not (Remove-WorkRoot)) { exit 1 }
New-Item -ItemType Directory -Path $WorkRoot | Out-Null
[IO.File]::WriteAllText((Join-Path $WorkRoot $markerName), 'created by tools\test-release-tools.ps1')
$tempDir = Join-Path $WorkRoot 'tmp'
New-Item -ItemType Directory -Path $tempDir | Out-Null
$env:TEMP = $tempDir
$env:TMP = $tempDir

$script:passed = 0
$script:failed = 0
$script:failedNames = New-Object System.Collections.Generic.List[string]
$script:currentCase = ''
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Check([bool]$Condition, [string]$Name) {
    if ($Condition) {
        $script:passed++
        Write-Host "    ok    $Name"
    } else {
        $script:failed++
        $script:failedNames.Add("$($script:currentCase): $Name")
        Write-Host "    FAIL  $Name"
    }
}

function Invoke-Case([string]$Name, [scriptblock]$Body) {
    $script:currentCase = $Name
    Write-Host ''
    Write-Host "== $Name"
    try {
        & $Body
    } catch {
        Check $false "no unexpected error ($($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber))"
    }
}

function Write-Text([string]$Path, [string]$Text) {
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    [IO.File]::WriteAllText($Path, $Text, $utf8)
}

function Get-Sha([string]$Path) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() }

# Runs a script in a child Windows PowerShell; output goes to a file, never
# through a pipe.
function Invoke-Script([string]$Script, [string[]]$Arguments) {
    $id = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $stdout = Join-Path $tempDir "run-$id-out.txt"
    $stderr = Join-Path $tempDir "run-$id-err.txt"
    $argText = (@('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $Script) + @($Arguments) |
            ForEach-Object { if ($_ -match '[\s"]' -or $_ -eq '') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    $proc = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -ArgumentList $argText `
        -WorkingDirectory $WorkRoot -RedirectStandardOutput $stdout -RedirectStandardError $stderr -NoNewWindow -PassThru
    [void]$proc.Handle
    if (-not $proc.WaitForExit(300000)) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        throw "$Script did not end within 5 minutes"
    }
    $text = [IO.File]::ReadAllText($stdout) + [IO.File]::ReadAllText($stderr)
    foreach ($line in ($text -split "`r?`n")) { if ($line) { Write-Host "      | $line" } }
    return [pscustomobject]@{ Code = $proc.ExitCode; Text = $text }
}

foreach ($f in @($notesScript, $ciTestsScript)) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) {
        Write-Host "test-release-tools: missing $f"
        exit 1
    }
}

# ---------------------------------------------------------------------------
# release-notes.ps1

$fx = Join-Path $WorkRoot 'notes'
$cmake123 = Join-Path $fx 'CMakeLists.txt'
Write-Text $cmake123 "cmake_minimum_required(VERSION 3.24)`r`nproject(ac_dlssg_bridge VERSION 1.2.3 LANGUAGES CXX)`r`n"
$changelog = Join-Path $fx 'CHANGELOG.md'
$section123 = "Frame generation for the fixture.`r`n`r`n### Added`r`n`r`n- One thing.`r`n- Another thing.`r`n`r`n### Known limitations`r`n`r`n- A limit."
Write-Text $changelog ("# Changelog`r`n`r`nIntro text.`r`n`r`n## [1.2.4] - 2026-10-01`r`n`r`n- Later.`r`n`r`n" +
    "## [1.2.3] - 2026-09-29`r`n`r`n$section123`r`n`r`n## [1.0.0] - 2026-09-01`r`n`r`n- Earlier.`r`n")
$emptyChangelog = Join-Path $fx 'CHANGELOG-empty.md'
Write-Text $emptyChangelog "# Changelog`r`n`r`n## [1.2.3] - 2026-09-29`r`n`r`n`r`n## [1.0.0] - 2026-09-01`r`n`r`n- Earlier.`r`n"

# A release zip as make-test-package.ps1 builds it: <name>/files/ac-dlssg.dll.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
function New-ReleaseZip([string]$Zip, [string]$Folder, [byte[]]$DllBytes, [switch]$NoDll) {
    $stage = Join-Path $fx ("stage-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $root = Join-Path $stage $Folder
    New-Item -ItemType Directory -Path (Join-Path $root 'files') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'install.bat'), "@echo off`r`n")
    if (-not $NoDll) { [IO.File]::WriteAllBytes((Join-Path $root 'files\ac-dlssg.dll'), $DllBytes) }
    [System.IO.Compression.ZipFile]::CreateFromDirectory($root, $Zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)
}
$dllBytes = [byte[]](1..200 | ForEach-Object { $_ % 256 })
$fixtureDll = Join-Path $fx 'ac-dlssg.dll'
[IO.File]::WriteAllBytes($fixtureDll, $dllBytes)
$otherDll = Join-Path $fx 'other\ac-dlssg.dll'
New-Item -ItemType Directory -Path (Split-Path -Parent $otherDll) -Force | Out-Null
[IO.File]::WriteAllBytes($otherDll, [byte[]](1..201 | ForEach-Object { $_ % 256 }))
$zip = Join-Path $fx 'ac-dlssg-1.2.3.zip'
New-ReleaseZip $zip 'ac-dlssg-1.2.3' $dllBytes
$wrongName = Join-Path $fx 'wrong\ac-dlssg-1.2.4.zip'
New-Item -ItemType Directory -Path (Split-Path -Parent $wrongName) -Force | Out-Null
New-ReleaseZip $wrongName 'ac-dlssg-1.2.4' $dllBytes
$noDllZip = Join-Path $fx 'nodll\ac-dlssg-1.2.3.zip'
New-Item -ItemType Directory -Path (Split-Path -Parent $noDllZip) -Force | Out-Null
New-ReleaseZip $noDllZip 'ac-dlssg-1.2.3' $dllBytes -NoDll

function Invoke-Notes([string[]]$Arguments) {
    return Invoke-Script $notesScript (@('-CMakeLists', $cmake123, '-Changelog', $changelog) + @($Arguments))
}

Invoke-Case 'RN1: the notes are the version''s CHANGELOG section plus the checksums and the verify line' {
    $out = Join-Path $fx 'out\notes-1.md'
    $r = Invoke-Notes @('-Zip', $zip, '-Dll', $fixtureDll, '-OutFile', $out)
    Check ($r.Code -eq 0) 'release-notes exits 0'
    Check (Test-Path -LiteralPath $out -PathType Leaf) 'the notes file is written'
    if (-not (Test-Path -LiteralPath $out -PathType Leaf)) { return }
    $bytes = [IO.File]::ReadAllBytes($out)
    Check (-not ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)) 'the notes are UTF-8 without a BOM'
    $text = $utf8.GetString($bytes)
    $norm = $text -replace "`r`n", "`n"
    Check ($norm.StartsWith(($section123 -replace "`r`n", "`n") + "`n")) 'the notes start with the 1.2.3 section, without its heading'
    Check ($norm -notmatch 'Later\.|Earlier\.|Intro text|## \[1\.2\.3\]') 'nothing from the other sections, the intro or the heading'
    $zipSha = Get-Sha $zip
    $dllSha = Get-Sha $fixtureDll
    Check ($norm.Contains("| ``ac-dlssg-1.2.3.zip`` | ``$zipSha`` |")) 'the zip''s SHA-256 is listed'
    Check ($norm.Contains("| ``ac-dlssg.dll`` (``files\ac-dlssg.dll`` in the zip) | ``$dllSha`` |")) 'the DLL''s SHA-256 is listed'
    Check ($norm.Contains("`nVerify with: ``gh attestation verify <file> --repo flopsy2iqq/ac-dlssg```n")) 'the verify line names the repository'
    Check ($r.Text.Contains($zipSha)) 'the notes are printed too'
}

Invoke-Case 'RN2: the DLL hash is read from the zip, and -Dll must match it' {
    $out = Join-Path $fx 'out\notes-2.md'
    $r = Invoke-Notes @('-Zip', $zip, '-OutFile', $out)
    Check ($r.Code -eq 0) 'without -Dll: exits 0'
    if (Test-Path -LiteralPath $out) {
        Check ((Get-Content -LiteralPath $out -Raw).Contains((Get-Sha $fixtureDll))) 'the DLL hash is the one of files\ac-dlssg.dll in the zip'
    } else { Check $false 'the notes file is written' }
    $out3 = Join-Path $fx 'out\notes-3.md'
    $r = Invoke-Notes @('-Zip', $zip, '-Dll', $otherDll, '-OutFile', $out3)
    Check ($r.Code -ne 0) 'a -Dll that differs from the zip''s DLL: exits non-zero'
    Check ($r.Text -match 'differs') 'and says that it differs'
    Check (-not (Test-Path -LiteralPath $out3)) 'and writes no notes'
    $r = Invoke-Notes @('-Zip', $noDllZip, '-OutFile', (Join-Path $fx 'out\notes-4.md'))
    Check ($r.Code -ne 0 -and $r.Text -match 'files/ac-dlssg\.dll') 'a zip without files/ac-dlssg.dll: exits non-zero and names the entry'
}

Invoke-Case 'RN3: the tag must be v<version>, and the zip must be ac-dlssg-<version>.zip' {
    $r = Invoke-Notes @('-Tag', 'v1.2.3', '-Zip', $zip, '-OutFile', (Join-Path $fx 'out\notes-5.md'))
    Check ($r.Code -eq 0) '-Tag v1.2.3 for version 1.2.3: exits 0'
    $r = Invoke-Notes @('-Tag', 'v1.2.4', '-Zip', $zip, '-OutFile', (Join-Path $fx 'out\notes-6.md'))
    Check ($r.Code -ne 0 -and $r.Text.Contains('v1.2.4') -and $r.Text.Contains('1.2.3')) '-Tag v1.2.4 for version 1.2.3: exits non-zero and names both'
    $r = Invoke-Notes @('-Tag', '1.2.3')
    Check ($r.Code -ne 0) '-Tag without the v: exits non-zero'
    $r = Invoke-Notes @('-Zip', $wrongName, '-OutFile', (Join-Path $fx 'out\notes-7.md'))
    Check ($r.Code -ne 0 -and $r.Text.Contains('ac-dlssg-1.2.3.zip')) 'a zip named for another version: exits non-zero and names the expected name'
}

Invoke-Case 'RN4: a version without a CHANGELOG section, or with an empty one, is refused' {
    $r = Invoke-Script $notesScript @('-CMakeLists', $cmake123, '-Changelog', $changelog, '-Version', '9.9.9')
    Check ($r.Code -ne 0 -and $r.Text.Contains('9.9.9')) 'no section for 9.9.9: exits non-zero and names the version'
    $r = Invoke-Script $notesScript @('-CMakeLists', $cmake123, '-Changelog', $emptyChangelog)
    Check ($r.Code -ne 0 -and $r.Text -match 'empty') 'an empty 1.2.3 section: exits non-zero and says it is empty'
    $r = Invoke-Script $notesScript @('-CMakeLists', $cmake123, '-Changelog', (Join-Path $fx 'missing.md'))
    Check ($r.Code -ne 0) 'a missing changelog: exits non-zero'
}

Invoke-Case 'RN5: without -Zip it only checks and prints the section; the version defaults to CMakeLists.txt' {
    $r = Invoke-Script $notesScript @('-CMakeLists', $cmake123, '-Changelog', $changelog)
    Check ($r.Code -eq 0) 'exits 0'
    Check ($r.Text.Contains('Another thing.') -and -not $r.Text.Contains('Later.')) 'prints the 1.2.3 section (from CMakeLists.txt''s VERSION)'
    Check ($r.Text -notmatch 'SHA-256') 'no checksums without a zip'
    $r = Invoke-Script $notesScript @('-CMakeLists', $cmake123, '-Changelog', $changelog, '-Version', '1.2.4')
    Check ($r.Code -eq 0 -and $r.Text.Contains('Later.') -and -not $r.Text.Contains('Another thing.')) '-Version 1.2.4 picks that section'
    $badCmake = Join-Path $fx 'bad\CMakeLists.txt'
    Write-Text $badCmake "project(x LANGUAGES CXX)`r`n"
    $r = Invoke-Script $notesScript @('-CMakeLists', $badCmake, '-Changelog', $changelog)
    Check ($r.Code -ne 0) 'a CMakeLists.txt without a project VERSION: exits non-zero'
}

# ---------------------------------------------------------------------------
# run-ci-tests.ps1, against a fake runner that behaves like acdb_tests.exe:
# it runs every registered test whose name contains the filter, except the
# Child_ tests, prints PASS/FAIL lines and the summary, and exits with the
# number of failed tests. Every filter it is called with is appended to
# calls.txt.

$ct = Join-Path $WorkRoot 'ci-tests'
$fakeDir = Join-Path $ct 'fake'
Write-Text (Join-Path $fakeDir 'fake-tests.cmd') ("@echo off`r`n" +
    "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ""%~dp0fake-tests.ps1"" %*`r`n" +
    "exit /b %ERRORLEVEL%`r`n")
Write-Text (Join-Path $fakeDir 'fake-tests.ps1') @'
param([string]$Filter)
$dir = $PSScriptRoot
Add-Content -LiteralPath (Join-Path $dir 'calls.txt') -Value $Filter
$passed = 0; $failed = 0; $skipped = 0
foreach ($line in [IO.File]::ReadAllLines((Join-Path $dir 'registered.txt'))) {
    if (-not $line) { continue }
    $parts = $line -split ' '
    $name = $parts[0]
    if (($Filter -and -not $name.Contains($Filter)) -or $name.StartsWith('Child_')) { $skipped++; continue }
    if ($parts.Count -gt 1 -and $parts[1] -eq 'FAIL') {
        Write-Output "  FAIL fake.cpp:1: CHECK(false)"
        Write-Output "FAIL $name"
        $failed++
    } else {
        Write-Output "  | a stats line"
        Write-Output "PASS $name"
        $passed++
    }
}
Write-Output ''
Write-Output "$passed passed, $failed failed, $skipped filtered out"
exit $failed
'@
$fakeExe = Join-Path $fakeDir 'fake-tests.cmd'

function New-CiFixture([string]$Name, [hashtable]$Sources, [string[]]$Registered) {
    $dir = Join-Path $ct $Name
    foreach ($file in $Sources.Keys) {
        $body = ($Sources[$file] | ForEach-Object { "TEST($_) {`r`n    CHECK(true);`r`n}`r`n" }) -join "`r`n"
        Write-Text (Join-Path $dir "tests\$file") ("#include ""test_framework.h""`r`n`r`n" + $body)
    }
    Write-Text (Join-Path $fakeDir 'registered.txt') (($Registered -join "`r`n") + "`r`n")
    $calls = Join-Path $fakeDir 'calls.txt'
    if (Test-Path -LiteralPath $calls) { Remove-Item -LiteralPath $calls -Force }
    return $dir
}

function Get-Calls {
    $calls = Join-Path $fakeDir 'calls.txt'
    if (-not (Test-Path -LiteralPath $calls)) { return @() }
    return @([IO.File]::ReadAllLines($calls) | Where-Object { $_ })
}

function Invoke-CiTests([string]$Dir, [string]$Source) {
    return Invoke-Script $ciTestsScript @('-TestExe', $fakeExe, '-TestsDir', (Join-Path $Dir 'tests'), '-OutDir', (Join-Path $Dir 'out'),
        '-Source', $Source)
}

$allNames = @('Foo_A', 'Foo_B', 'Bar_C', 'Gpu_D', 'Child_Foo_X')

Invoke-Case 'CT1: the tests of the listed files run, one filter per name prefix' {
    $d = New-CiFixture 'ct1' @{ 'test_free.cpp' = @('Foo_A', 'Foo_B'); 'test_more.cpp' = @('Bar_C'); 'test_gpu.cpp' = @('Gpu_D', 'Child_Foo_X') } $allNames
    $r = Invoke-CiTests $d 'test_free.cpp,test_more.cpp'
    Check ($r.Code -eq 0) 'run-ci-tests exits 0'
    $calls = @(Get-Calls | Sort-Object)
    Check (($calls -join '|') -eq 'Bar_|Foo_') "the runner is called once per prefix, Bar_ and Foo_ ($($calls -join ', '))"
    Check ($r.Text -match '3 tests passed' -and $r.Text.Contains('test_free.cpp') -and $r.Text.Contains('test_more.cpp')) 'the summary counts 3 tests and names the files'
    Check (-not $r.Text.Contains('Gpu_D')) 'the GPU file''s test never runs'
    Check (Test-Path -LiteralPath (Join-Path $d 'out\Foo_.txt') -PathType Leaf) 'each run''s output is kept in -OutDir'
}

Invoke-Case 'CT2: a prefix that another file''s test contains is run name by name' {
    $d = New-CiFixture 'ct2' @{ 'test_free.cpp' = @('Foo_A', 'Foo_B'); 'test_gpu.cpp' = @('NgxFoo_E') } @('Foo_A', 'Foo_B', 'NgxFoo_E')
    $r = Invoke-CiTests $d 'test_free.cpp'
    Check ($r.Code -eq 0) 'run-ci-tests exits 0'
    $calls = @(Get-Calls | Sort-Object)
    Check (($calls -join '|') -eq 'Foo_A|Foo_B') "the runner is called with Foo_A and Foo_B, never Foo_ ($($calls -join ', '))"
}

Invoke-Case 'CT3: a name that another file''s test contains is refused before anything runs' {
    $d = New-CiFixture 'ct3' @{ 'test_free.cpp' = @('Foo_A'); 'test_gpu.cpp' = @('NgxFoo_A') } @('Foo_A', 'NgxFoo_A')
    $r = Invoke-CiTests $d 'test_free.cpp'
    Check ($r.Code -ne 0) 'run-ci-tests exits non-zero'
    Check ($r.Text.Contains('NgxFoo_A')) 'and names the other test'
    Check (@(Get-Calls).Count -eq 0) 'the runner is never called'
}

Invoke-Case 'CT4: a failing test fails the run and is named' {
    $d = New-CiFixture 'ct4' @{ 'test_free.cpp' = @('Foo_A', 'Foo_B') } @('Foo_A', 'Foo_B FAIL')
    $r = Invoke-CiTests $d 'test_free.cpp'
    Check ($r.Code -ne 0) 'run-ci-tests exits non-zero'
    Check ($r.Text -match 'FAIL.*Foo_B') 'the failing test is named'
    Check ($r.Text.Contains('fake.cpp:1')) 'its failure lines are shown'
}

Invoke-Case 'CT5: a listed test that the runner does not run fails the run' {
    $d = New-CiFixture 'ct5' @{ 'test_free.cpp' = @('Foo_A', 'Foo_B') } @('Foo_A')
    $r = Invoke-CiTests $d 'test_free.cpp'
    Check ($r.Code -ne 0) 'run-ci-tests exits non-zero'
    Check ($r.Text.Contains('Foo_B')) 'and names the test that did not run'
}

Invoke-Case 'CT6: a listed file that is missing or has no tests fails the run' {
    $d = New-CiFixture 'ct6' @{ 'test_free.cpp' = @('Foo_A'); 'test_empty.cpp' = @() } @('Foo_A')
    $r = Invoke-CiTests $d 'test_free.cpp,test_missing.cpp'
    Check ($r.Code -ne 0 -and $r.Text.Contains('test_missing.cpp')) 'a missing file: exits non-zero and names it'
    $r = Invoke-CiTests $d 'test_free.cpp,test_empty.cpp'
    Check ($r.Code -ne 0 -and $r.Text.Contains('test_empty.cpp')) 'a file without tests: exits non-zero and names it'
}

# ---------------------------------------------------------------------------
# The repository itself.

Invoke-Case 'RP1: CHANGELOG.md has a section for the project version' {
    $r = Invoke-Script $notesScript @()
    Check ($r.Code -eq 0) 'release-notes.ps1 with the repository defaults exits 0'
}

# build.yml uploads build\package\ac-dlssg-<VERSION>.zip and release.yml
# attaches it, so the package's default name must follow CMakeLists.txt.
Invoke-Case 'RP2: without -Version, make-test-package.ps1 names the zip after the project VERSION' {
    Check (Test-Path -LiteralPath $Dll -PathType Leaf) "the bridge DLL exists ($Dll; build Release first)"
    if (-not (Test-Path -LiteralPath $Dll -PathType Leaf)) { return }
    $m = [regex]::Match([IO.File]::ReadAllText((Join-Path $repo 'CMakeLists.txt')), 'project\s*\(\s*\S+\s+VERSION\s+([0-9][0-9.]*)')
    Check $m.Success 'CMakeLists.txt has a project VERSION'
    if (-not $m.Success) { return }
    $version = $m.Groups[1].Value
    $out = Join-Path $WorkRoot 'package'
    $r = Invoke-Script (Join-Path $tools 'make-test-package.ps1') @('-Dll', $Dll, '-OutDir', $out)
    Check ($r.Code -eq 0) 'make-test-package exits 0'
    $zip = Join-Path $out "ac-dlssg-$version.zip"
    Check (Test-Path -LiteralPath $zip -PathType Leaf) "the zip is ac-dlssg-$version.zip"
    Check ((Test-Path -LiteralPath $out) -and @(Get-ChildItem -LiteralPath $out -Filter '*.zip' -File).Count -eq 1) 'and it is the only zip'
    if (Test-Path -LiteralPath $zip -PathType Leaf) {
        $n = Invoke-Script $notesScript @('-Zip', $zip, '-Dll', $Dll, '-OutFile', (Join-Path $WorkRoot 'package\notes.md'))
        Check ($n.Code -eq 0) 'release-notes.ps1 accepts it, with the DLL inside matching the build'
    }
}

Write-Host ''
Write-Host "test-release-tools: $($script:passed) passed, $($script:failed) failed"
if ($script:failed -gt 0) {
    $script:failedNames | ForEach-Object { Write-Host "  FAIL $_" }
    Write-Host "test-release-tools: the work folder is kept: $WorkRoot"
    exit 1
}
if (-not $Keep) { [void](Remove-WorkRoot) }
exit 0
