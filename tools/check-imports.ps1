<#
.SYNOPSIS
  Fails when a DLL depends on anything outside the allow-list (spec 6.1, 12).

.DESCRIPTION
  ReShade has no System32 fallback for a ProxyLibrary that fails to load, so
  ac-dlssg.dll may only import DLLs that exist on every Windows 10/11
  x64 installation. This runs "dumpbin /dependents" (found through vswhere)
  and checks both normal and delay-load dependencies. The allow-list lives in
  dev-common.ps1, whose Test-BridgeDll (used by dev-install.ps1) reads the
  import directories itself. CMake registers this script as the "imports"
  test.

  With -Helper it checks the installer's driver-profile helper
  ac-dlssg-drs.exe against its own, smaller list (KERNEL32.dll only,
  $script:AcdbHelperImportAllowList): the helper may run with administrator
  rights from the game folder, so it must not load a DLL that could be taken
  from there. CMake registers that as the "imports_drs" test.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-imports.ps1 -Dll build\Release\ac-dlssg.dll

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-imports.ps1 -Dll build\Release\ac-dlssg-drs.exe -Helper
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Dll,
    [switch]$Helper
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'dev-common.ps1')

# One list for this check and for dev-install's Test-BridgeDll.
$allowList = if ($Helper) { $script:AcdbHelperImportAllowList } else { $script:AcdbImportAllowList }

if (-not (Test-Path -LiteralPath $Dll -PathType Leaf)) {
    Write-Host "check-imports: file not found: $Dll"
    exit 1
}
$dllPath = (Resolve-Path -LiteralPath $Dll).Path

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    Write-Host "check-imports: vswhere.exe not found at $vswhere"
    exit 1
}
$dumpbin = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
if (-not $dumpbin -or -not (Test-Path -LiteralPath $dumpbin -PathType Leaf)) {
    Write-Host 'check-imports: dumpbin.exe not found through vswhere'
    exit 1
}

$output = & $dumpbin /nologo /dependents $dllPath
if ($LASTEXITCODE -ne 0) {
    Write-Host "check-imports: dumpbin failed with exit code $LASTEXITCODE"
    $output | ForEach-Object { Write-Host $_ }
    exit 1
}

# Dependency lines are indented bare file names. The surrounding headers are
# localized, so they are not used for parsing.
$dependencies = @($output | ForEach-Object { if ($_ -match '^\s+(\S+\.dll)\s*$') { $Matches[1] } } |
    Sort-Object -Unique)

$offenders = @($dependencies | Where-Object {
        $name = $_
        -not ($allowList | Where-Object { $_ -ieq $name })
    })

Write-Host "check-imports: $dllPath$(if ($Helper) { ' (the driver-profile helper)' })"
Write-Host ("  dependencies: " + ($(if ($dependencies.Count) { $dependencies -join ', ' } else { '(none)' })))
if ($offenders.Count -gt 0) {
    Write-Host 'check-imports: FAILED, dependencies outside the allow-list:'
    $offenders | ForEach-Object { Write-Host "  $_" }
    Write-Host ("  allowed: " + ($allowList -join ', '))
    exit 1
}
Write-Host 'check-imports: OK'
exit 0
