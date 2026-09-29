<#
.SYNOPSIS
  Runs the unit tests of the given test source files, and only those, for CI
  runners that have no GPU.

.DESCRIPTION
  acdb_tests.exe takes one filter and runs every test whose name contains it.
  This script reads the TEST(<name>) lines of every tests\*.cpp file, takes
  the names from the -Source files (Child_ tests run only as child processes
  and are left out), and calls the test runner once per name prefix
  ("Config_", "FgGate_", ...). A prefix that a test of another file also
  contains is run name by name instead, and a name that another file's test
  contains stops the script before anything runs, so that no test outside
  the -Source files is ever started.

  Every call goes through cmd.exe with its output redirected to
  <OutDir>\<filter>.txt, never through a pipe. A call must exit 0, and the
  tests it ran (its PASS and FAIL lines) must be exactly the ones expected
  for its filter. A failing run prints that call's output.

  Exit code 0 when every expected test ran and passed, 1 otherwise.

.PARAMETER Source
  Test source file names in -TestsDir, such as test_ini_config.cpp; a single
  comma-separated string works too.

.PARAMETER TestExe
  Default: build\Release\acdb_tests.exe.

.PARAMETER TestsDir
  Default: the repository's tests folder.

.PARAMETER OutDir
  Default: build\ci-tests. Re-created on every run.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\run-ci-tests.ps1 -Source test_ini_config.cpp,test_fg_policy.cpp
#>
param(
    [Parameter(Mandatory = $true)]
    [string[]]$Source,
    [string]$TestExe,
    [string]$TestsDir,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $TestExe) { $TestExe = Join-Path $repoRoot 'build\Release\acdb_tests.exe' }
if (-not $TestsDir) { $TestsDir = Join-Path $repoRoot 'tests' }
if (-not $OutDir) { $OutDir = Join-Path $repoRoot 'build\ci-tests' }
# "powershell -File" passes "-Source a,b" as one string.
$Source = @($Source | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

try {
    if (-not (Test-Path -LiteralPath $TestExe -PathType Leaf)) { throw "test runner not found: $TestExe" }
    $TestExe = (Resolve-Path -LiteralPath $TestExe).ProviderPath
    if (-not (Test-Path -LiteralPath $TestsDir -PathType Container)) { throw "tests folder not found: $TestsDir" }
    if ($Source.Count -eq 0) { throw 'no -Source files given' }

    # Every registered test name, with its file. Child_ tests never run from a filter.
    $fileOf = @{}
    foreach ($f in @(Get-ChildItem -LiteralPath $TestsDir -Filter '*.cpp' -File)) {
        foreach ($m in [regex]::Matches([IO.File]::ReadAllText($f.FullName), '(?m)^TEST\((\w+)\)')) {
            $name = $m.Groups[1].Value
            if (-not $name.StartsWith('Child_')) { $fileOf[$name] = $f.Name }
        }
    }
    $allowed = New-Object System.Collections.Generic.List[string]
    foreach ($s in $Source) {
        if (-not (Test-Path -LiteralPath (Join-Path $TestsDir $s) -PathType Leaf)) { throw "test source not found: $s in $TestsDir" }
        $names = @($fileOf.Keys | Where-Object { $fileOf[$_] -ieq $s } | Sort-Object)
        if ($names.Count -eq 0) { throw "$s registers no TEST(...) that a filter can run" }
        foreach ($n in $names) { $allowed.Add($n) }
    }
    $allowedSet = @{}
    foreach ($n in $allowed) { $allowedSet[$n] = $true }

    # The tests a filter selects in acdb_tests.exe: every name containing it.
    function Get-Selected([string]$Filter) {
        return @($fileOf.Keys | Where-Object { $_.Contains($Filter) } | Sort-Object)
    }

    # One filter per prefix when it selects only allowed tests, else one per name.
    $runs = New-Object System.Collections.Generic.List[object]
    foreach ($group in @($allowed | Group-Object { if ($_.Contains('_')) { $_.Substring(0, $_.IndexOf('_') + 1) } else { $_ } } | Sort-Object Name)) {
        $prefix = $group.Name
        $selected = Get-Selected $prefix
        $foreign = @($selected | Where-Object { -not $allowedSet.ContainsKey($_) })
        if ($foreign.Count -eq 0) {
            $runs.Add([pscustomobject]@{ Filter = $prefix; Expected = $selected })
            continue
        }
        foreach ($n in @($group.Group | Sort-Object)) {
            $selected = Get-Selected $n
            $foreign = @($selected | Where-Object { -not $allowedSet.ContainsKey($_) })
            if ($foreign.Count -gt 0) {
                throw "the filter $n would also run $(($foreign | ForEach-Object { "$_ ($($fileOf[$_]))" }) -join ', '); rename one of them"
            }
            $runs.Add([pscustomobject]@{ Filter = $n; Expected = $selected })
        }
    }

    if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    Write-Host "run-ci-tests: $TestExe"
    Write-Host "  files: $($Source -join ', ')"
    Write-Host "  $($allowed.Count) tests in $($runs.Count) runs"

    $problems = New-Object System.Collections.Generic.List[string]
    $passed = 0
    foreach ($run in $runs) {
        $out = Join-Path $OutDir "$($run.Filter).txt"
        # cmd.exe with a file redirect: the runner and anything it starts get
        # file handles, never this process's pipes. Start-Process passes the
        # argument string as it is, which "& cmd.exe" in Windows PowerShell
        # does not for embedded quotes.
        $cmdArgs = '/d /c ""' + $TestExe + '" ' + $run.Filter + ' > "' + $out + '" 2>&1"'
        $proc = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\cmd.exe') -ArgumentList $cmdArgs `
            -WorkingDirectory $OutDir -NoNewWindow -PassThru
        # Opened now, so that the exit code is still there after the wait.
        [void]$proc.Handle
        $proc.WaitForExit()
        $code = $proc.ExitCode
        $lines = @(if (Test-Path -LiteralPath $out) { [IO.File]::ReadAllLines($out) })
        $ran = @($lines | ForEach-Object { if ($_ -match '^(PASS|FAIL) (\S+)$') { $Matches[2] } } | Sort-Object)
        $failedHere = @($lines | ForEach-Object { if ($_ -match '^FAIL (\S+)$') { $Matches[1] } })
        $missing = @($run.Expected | Where-Object { $ran -notcontains $_ })
        $extra = @($ran | Where-Object { $run.Expected -notcontains $_ })
        $runProblems = @()
        if ($failedHere.Count -gt 0) { $runProblems += "failed: $($failedHere -join ', ')" }
        if ($missing.Count -gt 0) { $runProblems += "did not run: $($missing -join ', ')" }
        if ($extra.Count -gt 0) { $runProblems += "ran tests outside the list: $($extra -join ', ')" }
        if ($code -ne 0 -and $runProblems.Count -eq 0) { $runProblems += "exit code $code" }
        if ($runProblems.Count -gt 0) {
            Write-Host "  FAIL  $($run.Filter): $($runProblems -join '; ')"
            foreach ($l in $lines) { Write-Host "      | $l" }
            $problems.Add("$($run.Filter): $($runProblems -join '; ')")
        } else {
            Write-Host ("  ok    {0,-24} {1} tests" -f $run.Filter, $ran.Count)
        }
        $passed += @($ran | Where-Object { $failedHere -notcontains $_ }).Count
    }

    if ($problems.Count -gt 0) {
        Write-Host "run-ci-tests: FAILED: $($problems.Count) of $($runs.Count) runs; output in $OutDir"
        exit 1
    }
    Write-Host "run-ci-tests: $passed tests passed from $($Source -join ', ')"
    exit 0
} catch {
    Write-Host "run-ci-tests: FAILED: $($_.Exception.Message)"
    exit 1
}
