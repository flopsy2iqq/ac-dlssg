<#
.SYNOPSIS
  Tests dev-install.ps1 and dev-uninstall.ps1 against throw-away fake game folders.

.DESCRIPTION
  Every case builds its own fake Assetto Corsa folder under -FakeRoot (a dummy
  acs.exe, a fake ReShade dxgi.dll, ReShade.ini fixtures and a hand-made x64 PE
  file that stands in for ac-dlssg.dll), runs the scripts in a child Windows
  PowerShell and checks the resulting bytes.

  The scripts are never pointed at a real game folder: every run passes
  -GameDir, and the runner refuses any -GameDir outside -FakeRoot.

  The Streamline runtime installed by every case is the real, NVIDIA-signed
  one from -StreamlineDir (default: deps\streamline-2.14.1\bin\x64, staged by
  fetch-deps.ps1). Copies of it under -FakeRoot stand in for a newer build
  (another signed DLL under one of the names), a tampered DLL (one byte
  changed) and an unsigned one (certificate table removed).

  The fake dxgi.dll is a resource-only DLL whose version resource ProductName
  is "ReShade". It is built once with the Windows SDK's rc.exe and MSVC's
  link.exe, which the project's build needs anyway.

  TEMP and TMP point into -FakeRoot for this process and its children, so
  PowerShell, rc.exe and link.exe leave nothing in the user's temp folder.

  -FakeRoot (default: build\dev-scripts-test) is deleted and re-created on
  every run, and deleted again after a passing run unless -Keep is given. It is
  only deleted when it holds the marker file this script writes, so a folder
  that was not created by this script is never removed.

  One case reads collect-sysinfo.ps1 without running it: the wording of its
  HAGS section, and that it contains no command that changes the system.

  Exits 0 when every check passes, 1 otherwise.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\test-dev-scripts.ps1
#>
param(
    [string]$FakeRoot,
    [string]$StreamlineDir,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$tools = $PSScriptRoot
$installScript = Join-Path $tools 'dev-install.ps1'
$uninstallScript = Join-Path $tools 'dev-uninstall.ps1'
$commonScript = Join-Path $tools 'dev-common.ps1'

foreach ($f in @($installScript, $uninstallScript, $commonScript)) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) {
        Write-Host "test-dev-scripts: missing $f"
        exit 1
    }
}
. $commonScript

if (-not $FakeRoot) { $FakeRoot = Join-Path $tools '..\build\dev-scripts-test' }
$FakeRoot = [IO.Path]::GetFullPath($FakeRoot).TrimEnd('\')
$markerName = '.acdb-dev-scripts-test-root'

function Remove-FakeRoot {
    if (-not (Test-Path -LiteralPath $FakeRoot)) { return $true }
    if (-not (Test-Path -LiteralPath (Join-Path $FakeRoot $markerName) -PathType Leaf)) {
        Write-Host "test-dev-scripts: $FakeRoot exists but has no $markerName marker; refusing to delete it"
        return $false
    }
    Remove-Item -LiteralPath $FakeRoot -Recurse -Force
    return $true
}

# All test cases would fail against a running game, and the runner never
# stops a process it did not start.
$realGame = @(Get-Process -Name acs -ErrorAction SilentlyContinue)
if ($realGame.Count -gt 0) {
    Write-Host "test-dev-scripts: acs.exe is running (pid $(($realGame | ForEach-Object { $_.Id }) -join ', ')); close it first"
    exit 1
}

if (-not (Remove-FakeRoot)) { exit 1 }
New-Item -ItemType Directory -Path $FakeRoot | Out-Null
[IO.File]::WriteAllText((Join-Path $FakeRoot $markerName), 'created by tools\test-dev-scripts.ps1')

$tempDir = Join-Path $FakeRoot 'tmp'
New-Item -ItemType Directory -Path $tempDir | Out-Null
$env:TEMP = $tempDir
$env:TMP = $tempDir

# The child processes inherit this; only the env-override cases set it.
Remove-Item Env:\RESHADE_BASE_PATH_OVERRIDE -ErrorAction SilentlyContinue

$script:passed = 0
$script:failed = 0
$script:failedNames = New-Object System.Collections.Generic.List[string]
$script:currentCase = ''
$utf8 = New-Object System.Text.UTF8Encoding($false)
$ourDll = 'ac-dlssg.dll'

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

# Runs one case; an unexpected error fails that case and the run goes on.
function Invoke-Case([string]$Name, [scriptblock]$Body) {
    $script:currentCase = $Name
    Write-Host ''
    Write-Host "== $Name"
    try {
        & $Body
    } catch {
        Check $false "no unexpected error ($($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber))"
    } finally {
        Remove-Item Env:\RESHADE_BASE_PATH_OVERRIDE -ErrorAction SilentlyContinue
    }
}

function Test-UnderFakeRoot([string]$Path) {
    if (-not $Path) { return $false }
    $full = [IO.Path]::GetFullPath($Path)
    return $full.StartsWith($FakeRoot + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Invoke-Tool([string]$Script, [string[]]$Arguments) {
    # Safety net: without -GameDir the scripts would find the real game.
    $i = [array]::IndexOf($Arguments, '-GameDir')
    if ($i -lt 0 -or $i + 1 -ge $Arguments.Count -or -not (Test-UnderFakeRoot $Arguments[$i + 1])) {
        throw "refusing to run $Script without a -GameDir under $FakeRoot"
    }
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $Script @Arguments 2>&1 |
                ForEach-Object { "$_" })
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $eap
    }
    foreach ($line in $output) { Write-Host "      | $line" }
    return [pscustomobject]@{ Code = $code; Text = ($output -join "`n") }
}

# -Sl '' leaves -StreamlineDir to the script's default.
function Install([string]$Game, [string]$Dll, [switch]$Force, [string]$Sl = $slReal) {
    $a = @('-GameDir', $Game)
    if ($Dll) { $a += @('-Dll', $Dll) }
    if ($Sl) { $a += @('-StreamlineDir', $Sl) }
    if ($Force) { $a += '-Force' }
    return Invoke-Tool $installScript $a
}

function Uninstall([string]$Game, [switch]$RemoveData) {
    $a = @('-GameDir', $Game)
    if ($RemoveData) { $a += '-RemoveData' }
    return Invoke-Tool $uninstallScript $a
}

function Get-Utf8Bytes([string]$Text) { return , $utf8.GetBytes($Text) }

function Join-Bytes([object[]]$Parts) {
    $list = New-Object System.Collections.Generic.List[byte]
    foreach ($p in $Parts) { $list.AddRange([byte[]]$p) }
    return , $list.ToArray()
}

function Write-Bytes([string]$Path, [byte[]]$Bytes) {
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    [IO.File]::WriteAllBytes($Path, $Bytes)
}

function Write-Text([string]$Path, [string]$Text) { Write-Bytes $Path (Get-Utf8Bytes $Text) }

function Read-Bytes([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return , [byte[]]@() }
    return , [IO.File]::ReadAllBytes($Path)
}

function Test-SameBytes([byte[]]$A, [byte[]]$B) {
    return [Convert]::ToBase64String($A) -eq [Convert]::ToBase64String($B)
}

function Get-Sha([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Test-Refused($Result) { return $Result.Code -ne 0 -and $Result.Text -match 'REFUSED:' }

# A structurally valid PE32+ DLL header with an export table, and optionally
# import and delay-import directories that name DLLs. It is never loaded; it
# only has to satisfy the install script's header, export and import checks.
function New-FakePeDll([string]$Path, [string[]]$Exports, [uint16]$Machine = 0x8664, [string]$Salt = '',
    [string[]]$Imports = @(), [string[]]$DelayImports = @()) {
    $names = @($Exports | Sort-Object)
    $n = $names.Count
    $secRva = 0x1000
    $dirSize = 40
    $funcOff = $dirSize
    $namePtrOff = $funcOff + 4 * $n
    $ordOff = $namePtrOff + 4 * $n
    $strOff = $ordOff + 2 * $n
    $strings = New-Object System.Collections.Generic.List[byte]
    $dllNameRva = $secRva + $strOff
    $strings.AddRange([Text.Encoding]::ASCII.GetBytes("ac-dlssg.dll$Salt")); $strings.Add(0)
    $nameRvas = @()
    foreach ($name in $names) {
        $nameRvas += $secRva + $strOff + $strings.Count
        $strings.AddRange([Text.Encoding]::ASCII.GetBytes($name)); $strings.Add(0)
    }
    $importNameRvas = @()
    foreach ($name in @($Imports) + @($DelayImports)) {
        $importNameRvas += $secRva + $strOff + $strings.Count
        $strings.AddRange([Text.Encoding]::ASCII.GetBytes($name)); $strings.Add(0)
    }
    # Import descriptors (20 bytes) and delay-import descriptors (32 bytes),
    # each list ended by a zero entry, after the strings.
    $impOff = [int]([Math]::Ceiling(($strOff + $strings.Count) / 8.0) * 8)
    $delayOff = $impOff + 20 * (@($Imports).Count + 1)
    $secSize = $delayOff + 32 * (@($DelayImports).Count + 1)
    $rawSize = [int]([Math]::Ceiling($secSize / 512.0) * 512)
    if ($rawSize -eq 0) { $rawSize = 512 }
    $b = New-Object byte[] (0x200 + $rawSize)
    $put16 = { param($o, $v) [BitConverter]::GetBytes([uint16]$v).CopyTo($b, $o) }
    $put32 = { param($o, $v) [BitConverter]::GetBytes([uint32]$v).CopyTo($b, $o) }
    $put64 = { param($o, $v) [BitConverter]::GetBytes([uint64]$v).CopyTo($b, $o) }
    $b[0] = 0x4D; $b[1] = 0x5A
    & $put32 0x3C 0x80
    $b[0x80] = 0x50; $b[0x81] = 0x45
    $coff = 0x84
    & $put16 ($coff + 0) $Machine
    & $put16 ($coff + 2) 1
    & $put16 ($coff + 16) 0xF0
    & $put16 ($coff + 18) 0x2022
    $opt = $coff + 20
    & $put16 ($opt + 0) 0x20B
    & $put64 ($opt + 24) 0x180000000
    & $put32 ($opt + 32) 0x1000
    & $put32 ($opt + 36) 0x200
    & $put32 ($opt + 56) (0x1000 + [int]([Math]::Ceiling($secSize / 4096.0) * 4096))
    & $put32 ($opt + 60) 0x200
    & $put16 ($opt + 68) 2
    & $put32 ($opt + 108) 16
    & $put32 ($opt + 112) $secRva
    & $put32 ($opt + 116) $secSize
    $sec = $opt + 0xF0
    [Text.Encoding]::ASCII.GetBytes('.edata').CopyTo($b, $sec)
    & $put32 ($sec + 8) $secSize
    & $put32 ($sec + 12) $secRva
    & $put32 ($sec + 16) $rawSize
    & $put32 ($sec + 20) 0x200
    & $put32 ($sec + 36) 0x40000040
    $d = 0x200
    & $put32 ($d + 12) $dllNameRva
    & $put32 ($d + 16) 1
    & $put32 ($d + 20) $n
    & $put32 ($d + 24) $n
    & $put32 ($d + 28) ($secRva + $funcOff)
    & $put32 ($d + 32) ($secRva + $namePtrOff)
    & $put32 ($d + 36) ($secRva + $ordOff)
    for ($i = 0; $i -lt $n; $i++) {
        & $put32 ($d + $funcOff + 4 * $i) $secRva
        & $put32 ($d + $namePtrOff + 4 * $i) $nameRvas[$i]
        & $put16 ($d + $ordOff + 2 * $i) $i
    }
    $strings.ToArray().CopyTo($b, $d + $strOff)
    $k = 0
    if (@($Imports).Count -gt 0) {
        & $put32 ($opt + 120) ($secRva + $impOff)
        & $put32 ($opt + 124) (20 * (@($Imports).Count + 1))
        for ($i = 0; $i -lt @($Imports).Count; $i++) {
            & $put32 ($d + $impOff + 20 * $i + 12) $importNameRvas[$k]
            $k++
        }
    }
    if (@($DelayImports).Count -gt 0) {
        & $put32 ($opt + 112 + 8 * 13) ($secRva + $delayOff)
        & $put32 ($opt + 116 + 8 * 13) (32 * (@($DelayImports).Count + 1))
        for ($i = 0; $i -lt @($DelayImports).Count; $i++) {
            & $put32 ($d + $delayOff + 32 * $i) 1  # Attributes: RVA-based
            & $put32 ($d + $delayOff + 32 * $i + 4) $importNameRvas[$k]
            $k++
        }
    }
    Write-Bytes $Path $b
}

# The Windows SDK's x64 rc.exe and MSVC's x64 link.exe, or $null.
function Find-ResourceTools {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $rc = Get-ChildItem -Path $kits -Filter rc.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\rc\.exe$' } | Sort-Object FullName -Descending | Select-Object -First 1
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $link = $null
    if (Test-Path -LiteralPath $vswhere) {
        $link = & $vswhere -latest -products * -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\link.exe' | Select-Object -First 1
    }
    Write-Host "    rc.exe: $(if ($rc) { $rc.FullName } else { 'not found' })"
    Write-Host "    link.exe: $(if ($link) { $link } else { 'not found' })"
    if (-not $rc -or -not $link) { return $null }
    return [pscustomobject]@{ Rc = $rc.FullName; Link = $link }
}

# Resource-only DLL with the given ProductName. Returns the path, or $null
# when rc.exe or link.exe fails.
function New-VersionResourceDll($ToolPaths, [string]$OutPath, [string]$ProductName) {
    $rc = $ToolPaths.Rc
    $link = $ToolPaths.Link
    $work = Join-Path (Split-Path -Parent $OutPath) 'obj'
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    $rcFile = Join-Path $work 'version.rc'
    $resFile = Join-Path $work 'version.res'
    Write-Text $rcFile ((@'
1 VERSIONINFO
FILEVERSION 6,8,0,2155
PRODUCTVERSION 6,8,0,2155
FILEOS 0x40004
FILETYPE 0x2
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "ProductName", "@PRODUCT@"
      VALUE "ProductVersion", "6.8.0.2155"
      VALUE "FileVersion", "6.8.0.2155"
      VALUE "FileDescription", "Fake dxgi.dll for the ac-dlssg dev script tests"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
'@ -replace "`r?`n", "`r`n") -replace '@PRODUCT@', $ProductName)
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $rc /nologo /fo $resFile $rcFile 2>&1
        if ($LASTEXITCODE -ne 0) { Write-Host "    rc.exe failed: $out"; return $null }
        $out = & $link /NOLOGO /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 "/OUT:$OutPath" $resFile 2>&1
        if ($LASTEXITCODE -ne 0) { Write-Host "    link.exe failed: $out"; return $null }
    } finally {
        $ErrorActionPreference = $eap
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ((Get-Item -LiteralPath $OutPath).VersionInfo.ProductName -ne $ProductName) { return $null }
    return $OutPath
}

$goodExports = @('CreateDXGIFactory', 'CreateDXGIFactory1', 'CreateDXGIFactory2', 'DXGIGetDebugInterface1',
    'DXGIDeclareAdapterRemovalSupport')

$fakeBuild = Join-Path $FakeRoot 'fake build'
$dllV1 = Join-Path $fakeBuild 'Release\ac-dlssg.dll'
$dllV2 = Join-Path $fakeBuild 'v2\ac-dlssg.dll'
New-FakePeDll $dllV1 $goodExports
New-FakePeDll $dllV2 $goodExports -Salt '-v2'

if (-not $StreamlineDir) { $StreamlineDir = Join-Path $tools '..\deps\streamline-2.14.1\bin\x64' }
$slReal = Get-NormalizedPath $StreamlineDir
$slProblems = @(foreach ($n in $script:AcdbSlDlls) {
        $p = Join-Path $slReal $n
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { "$n is missing" } else { $x = Get-NvidiaSignatureProblem $p; if ($x) { "$n`: $x" } }
    })
if ($slProblems.Count -gt 0) {
    Write-Host "test-dev-scripts: FAILED, $slReal does not hold the signed Streamline runtime ($($slProblems -join '; ')). Stage it with tools\fetch-deps.ps1."
    exit 1
}
$slRealFiles = @(Get-ChildItem -LiteralPath $slReal -File | Where-Object { $script:AcdbSlDlls -contains $_.Name -or $_.Name -like '*license*' } |
        ForEach-Object { $_.Name } | Sort-Object)
$slLicense = @($slRealFiles | Where-Object { $_ -like '*license*' })[0]

function New-SlVariant([string]$Name) {
    $dir = Join-Path $FakeRoot "streamline\$Name"
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    foreach ($f in $slRealFiles) { Copy-Item -LiteralPath (Join-Path $slReal $f) -Destination $dir }
    return $dir
}

# Drops the Authenticode certificate table of a PE32+ file: its data
# directory entry (index 4) holds a file offset, and the table ends the file.
function Remove-PeSignature([string]$Path) {
    $b = [IO.File]::ReadAllBytes($Path)
    $opt = [BitConverter]::ToInt32($b, 0x3C) + 24
    if ([BitConverter]::ToUInt16($b, $opt) -ne 0x20B) { throw "$Path is not PE32+" }
    $entry = $opt + 112 + 8 * 4
    $offset = [BitConverter]::ToInt32($b, $entry)
    if ($offset -le 0) { throw "$Path has no certificate table" }
    for ($i = 0; $i -lt 8; $i++) { $b[$entry + $i] = 0 }
    $out = New-Object byte[] $offset
    [Array]::Copy($b, $out, $offset)
    [IO.File]::WriteAllBytes($Path, $out)
}

Write-Host 'test-dev-scripts: building the Streamline variants'
# A newer build: another NVIDIA-signed DLL under one name, a changed license.
$slV2 = New-SlVariant 'v2'
Copy-Item -LiteralPath (Join-Path $slReal 'sl.reflex.dll') -Destination (Join-Path $slV2 'sl.pcl.dll') -Force
[IO.File]::AppendAllText((Join-Path $slV2 $slLicense), "changed for the upgrade test`r`n")
$slTampered = New-SlVariant 'tampered'
$tamperedDll = Join-Path $slTampered 'sl.common.dll'
$tb = [IO.File]::ReadAllBytes($tamperedDll)
$tb[0x1000] = $tb[0x1000] -bxor 0xFF
[IO.File]::WriteAllBytes($tamperedDll, $tb)
$slUnsigned = New-SlVariant 'unsigned'
Remove-PeSignature (Join-Path $slUnsigned 'sl.pcl.dll')
$slMissing = New-SlVariant 'missing'
Remove-Item -LiteralPath (Join-Path $slMissing 'sl.interposer.dll')

Write-Host 'test-dev-scripts: building the fake dxgi.dll files'
$resourceTools = Find-ResourceTools
$fakeReShade = $null
$fakeOther = $null
if ($resourceTools) {
    $fakeReShade = New-VersionResourceDll $resourceTools (Join-Path $FakeRoot 'fake reshade\dxgi.dll') 'ReShade'
    $fakeOther = New-VersionResourceDll $resourceTools (Join-Path $FakeRoot 'fake other\dxgi.dll') 'Not ReShade'
}
if (-not $fakeReShade -or -not $fakeOther) {
    Write-Host 'test-dev-scripts: FAILED, cannot build the fake dxgi.dll files (needs the Windows SDK rc.exe and MSVC link.exe)'
    exit 1
}

# A fake game folder with a fake ReShade dxgi.dll. The space in the name
# exercises quoting.
function New-FakeGame([string]$Case) {
    $game = Join-Path $FakeRoot "games\$Case\assetto corsa"
    New-Item -ItemType Directory -Path $game -Force | Out-Null
    Write-Text (Join-Path $game 'acs.exe') 'not a real executable'
    Copy-Item -LiteralPath $fakeReShade -Destination (Join-Path $game 'dxgi.dll')
    return $game
}

function Get-ManifestPath([string]$Game) { return Join-Path $Game 'ac-dlssg\install\dev-manifest.json' }
function Get-Manifest([string]$Game) { return Get-Content -Raw -LiteralPath (Get-ManifestPath $Game) | ConvertFrom-Json }

function Test-NoLeftovers([string[]]$Dirs) {
    foreach ($d in $Dirs) {
        if (@(Get-ChildItem -LiteralPath $d -Filter '*.new' -File -ErrorAction SilentlyContinue).Count -gt 0) { return $false }
    }
    return $true
}

function Test-HasBom([byte[]]$B) { return $B.Length -ge 3 -and $B[0] -eq 0xEF -and $B[1] -eq 0xBB -and $B[2] -eq 0xBF }

# A realistic ReShade.ini (CRLF, no BOM, UTF-8 and one raw non-UTF-8 byte)
# with the given [PROXY] lines and look-alike keys in another section.
function Get-RealisticIni([string]$EnableLine, [string]$ProxyLine) {
    $nonAscii = [string][char]0x00FC + [char]0x0436
    $head = "[ADDON]`r`nDisabledAddons=DLSS 5 Bridge 1.4.12@dlss5-bridge.addon64`r`n`r`n" +
    "[GENERAL]`r`n; comment $nonAscii`r`nEffectSearchPaths=.\reshade-shaders\Shaders\**`r`nNoReloadOnInit=0`r`nRaw="
    $tail = "`r`n`r`n[OTHER]`r`nProxyLibrary=must-not-change.dll`r`nEnableProxyLibrary=0`r`n`r`n" +
    "[PROXY]`r`n$EnableLine`r`n$ProxyLine`r`n`r`n[SCREENSHOT]`r`nSavePath=.\`r`n"
    return , (Join-Bytes @((Get-Utf8Bytes $head), [byte[]]@(0xE9), (Get-Utf8Bytes $tail)))
}

$simpleIni = "[PROXY]`r`nEnableProxyLibrary=0`r`nProxyLibrary=`r`n"

# ---------------------------------------------------------------------------
Invoke-Case 'A: realistic ReShade.ini, EnableProxyLibrary=0, CRLF, upgrade, full round trip' {
    $game = New-FakeGame 'A'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-RealisticIni 'EnableProxyLibrary=0' 'ProxyLibrary='
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    Check ($r.Text -match 'ReShade: .*dxgi\.dll, version 6\.8\.0\.2155') 'install reports the ReShade version it checked'
    $expected = Get-RealisticIni 'EnableProxyLibrary=1' "ProxyLibrary=$ourDll"
    Check (Test-SameBytes (Read-Bytes $ini) $expected) 'only the two [PROXY] lines changed, CRLF kept, other bytes identical'
    Check (-not (Test-HasBom (Read-Bytes $ini))) 'no BOM added'
    Check ((Test-Path -LiteralPath (Join-Path $game $ourDll)) -and ((Get-Sha (Join-Path $game $ourDll)) -eq (Get-Sha $dllV1))) 'DLL copied with the same hash'
    Check (Test-NoLeftovers @($game)) 'no .new files left'
    $manifestBytes = Read-Bytes (Get-ManifestPath $game)
    Check ($manifestBytes.Length -gt 0 -and -not (Test-HasBom $manifestBytes)) 'manifest written as UTF-8 without BOM'
    $m = Get-Manifest $game
    Check ($m.state -eq 'installed') 'manifest state is installed'
    Check ($m.reshade.keys.EnableProxyLibrary.present -eq $true -and $m.reshade.keys.EnableProxyLibrary.value -eq '0') 'manifest records EnableProxyLibrary=0'
    Check ($m.reshade.keys.ProxyLibrary.present -eq $true -and $m.reshade.keys.ProxyLibrary.value -eq '') 'manifest records empty ProxyLibrary'
    Check ($m.reshade.iniSha256Before -eq (Get-Sha256OfBytes $original)) 'manifest records the ReShade.ini hash before'
    Check ($m.reshade.iniSha256After -eq (Get-Sha $ini)) 'manifest records the ReShade.ini hash after'
    Check ($m.dll.sha256 -eq (Get-Sha $dllV1)) 'manifest records the DLL hash'
    Check ([IO.Path]::GetFullPath($m.reshade.ini) -eq [IO.Path]::GetFullPath($ini)) 'manifest records which ReShade.ini was edited'
    $cfg = Join-Path $game 'ac-dlssg\ac-dlssg.ini'
    Check ((Test-Path -LiteralPath $cfg) -and -not (Test-HasBom (Read-Bytes $cfg))) 'config written without BOM'
    $cfgText = [IO.File]::ReadAllText($cfg)
    Check ($cfgText -match '(?m)^\[bridge\]\r?$' -and $cfgText -match '(?m)^log_level=debug\r?$' -and $cfgText -match '(?m)^enabled=1\r?$' -and
        $cfgText -match '(?m)^start_with_fg=1\r?$' -and $cfgText -match '(?m)^hotkey=ctrl\+f10\r?$') 'config has [bridge] defaults and log_level=debug'
    Check ($cfgText -notmatch '(?m)^max_frame_latency=') 'config leaves max_frame_latency unset'

    $afterFirst = Read-Bytes $ini
    $r = Install $game $dllV2
    Check ($r.Code -eq 0) 're-install (upgrade) exits 0'
    Check (Test-SameBytes (Read-Bytes $ini) $afterFirst) 're-install leaves ReShade.ini unchanged'
    Check ((Get-Sha (Join-Path $game $ourDll)) -eq (Get-Sha $dllV2)) 're-install replaces the DLL'
    Check (Test-NoLeftovers @($game)) 'no .new files left after the upgrade'
    $m = Get-Manifest $game
    Check ($m.reshade.keys.EnableProxyLibrary.value -eq '0' -and $m.reshade.keys.ProxyLibrary.value -eq '') 're-install keeps the recorded original values'
    Check ($m.reshade.iniSha256Before -eq (Get-Sha256OfBytes $original)) 're-install keeps the recorded original hash'
    Check ($m.dll.sha256 -eq (Get-Sha $dllV2)) 're-install updates the recorded DLL hash'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg\install\backup'))) 'the rollback copy of the old build is gone after the upgrade'

    $log = Join-Path $game 'ac-dlssg\logs\bridge.log'
    Write-Text $log "fake log`r`n"
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check ($r.Text -match 'verified: .* does not load ac-dlssg\.dll') 'uninstall reports the re-read check'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'round trip restores a byte-identical ReShade.ini'
    Check (-not (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'uninstall deletes the DLL'
    Check (Test-Path -LiteralPath $log) 'uninstall keeps logs'
    Check (Test-Path -LiteralPath $cfg) 'uninstall keeps the config'
    Check (-not (Test-Path -LiteralPath (Get-ManifestPath $game))) 'uninstall removes the manifest'
}

# ---------------------------------------------------------------------------
function Get-SlDir([string]$Game) { return Join-Path $Game 'ac-dlssg\sl' }

# True when <game>\ac-dlssg\sl holds exactly the Streamline files of $Source.
function Test-SlMatches([string]$Game, [string]$Source) {
    $sl = Get-SlDir $Game
    if (-not (Test-Path -LiteralPath $sl -PathType Container)) { return $false }
    $names = @(Get-ChildItem -LiteralPath $sl -Force | ForEach-Object { $_.Name } | Sort-Object)
    if (($names -join '|') -ne ($slRealFiles -join '|')) { return $false }
    foreach ($n in $names) {
        if ((Get-Sha (Join-Path $sl $n)) -ne (Get-Sha (Join-Path $Source $n))) { return $false }
    }
    return $true
}

# The hash the manifest records for ac-dlssg\sl\<Name>; $null unless there is exactly one record.
function Get-SlRecordedHash($Manifest, [string]$Name) {
    $rec = @($Manifest.streamline.files | Where-Object { $_.path -eq "ac-dlssg\sl\$Name" })
    if ($rec.Count -ne 1) { return $null }
    return [string]$rec[0].sha256
}

Invoke-Case 'SL1: Streamline runtime: fresh install, upgrade, re-run, uninstall' {
    $game = New-FakeGame 'SL1'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-RealisticIni 'EnableProxyLibrary=0' 'ProxyLibrary='
    Write-Bytes $ini $original
    $sl = Get-SlDir $game
    $r = Install $game $dllV1 -Sl ''
    Check ($r.Code -eq 0) 'install with the default -StreamlineDir exits 0'
    Check ($r.Text -match '6 DLLs signed by NVIDIA Corporation') 'install reports the signature check'
    Check (Test-SlMatches $game $slReal) 'ac-dlssg\sl holds the six DLLs and the license files with the same hashes, nothing else'
    $m = Get-Manifest $game
    Check (Test-SamePath $m.streamline.source $slReal) 'manifest records deps\streamline-2.14.1\bin\x64 as the source'
    $bad = @($slRealFiles | Where-Object { (Get-SlRecordedHash $m $_) -ne (Get-Sha (Join-Path $slReal $_)) })
    Check (@($m.streamline.files).Count -eq $slRealFiles.Count -and $bad.Count -eq 0) 'manifest records every Streamline file with its hash'
    $firstSl = $r.Text.IndexOf('copied ac-dlssg\sl\')
    Check ($firstSl -ge 0 -and $firstSl -lt $r.Text.IndexOf("copied $ourDll")) 'Streamline is copied before the bridge DLL'

    $afterFirst = Read-Bytes $ini
    $r = Install $game $dllV2 -Sl $slV2
    Check ($r.Code -eq 0) 'upgrade to a newer Streamline exits 0'
    Check (Test-SlMatches $game $slV2) 'the changed Streamline files are replaced'
    Check ($r.Text -match 'sl\.common\.dll is already this version' -and $r.Text -match 'copied ac-dlssg\\sl\\sl\.pcl\.dll') 'only the changed files are copied'
    Check (Test-SameBytes (Read-Bytes $ini) $afterFirst) 'ReShade.ini unchanged by the upgrade'
    $m = Get-Manifest $game
    Check ((Get-SlRecordedHash $m 'sl.pcl.dll') -eq (Get-Sha (Join-Path $slV2 'sl.pcl.dll')) -and
        (Get-SlRecordedHash $m $slLicense) -eq (Get-Sha (Join-Path $slV2 $slLicense))) 'manifest records the new hashes'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg\install\backup'))) 'no backups left after the upgrade'
    Check (Test-NoLeftovers @($game, $sl)) 'no .new files left'
    $r = Install $game $dllV2 -Sl $slV2
    Check ($r.Code -eq 0 -and (Test-SlMatches $game $slV2) -and $r.Text -notmatch 'copied ') 'a re-run copies nothing'

    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    $verified = $r.Text.IndexOf('verified:')
    Check ($verified -ge 0 -and $r.Text.IndexOf("deleted $sl\") -gt $verified) 'the Streamline files are deleted after ReShade.ini is reverted and verified'
    Check (-not (Test-Path -LiteralPath $sl)) 'uninstall removes ac-dlssg\sl'
    Check (-not (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'uninstall deletes the bridge DLL'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'round trip restores a byte-identical ReShade.ini'
}

Invoke-Case 'SL2: Streamline refusals and files changed outside the script' {
    $game = New-FakeGame 'SL2'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes $simpleIni
    Write-Bytes $ini $original
    Check ((Get-AuthenticodeSignature -LiteralPath $tamperedDll).Status -eq 'HashMismatch') 'the tampered fixture fails its signature check'
    Check ((Get-AuthenticodeSignature -LiteralPath (Join-Path $slUnsigned 'sl.pcl.dll')).Status -eq 'NotSigned') 'the unsigned fixture has no signature'
    $r = Install $game $dllV1 -Sl $slTampered
    Check ((Test-Refused $r) -and $r.Text -match 'sl\.common\.dll: signature status HashMismatch') 'install refuses a tampered Streamline DLL'
    $r = Install $game $dllV1 -Sl $slUnsigned
    Check ((Test-Refused $r) -and $r.Text -match 'sl\.pcl\.dll: signature status NotSigned') 'install refuses an unsigned Streamline DLL'
    $r = Install $game $dllV1 -Sl $slMissing
    Check ((Test-Refused $r) -and $r.Text -match 'sl\.interposer\.dll is missing') 'install refuses a missing Streamline DLL'
    $r = Install $game $dllV1 -Sl (Join-Path $FakeRoot 'streamline\none')
    Check ((Test-Refused $r) -and $r.Text -match 'fetch-deps') 'install refuses a missing -StreamlineDir'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'ReShade.ini unchanged'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg'))) 'no data folder created'
    Check (-not (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'no DLL copied'

    Write-Bytes (Join-Path $game 'ac-dlssg\sl\sl.common.dll') (Read-Bytes (Join-Path $slReal 'sl.common.dll'))
    $r = Install $game $dllV1
    Check ((Test-Refused $r) -and $r.Text -match 'sl\.common\.dll exists but there is no') 'install refuses Streamline files without a manifest'
    Remove-Item -LiteralPath (Join-Path $game 'ac-dlssg') -Recurse -Force

    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $common = Join-Path $game 'ac-dlssg\sl\sl.common.dll'
    Copy-Item -LiteralPath $tamperedDll -Destination $common -Force
    $manifestBefore = Read-Bytes (Get-ManifestPath $game)
    $r = Install $game $dllV2 -Sl $slV2
    Check ((Test-Refused $r) -and $r.Text -match 'sl\.common\.dll was changed outside this script' -and
        $r.Text -match (Get-Sha (Join-Path $slReal 'sl.common.dll'))) 'the upgrade refuses a tampered installed DLL and names the recorded hash'
    Check ((Get-Sha $common) -eq (Get-Sha $tamperedDll)) 'the tampered file is untouched'
    Check ((Get-Sha (Join-Path $game 'ac-dlssg\sl\sl.pcl.dll')) -eq (Get-Sha (Join-Path $slReal 'sl.pcl.dll'))) 'no other Streamline file is replaced'
    Check ((Get-Sha (Join-Path $game $ourDll)) -eq (Get-Sha $dllV1)) 'the bridge DLL is not replaced'
    Check (Test-SameBytes (Read-Bytes (Get-ManifestPath $game)) $manifestBefore) 'the manifest is untouched'
    $r = Install $game $dllV2 -Sl $slV2 -Force
    Check ($r.Code -eq 0 -and (Test-SlMatches $game $slV2)) 'with -Force the upgrade replaces it'
    $backups = @(Get-ChildItem -LiteralPath (Join-Path $game 'ac-dlssg\install\backup') -File -ErrorAction SilentlyContinue)
    Check ($backups.Count -eq 1 -and (Get-Sha $backups[0].FullName) -eq (Get-Sha $tamperedDll)) 'only the tampered DLL is kept in install\backup'

    $license = Join-Path (Get-SlDir $game) $slLicense
    [IO.File]::AppendAllText($license, "edited`r`n")
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check ((Test-Path -LiteralPath $license) -and $r.Text -match "WARNING: .*$([regex]::Escape($slLicense)) is not the file") 'a Streamline file changed after the install is kept with a warning'
    Check (@(Get-ChildItem -LiteralPath (Get-SlDir $game) -Force).Count -eq 1) 'the other Streamline files are deleted'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'round trip restores a byte-identical ReShade.ini'
}

Invoke-Case 'SL3: upgrade of an M1 install (manifest without Streamline)' {
    $game = New-FakeGame 'SL3'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes $simpleIni
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $m = Get-Manifest $game
    $m.PSObject.Properties.Remove('streamline')
    [IO.File]::WriteAllText((Get-ManifestPath $game), ($m | ConvertTo-Json -Depth 8), $utf8)
    Remove-Item -LiteralPath (Get-SlDir $game) -Recurse -Force
    $r = Install $game $dllV2
    Check ($r.Code -eq 0 -and (Test-SlMatches $game $slReal)) 'the upgrade installs Streamline'
    Check (@((Get-Manifest $game).streamline.files).Count -eq $slRealFiles.Count) 'the manifest now records the Streamline files'
    $r = Uninstall $game
    Check ($r.Code -eq 0 -and -not (Test-Path -LiteralPath (Get-SlDir $game)) -and (Test-SameBytes (Read-Bytes $ini) $original)) 'uninstall removes them and restores ReShade.ini'
}

# ---------------------------------------------------------------------------
Invoke-Case 'B: no [PROXY] section, CRLF with a final newline' {
    $game = New-FakeGame 'B'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes "[GENERAL]`r`nNoReloadOnInit=0`r`n`r`n[SCREENSHOT]`r`nSavePath=.\`r`n"
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $expected = Get-Utf8Bytes ("[GENERAL]`r`nNoReloadOnInit=0`r`n`r`n[SCREENSHOT]`r`nSavePath=.\`r`n" +
        "`r`n[PROXY]`r`nEnableProxyLibrary=1`r`nProxyLibrary=$ourDll`r`n")
    Check (Test-SameBytes (Read-Bytes $ini) $expected) '[PROXY] appended with CRLF after a blank line'
    $m = Get-Manifest $game
    Check ($m.reshade.keys.EnableProxyLibrary.present -eq $false -and $m.reshade.keys.ProxyLibrary.present -eq $false) 'manifest records both keys as missing'
    $r = Install $game $dllV1
    Check ($r.Code -eq 0 -and (Test-SameBytes (Read-Bytes $ini) $expected)) 're-install adds no second [PROXY] section or key'
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'round trip restores a byte-identical ReShade.ini'
}

# ---------------------------------------------------------------------------
Invoke-Case 'C: no [PROXY] section, LF line endings, no final newline' {
    $game = New-FakeGame 'C'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes "[GENERAL]`nNoReloadOnInit=0"
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $expected = Get-Utf8Bytes "[GENERAL]`nNoReloadOnInit=0`n`n[PROXY]`nEnableProxyLibrary=1`nProxyLibrary=$ourDll"
    Check (Test-SameBytes (Read-Bytes $ini) $expected) 'LF kept, no CR added, still no final newline'
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'round trip restores a byte-identical ReShade.ini'
}

# ---------------------------------------------------------------------------
Invoke-Case 'D: [PROXY] with only "EnableProxyLibrary = 0", next section follows' {
    $game = New-FakeGame 'D'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes "[INPUT]`r`nKeyOverlay=36,0,0,0`r`n`r`n[PROXY]`r`nEnableProxyLibrary = 0`r`n`r`n[SCREENSHOT]`r`nSavePath=.\`r`n"
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $expected = Get-Utf8Bytes ("[INPUT]`r`nKeyOverlay=36,0,0,0`r`n`r`n[PROXY]`r`nEnableProxyLibrary=1`r`nProxyLibrary=$ourDll`r`n" +
        "`r`n[SCREENSHOT]`r`nSavePath=.\`r`n")
    Check (Test-SameBytes (Read-Bytes $ini) $expected) 'EnableProxyLibrary replaced in place, ProxyLibrary added inside [PROXY]'
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'round trip restores the original spacing byte for byte'
}

# ---------------------------------------------------------------------------
Invoke-Case 'E: another ProxyLibrary is chained' {
    $game = New-FakeGame 'E'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes "[PROXY]`r`nEnableProxyLibrary=1`r`nProxyLibrary=other-proxy.dll`r`n"
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check (Test-Refused $r) 'install refuses with a non-zero exit code'
    Check ($r.Text -match 'other-proxy\.dll') 'the refusal names the other DLL'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'ReShade.ini unchanged'
    Check (-not (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'no DLL copied'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg'))) 'no data folder created'

    $original = Get-Utf8Bytes "[PROXY]`r`nEnableProxyLibrary=0`r`nProxyLibrary=old.dll`r`n"
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check (Test-Refused $r) 'install refuses a non-empty foreign ProxyLibrary even when disabled'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'ReShade.ini unchanged'
}

# ---------------------------------------------------------------------------
Invoke-Case 'F1: relative [INSTALL] BasePath (wins over RESHADE_BASE_PATH_OVERRIDE)' {
    $game = New-FakeGame 'F1'
    $gameIni = Join-Path $game 'ReShade.ini'
    $gameIniBytes = Get-Utf8Bytes "[INSTALL]`r`nBasePath=reshade data`r`n`r`n[PROXY]`r`nEnableProxyLibrary=0`r`nProxyLibrary=`r`n"
    Write-Bytes $gameIni $gameIniBytes
    $baseIni = Join-Path $game 'reshade data\ReShade.ini'
    $original = Get-Utf8Bytes "[GENERAL]`r`nNoReloadOnInit=0`r`n`r`n[PROXY]`r`nEnableProxyLibrary=0`r`nProxyLibrary=`r`n"
    Write-Bytes $baseIni $original
    $decoyDir = Join-Path $FakeRoot 'games\F1\env base'
    $decoyIni = Join-Path $decoyDir 'ReShade.ini'
    $decoyBytes = Get-Utf8Bytes $simpleIni
    Write-Bytes $decoyIni $decoyBytes
    $env:RESHADE_BASE_PATH_OVERRIDE = $decoyDir
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $absDll = Join-Path $game $ourDll
    $expected = Get-Utf8Bytes "[GENERAL]`r`nNoReloadOnInit=0`r`n`r`n[PROXY]`r`nEnableProxyLibrary=1`r`nProxyLibrary=$absDll`r`n"
    Check (Test-SameBytes (Read-Bytes $baseIni) $expected) 'the BasePath ReShade.ini gets the absolute DLL path'
    Check (Test-SameBytes (Read-Bytes $gameIni) $gameIniBytes) 'the game folder ReShade.ini is unchanged'
    Check (Test-SameBytes (Read-Bytes $decoyIni) $decoyBytes) 'the RESHADE_BASE_PATH_OVERRIDE ReShade.ini is unchanged'
    Check (Test-Path -LiteralPath $absDll) 'the DLL goes into the game folder'
    Check (Test-NoLeftovers @($game, (Split-Path -Parent $baseIni))) 'no .new files left'
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check (Test-SameBytes (Read-Bytes $baseIni) $original) 'round trip restores the BasePath ReShade.ini byte for byte'
    Check (-not (Test-Path -LiteralPath $absDll)) 'uninstall deletes the DLL'
}

# ---------------------------------------------------------------------------
Invoke-Case 'F2: absolute [INSTALL] BasePath' {
    $game = New-FakeGame 'F2'
    $baseDir = Join-Path $FakeRoot 'games\F2\absolute base'
    $gameIni = Join-Path $game 'ReShade.ini'
    $gameIniBytes = Get-Utf8Bytes "[INSTALL]`r`nBasePath=$baseDir`r`n"
    Write-Bytes $gameIni $gameIniBytes
    $baseIni = Join-Path $baseDir 'ReShade.ini'
    $original = Get-Utf8Bytes $simpleIni
    Write-Bytes $baseIni $original
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $expected = Get-Utf8Bytes "[PROXY]`r`nEnableProxyLibrary=1`r`nProxyLibrary=$(Join-Path $game $ourDll)`r`n"
    Check (Test-SameBytes (Read-Bytes $baseIni) $expected) 'the BasePath ReShade.ini gets the absolute DLL path'
    Check (Test-SameBytes (Read-Bytes $gameIni) $gameIniBytes) 'the game folder ReShade.ini (no [PROXY]) is unchanged'
    $r = Uninstall $game
    Check ($r.Code -eq 0 -and (Test-SameBytes (Read-Bytes $baseIni) $original)) 'round trip restores the BasePath ReShade.ini byte for byte'
}

# ---------------------------------------------------------------------------
Invoke-Case 'F3: [INSTALL] BasePath=. is the game folder' {
    $game = New-FakeGame 'F3'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes "[INSTALL]`r`nBasePath=.`r`n`r`n$simpleIni"
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $expected = Get-Utf8Bytes "[INSTALL]`r`nBasePath=.`r`n`r`n[PROXY]`r`nEnableProxyLibrary=1`r`nProxyLibrary=$ourDll`r`n"
    Check (Test-SameBytes (Read-Bytes $ini) $expected) 'ProxyLibrary is the bare DLL name'
    $r = Uninstall $game
    Check ($r.Code -eq 0 -and (Test-SameBytes (Read-Bytes $ini) $original)) 'round trip restores a byte-identical ReShade.ini'
}

# ---------------------------------------------------------------------------
Invoke-Case 'G: RESHADE_BASE_PATH_OVERRIDE without BasePath' {
    $game = New-FakeGame 'G'
    $gameIni = Join-Path $game 'ReShade.ini'
    $gameIniBytes = Get-Utf8Bytes $simpleIni
    Write-Bytes $gameIni $gameIniBytes
    $envDir = Join-Path $FakeRoot 'games\G\env base'
    $envIni = Join-Path $envDir 'ReShade.ini'
    $original = Get-Utf8Bytes $simpleIni
    Write-Bytes $envIni $original
    $env:RESHADE_BASE_PATH_OVERRIDE = $envDir
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $expected = Get-Utf8Bytes "[PROXY]`r`nEnableProxyLibrary=1`r`nProxyLibrary=$(Join-Path $game $ourDll)`r`n"
    Check (Test-SameBytes (Read-Bytes $envIni) $expected) 'the override ReShade.ini is edited with the absolute DLL path'
    Check (Test-SameBytes (Read-Bytes $gameIni) $gameIniBytes) 'the game folder ReShade.ini is unchanged'
    # The uninstaller re-resolves the base path, so it runs with the same environment.
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check (Test-SameBytes (Read-Bytes $envIni) $original) 'round trip restores the override ReShade.ini byte for byte'
}

# ---------------------------------------------------------------------------
Invoke-Case 'H: acs.exe running (a renamed copy of timeout.exe)' {
    $game = New-FakeGame 'H'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes $simpleIni
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0 while acs.exe is not running'
    $installed = Read-Bytes $ini
    $fakeAcs = Join-Path $game 'acs.exe'
    Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\timeout.exe') -Destination $fakeAcs -Force
    $proc = Start-Process -FilePath $fakeAcs -ArgumentList '/t', '120', '/nobreak' -WindowStyle Hidden -PassThru
    try {
        $running = $null
        for ($i = 0; $i -lt 30 -and -not $running; $i++) {
            Start-Sleep -Milliseconds 100
            $running = Get-Process -Name acs -ErrorAction SilentlyContinue | Where-Object { $_.Id -eq $proc.Id }
        }
        Check ($null -ne $running -and -not $proc.HasExited) 'the fake acs.exe is running'
        $r = Install $game $dllV2
        Check (Test-Refused $r) 'install refuses while acs.exe runs'
        Check ($r.Text -match 'acs\.exe is running') 'the refusal says why'
        Check ((Get-Sha (Join-Path $game $ourDll)) -eq (Get-Sha $dllV1)) 'the DLL is not replaced'
        $r = Uninstall $game
        Check (Test-Refused $r) 'uninstall refuses while acs.exe runs'
        Check (Test-SameBytes (Read-Bytes $ini) $installed) 'ReShade.ini unchanged'
        Check (Test-Path -LiteralPath (Join-Path $game $ourDll)) 'the DLL is still there'
        Check (Test-Path -LiteralPath (Get-ManifestPath $game)) 'the manifest is still there'
    } finally {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        [void]$proc.WaitForExit(5000)
    }
    Check ($proc.HasExited) 'the fake acs.exe was stopped'
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0 once acs.exe has exited'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'round trip restores a byte-identical ReShade.ini'
}

# ---------------------------------------------------------------------------
Invoke-Case 'I: ReShade checks' {
    $game = New-FakeGame 'I1'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes $simpleIni
    Write-Bytes $ini $original
    $dxgi = Join-Path $game 'dxgi.dll'
    Copy-Item -LiteralPath $fakeOther -Destination $dxgi -Force
    $r = Install $game $dllV1
    Check ((Test-Refused $r) -and $r.Text -match 'Not ReShade') 'install refuses a dxgi.dll whose ProductName is not ReShade'
    Write-Text $dxgi 'not a PE file'
    $r = Install $game $dllV1
    Check (Test-Refused $r) 'install refuses a dxgi.dll without a version resource'
    Remove-Item -LiteralPath $dxgi
    $r = Install $game $dllV1
    Check (Test-Refused $r) 'install refuses when dxgi.dll is missing'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'ReShade.ini unchanged'
    Check (-not (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'no DLL copied'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg'))) 'no data folder created'

    $game = New-FakeGame 'I2'
    $r = Install $game $dllV1
    Check ((Test-Refused $r) -and $r.Text -match 'ReShade\.ini not found') 'install refuses when ReShade.ini is missing'
    Check (-not (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'no DLL copied'
}

# ---------------------------------------------------------------------------
Invoke-Case 'J: keys changed by the user after install' {
    $game = New-FakeGame 'J1'
    $ini = Join-Path $game 'ReShade.ini'
    Write-Bytes $ini (Get-Utf8Bytes $simpleIni)
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $changed = Get-Utf8Bytes "[PROXY]`r`nEnableProxyLibrary=1`r`nProxyLibrary=someone-else.dll`r`n"
    Write-Bytes $ini $changed
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0 when ReShade no longer loads our DLL'
    Check (Test-SameBytes (Read-Bytes $ini) $changed) 'keys that are no longer ours are left alone'
    Check ($r.Text -match 'someone-else\.dll') 'uninstall reports the foreign value'
    Check (-not (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'the DLL is deleted'

    $game = New-FakeGame 'J2'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes $simpleIni
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    Write-Bytes $ini (Get-Utf8Bytes "[PROXY]`r`nEnableProxyLibrary=0`r`nProxyLibrary=$ourDll`r`n")
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'ProxyLibrary restored, the user''s EnableProxyLibrary=0 kept'
}

# ---------------------------------------------------------------------------
Invoke-Case 'K: the original values already name our DLL' {
    $game = New-FakeGame 'K'
    $ini = Join-Path $game 'ReShade.ini'
    Write-Bytes $ini (Get-Utf8Bytes "[PROXY]`r`nEnableProxyLibrary=1`r`nProxyLibrary=$ourDll`r`n")
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $r = Uninstall $game
    Check (Test-Refused $r) 'uninstall stops because ReShade would still load our DLL'
    Check (Test-Path -LiteralPath (Join-Path $game $ourDll)) 'the DLL is kept'
    Check (Test-Path -LiteralPath (Get-ManifestPath $game)) 'the manifest is kept'
}

# ---------------------------------------------------------------------------
Invoke-Case 'L: -RemoveData' {
    $game = New-FakeGame 'L'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes $simpleIni
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    Write-Text (Join-Path $game 'ac-dlssg\logs\bridge.log') "fake log`r`n"
    $r = Uninstall $game -RemoveData
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg'))) 'the data folder is deleted'
    Check (-not (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'the DLL is deleted'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'round trip restores a byte-identical ReShade.ini'
}

# ---------------------------------------------------------------------------
Invoke-Case 'M: uninstall without a manifest' {
    $game = New-FakeGame 'M'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes "[PROXY]`r`nEnableProxyLibrary=1`r`nProxyLibrary=$ourDll`r`n"
    Write-Bytes $ini $original
    Write-Bytes (Join-Path $game $ourDll) (Read-Bytes $dllV1)
    $r = Uninstall $game
    Check (Test-Refused $r) 'uninstall refuses'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'ReShade.ini unchanged'
    Check (Test-Path -LiteralPath (Join-Path $game $ourDll)) 'the DLL is left alone'
}

# ---------------------------------------------------------------------------
Invoke-Case 'N: duplicates' {
    $game = New-FakeGame 'N1'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes "[PROXY]`r`nEnableProxyLibrary=0`r`nProxyLibrary=`r`nProxyLibrary=`r`n"
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check (Test-Refused $r) 'install refuses a duplicated key'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'ReShade.ini unchanged'
    $game = New-FakeGame 'N2'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes "[PROXY]`r`nEnableProxyLibrary=0`r`n`r`n[GENERAL]`r`nA=1`r`n`r`n[proxy]`r`nProxyLibrary=`r`n"
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check (Test-Refused $r) 'install refuses a duplicated [PROXY] section'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'ReShade.ini unchanged'
}

# ---------------------------------------------------------------------------
Invoke-Case 'O: an existing UTF-8 BOM and LF are kept' {
    $game = New-FakeGame 'O'
    $ini = Join-Path $game 'ReShade.ini'
    $bom = [byte[]]@(0xEF, 0xBB, 0xBF)
    $original = Join-Bytes @($bom, (Get-Utf8Bytes "[PROXY]`nEnableProxyLibrary=0`nProxyLibrary=`n"))
    Write-Bytes $ini $original
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $expected = Join-Bytes @($bom, (Get-Utf8Bytes "[PROXY]`nEnableProxyLibrary=1`nProxyLibrary=$ourDll`n"))
    Check (Test-SameBytes (Read-Bytes $ini) $expected) 'existing BOM and LF kept'
    $r = Uninstall $game
    Check ($r.Code -eq 0 -and (Test-SameBytes (Read-Bytes $ini) $original)) 'round trip restores a byte-identical ReShade.ini'
}

# ---------------------------------------------------------------------------
Invoke-Case 'P: an existing ac-dlssg.ini is kept' {
    $game = New-FakeGame 'P'
    Write-Bytes (Join-Path $game 'ReShade.ini') (Get-Utf8Bytes $simpleIni)
    $cfg = Join-Path $game 'ac-dlssg\ac-dlssg.ini'
    $cfgBytes = Get-Utf8Bytes "[bridge]`r`nenabled=0`r`nlog_level=info`r`n"
    Write-Bytes $cfg $cfgBytes
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    Check (Test-SameBytes (Read-Bytes $cfg) $cfgBytes) 'config not overwritten'
    $m = Get-Manifest $game
    Check ($m.config.created -eq $false) 'manifest says the config was not created by the install'
    $r = Uninstall $game -RemoveData
    Check ($r.Code -eq 0) 'uninstall exits 0'
}

# ---------------------------------------------------------------------------
Invoke-Case 'R: bad bridge DLLs' {
    $game = New-FakeGame 'R'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes $simpleIni
    Write-Bytes $ini $original
    $bad = Join-Path $FakeRoot 'games\R\text.dll'
    Write-Text $bad 'not a PE file'
    $r = Install $game $bad
    Check (Test-Refused $r) 'install refuses a file that is not a PE'
    $bad = Join-Path $FakeRoot 'games\R\noexport.dll'
    New-FakePeDll $bad @('CreateDXGIFactory', 'CreateDXGIFactory2')
    $r = Install $game $bad
    Check ((Test-Refused $r) -and $r.Text -match 'CreateDXGIFactory1') 'install refuses a DLL without CreateDXGIFactory1'
    $bad = Join-Path $FakeRoot 'games\R\reshade-export.dll'
    New-FakePeDll $bad ($goodExports + 'ReShadeRegisterAddon')
    $r = Install $game $bad
    Check ((Test-Refused $r) -and $r.Text -match 'ReShadeRegisterAddon') 'install refuses a DLL that exports ReShadeRegisterAddon'
    $bad = Join-Path $FakeRoot 'games\R\x86.dll'
    New-FakePeDll $bad $goodExports -Machine 0x14C
    $r = Install $game $bad
    Check (Test-Refused $r) 'install refuses a 32-bit DLL'
    $r = Install $game (Join-Path $FakeRoot 'games\R\missing.dll')
    Check (Test-Refused $r) 'install refuses a missing DLL'
    $bad = Join-Path $FakeRoot 'games\R\imports-version.dll'
    New-FakePeDll $bad $goodExports -Imports @('KERNEL32.dll', 'VERSION.dll')
    $r = Install $game $bad
    Check ((Test-Refused $r) -and $r.Text -match 'imports VERSION\.dll' -and $r.Text -notmatch 'imports KERNEL32') 'install refuses a DLL that imports VERSION.dll (and only names that one)'
    $bad = Join-Path $FakeRoot 'games\R\delay-d3d11.dll'
    New-FakePeDll $bad $goodExports -Imports @('kernel32.dll', 'User32.DLL') -DelayImports @('d3d11.dll')
    $r = Install $game $bad
    Check ((Test-Refused $r) -and $r.Text -match 'delay-imports d3d11\.dll' -and $r.Text -notmatch ' imports ') 'install refuses a DLL that delay-imports d3d11.dll; allowed names match case-insensitively'
    $ok = Join-Path $FakeRoot 'games\R\allowed-imports.dll'
    New-FakePeDll $ok $goodExports -Imports @('KERNEL32.dll', 'USER32.dll', 'ADVAPI32.dll', 'SHELL32.dll', 'ole32.dll')
    Check (@(Test-BridgeDll $ok).Count -eq 0) 'a DLL that imports only the allow-list passes the DLL check'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'ReShade.ini unchanged'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg'))) 'no data folder created'
}

# ---------------------------------------------------------------------------
Invoke-Case 'S: bridge DLL present without a manifest' {
    $game = New-FakeGame 'S'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes $simpleIni
    Write-Bytes $ini $original
    Write-Bytes (Join-Path $game $ourDll) (Read-Bytes $dllV2)
    $r = Install $game $dllV1
    Check (Test-Refused $r) 'install refuses'
    Check ((Get-Sha (Join-Path $game $ourDll)) -eq (Get-Sha $dllV2)) 'the existing DLL is untouched'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'ReShade.ini unchanged'
}

# ---------------------------------------------------------------------------
Invoke-Case 'V: an upgrade whose ReShade.ini write fails changes nothing' {
    $game = New-FakeGame 'V'
    $ini = Join-Path $game 'ReShade.ini'
    Write-Bytes $ini (Get-Utf8Bytes $simpleIni)
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'first install exits 0'
    # The user switched the proxy off, so the upgrade has to write ReShade.ini.
    $switchedOff = Get-Utf8Bytes "[PROXY]`r`nEnableProxyLibrary=0`r`nProxyLibrary=$ourDll`r`n"
    Write-Bytes $ini $switchedOff
    $manifestBefore = Read-Bytes (Get-ManifestPath $game)
    # Readable but not replaceable while this handle is open.
    $lock = [IO.File]::Open($ini, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $r = Install $game $dllV2 -Sl $slV2
    } finally {
        $lock.Dispose()
    }
    Check ($r.Code -ne 0 -and $r.Text -match 'FAILED:') 'the upgrade fails'
    Check ($r.Text -match 'rolled back: restored .*ac-dlssg\.dll from its backup') 'the rollback reports the restored DLL'
    Check ((Get-Sha (Join-Path $game $ourDll)) -eq (Get-Sha $dllV1)) 'the old DLL is back'
    Check (Test-SlMatches $game $slReal) 'the old Streamline files are back'
    Check (Test-SameBytes (Read-Bytes (Get-ManifestPath $game)) $manifestBefore) 'the manifest is byte-identical to before'
    Check ((Get-Manifest $game).dll.sha256 -eq (Get-Sha $dllV1)) 'the manifest still records the old DLL'
    Check (Test-SameBytes (Read-Bytes $ini) $switchedOff) 'ReShade.ini unchanged'
    Check (Test-NoLeftovers @($game, (Get-SlDir $game))) 'no .new files left'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg\install\backup'))) 'no backup folder left'
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check (-not (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'uninstall deletes the DLL (it is the recorded build)'
}

# ---------------------------------------------------------------------------
Invoke-Case 'W: a DLL changed outside the script is replaced only with -Force' {
    $game = New-FakeGame 'W'
    $ini = Join-Path $game 'ReShade.ini'
    Write-Bytes $ini (Get-Utf8Bytes $simpleIni)
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'first install exits 0'
    $target = Join-Path $game $ourDll
    Write-Bytes $target (Read-Bytes $dllV2)
    $manifestBefore = Read-Bytes (Get-ManifestPath $game)
    $r = Install $game $dllV1
    Check (Test-Refused $r) 'the upgrade refuses without -Force'
    Check ($r.Text -match (Get-Sha $dllV2) -and $r.Text -match (Get-Sha $dllV1)) 'the refusal names both hashes'
    Check ((Get-Sha $target) -eq (Get-Sha $dllV2)) 'the changed DLL is untouched'
    Check (Test-SameBytes (Read-Bytes (Get-ManifestPath $game)) $manifestBefore) 'the manifest is untouched'
    $r = Install $game $dllV1 -Force
    Check ($r.Code -eq 0) 'the upgrade with -Force exits 0'
    Check ((Get-Sha $target) -eq (Get-Sha $dllV1)) 'the DLL is replaced'
    $backups = @(Get-ChildItem -LiteralPath (Join-Path $game 'ac-dlssg\install\backup') -File -ErrorAction SilentlyContinue)
    Check ($backups.Count -eq 1 -and (Get-Sha $backups[0].FullName) -eq (Get-Sha $dllV2)) 'the replaced DLL is kept in install\backup'
    $r = Uninstall $game -RemoveData
    Check ($r.Code -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg'))) 'uninstall -RemoveData removes the backup too'
}

# ---------------------------------------------------------------------------
Invoke-Case 'T: game folder checks' {
    $game = Join-Path $FakeRoot 'games\T\not a game'
    New-Item -ItemType Directory -Path $game -Force | Out-Null
    $r = Install $game $dllV1
    Check (Test-Refused $r) 'install refuses a folder without acs.exe'
    $r = Uninstall $game
    Check (Test-Refused $r) 'uninstall refuses a folder without acs.exe'
    $r = Install (Join-Path $FakeRoot 'games\T\missing') $dllV1
    Check (Test-Refused $r) 'install refuses a folder that does not exist'
}

# ---------------------------------------------------------------------------
Invoke-Case 'U: default -Dll is build\Release\ac-dlssg.dll' {
    $game = New-FakeGame 'U'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes $simpleIni
    Write-Bytes $ini $original
    $release = Get-NormalizedPath (Join-Path $tools "..\build\Release\$ourDll")
    $r = Install $game ''
    if (Test-Path -LiteralPath $release -PathType Leaf) {
        Check ($r.Code -eq 0) 'install without -Dll exits 0'
        $m = Get-Manifest $game
        Check ((Test-SamePath $m.dll.source $release)) 'the source is build\Release\ac-dlssg.dll'
        Check ((Get-Sha (Join-Path $game $ourDll)) -eq $m.dll.sha256) 'the copied DLL matches the recorded hash'
        $r = Uninstall $game -RemoveData
        Check ($r.Code -eq 0 -and (Test-SameBytes (Read-Bytes $ini) $original)) 'round trip restores a byte-identical ReShade.ini'
    } else {
        Check ((Test-Refused $r) -and $r.Text -match [regex]::Escape($ourDll)) 'install without -Dll refuses when there is no Release build'
    }
}

# ---------------------------------------------------------------------------
Invoke-Case 'Q: helpers (Steam library discovery, PE check on real builds)' {
    $steam = Join-Path $FakeRoot 'steam\steam'
    $lib0 = Join-Path $FakeRoot 'steam\lib0'
    $lib1 = Join-Path $FakeRoot 'steam\lib 1'
    New-Item -ItemType Directory -Path (Join-Path $steam 'steamapps') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $lib0 'steamapps\common\assettocorsa') -Force | Out-Null
    $acDir = Join-Path $lib1 'steamapps\common\assettocorsa'
    Write-Text (Join-Path $acDir 'acs.exe') 'fake'
    $vdf = "`"libraryfolders`"`n{`n`t`"0`"`n`t{`n`t`t`"path`"`t`t`"$($lib0.Replace('\', '\\'))`"`n`t`t`"apps`"`n`t`t{`n`t`t`t`"244210`"`t`t`"0`"`n`t`t}`n`t}`n" +
    "`t`"1`"`n`t{`n`t`t`"path`"`t`t`"$($lib1.Replace('\', '\\'))`"`n`t`t`"label`"`t`t`"`"`n`t`t`"apps`"`n`t`t{`n`t`t`t`"244210`"`t`t`"123`"`n`t`t}`n`t}`n}`n"
    Write-Text (Join-Path $steam 'steamapps\libraryfolders.vdf') $vdf
    $found = Find-AssettoCorsaDir -SteamPath $steam
    Check ($found -and ([IO.Path]::GetFullPath($found) -eq [IO.Path]::GetFullPath($acDir))) 'libraryfolders.vdf (current format): skips a library without acs.exe, finds the second'
    $vdfOld = "`"LibraryFolders`"`n{`n`t`"TimeNextStatsReport`"`t`t`"1`"`n`t`"1`"`t`t`"$($lib1.Replace('\', '\\'))`"`n}`n"
    Write-Text (Join-Path $steam 'steamapps\libraryfolders.vdf') $vdfOld
    $found = Find-AssettoCorsaDir -SteamPath $steam
    Check ($found -and ([IO.Path]::GetFullPath($found) -eq [IO.Path]::GetFullPath($acDir))) 'libraryfolders.vdf (old format)'
    Remove-Item -LiteralPath (Join-Path $acDir 'acs.exe')
    Check (-not (Find-AssettoCorsaDir -SteamPath $steam)) 'nothing found when no library has acs.exe'

    foreach ($config in @('Release', 'Debug')) {
        $real = Join-Path $tools "..\build\$config\ac-dlssg.dll"
        if (Test-Path -LiteralPath $real -PathType Leaf) {
            $problems = @(Test-BridgeDll $real)
            Check ($problems.Count -eq 0) "the real $config build passes the DLL check ($($problems -join '; '))"
        } else {
            Write-Host "    skip  no $config build of ac-dlssg.dll"
        }
    }
}

# ---------------------------------------------------------------------------
# collect-sysinfo.ps1 runs dxdiag and writes its report next to itself, so it
# is checked as text, never run.
Invoke-Case 'R: collect-sysinfo.ps1 (HAGS section, read-only)' {
    $path = Join-Path $tools 'collect-sysinfo.ps1'
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
    Check (@($errors).Count -eq 0) 'collect-sysinfo.ps1 parses'
    $text = [IO.File]::ReadAllText($path)
    $m = [regex]::Match($text, "(?s)Section 'Hardware-accelerated GPU scheduling \(registry\)'(.*?)\r?\nSection ")
    Check $m.Success 'the HAGS section exists'
    $section = if ($m.Success) { $m.Groups[1].Value } else { '' }
    Check ($section -match 'can be absent while HAGS is on') 'it says HwSchMode can be absent while HAGS is on'
    Check ($section -match 'Hardware Scheduling' -and $section -match 'DirectX diagnostic') `
        'it points at the per-adapter ''Hardware Scheduling'' lines in the DirectX diagnostic section'
    Check ($section -notmatch '\(off, or not offered') 'a missing HwSchMode is not reported as off'
    $writers = @('Set-ItemProperty', 'New-ItemProperty', 'Remove-ItemProperty', 'Rename-ItemProperty',
        'Clear-ItemProperty', 'Set-Item', 'Remove-Item', 'Set-Content', 'Add-Content', 'reg', 'reg.exe', 'bcdedit',
        'Set-Service', 'Stop-Service', 'Stop-Process')
    $commands = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] }, $true) |
        ForEach-Object { $_.GetCommandName() } | Where-Object { $_ })
    $bad = @($commands | Where-Object { $writers -contains $_ } | Sort-Object -Unique)
    Check ($bad.Count -eq 0) "no command that changes the system ($($bad -join ', '))"
}

# ---------------------------------------------------------------------------
Write-Host ''
if (@(Get-Process -Name acs -ErrorAction SilentlyContinue).Count -gt 0) {
    $script:failed++
    $script:failedNames.Add('end: an acs.exe process is still running')
}
if ($script:failed -gt 0) {
    Write-Host "test-dev-scripts: FAILED, $($script:failed) of $($script:passed + $script:failed) checks"
    foreach ($n in $script:failedNames) { Write-Host "  $n" }
    Write-Host "  the fake folders are kept in $FakeRoot"
    exit 1
}
if ($Keep) {
    Write-Host "test-dev-scripts: fake folders kept in $FakeRoot (-Keep)"
} else {
    # TEMP points inside the folder; move it out before deleting.
    $env:TEMP = Split-Path -Parent $FakeRoot
    $env:TMP = $env:TEMP
    if (-not (Remove-FakeRoot)) { exit 1 }
}
Write-Host "test-dev-scripts: OK, $($script:passed) checks passed"
exit 0
