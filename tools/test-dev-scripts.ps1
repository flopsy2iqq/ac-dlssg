<#
.SYNOPSIS
  Tests dev-install.ps1 and dev-uninstall.ps1 against throw-away fake game folders.

.DESCRIPTION
  Every case builds its own fake Assetto Corsa folder under -FakeRoot (a dummy
  acs.exe, a fake ReShade dxgi.dll, ReShade.ini fixtures and a hand-made x64 PE
  file that stands in for ac-dlssg.dll), runs the scripts in a child Windows
  PowerShell and checks the resulting bytes. The standalone cases (SA*) use
  fake game folders without a dxgi.dll, or with a foreign one, and one of
  them denies the current user writing into the fake game folder to check
  the elevated-PowerShell hint.

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

  The SP cases run the test package's install.ps1 and uninstall.ps1 with the
  dlssg_for_sm86 step (spec 10). They never download: the spoof files are
  local fixtures under -FakeRoot (-SpoofSourceDir) with their own pins
  (-SpoofPins), and the GPU is injected with -GpuDeviceIds, because the PC
  that runs the tests may well have an RTX 30. The runner refuses to start
  the package install.ps1 without -NoSpoof or -SpoofSourceDir.
  LOCALAPPDATA also points into -FakeRoot, so that the uninstaller's
  %LOCALAPPDATA%\DlssgSm86 is a fake one too.

  -Case runs only the named cases (their ids, such as SA1 or LA2; wildcards
  allowed); the fixtures are built as always.

  Exits 0 when every check passes, 1 otherwise.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\test-dev-scripts.ps1
#>
param(
    [string]$FakeRoot,
    [string]$StreamlineDir,
    [string[]]$Case = @(),
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
# "powershell -File" passes "-Case A,B" as one string.
$Case = @($Case | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

$tools = $PSScriptRoot
$installScript = Join-Path $tools 'dev-install.ps1'
$uninstallScript = Join-Path $tools 'dev-uninstall.ps1'
$commonScript = Join-Path $tools 'dev-common.ps1'
$packageScript = Join-Path $tools 'make-test-package.ps1'

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
# uninstall.ps1 -RemoveData deletes %LOCALAPPDATA%\DlssgSm86; the children
# get a fake LOCALAPPDATA so that the real one is never touched.
$fakeLocalAppData = Join-Path $FakeRoot 'localappdata'
New-Item -ItemType Directory -Path $fakeLocalAppData | Out-Null
$env:LOCALAPPDATA = $fakeLocalAppData

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
    # -Case: only the cases whose id (the text before the colon) is listed.
    if ($Case.Count -and -not ($Case | Where-Object { $Name -like "$($_):*" })) { return }
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

# Safety net for the package scripts: install.ps1 (and install.bat) download
# dlssg_for_sm86 on an RTX 30, and install and uninstall start again with
# administrator rights (a UAC prompt) when the game folder is not writable.
function Assert-PackageArguments([string]$Script, [string[]]$Arguments) {
    $leaf = Split-Path -Leaf $Script
    if (@('install.ps1', 'install.bat') -contains $leaf -and $Arguments -notcontains '-NoSpoof' -and $Arguments -notcontains '-SpoofSourceDir') {
        throw "refusing to run $Script without -NoSpoof or -SpoofSourceDir; it could download dlssg_for_sm86"
    }
    if (@('install.ps1', 'install.bat', 'uninstall.ps1', 'uninstall.bat') -contains $leaf -and $Arguments -notcontains '-NoElevate') {
        throw "refusing to run $Script without -NoElevate; it could ask Windows for administrator rights"
    }
}

function Invoke-Tool([string]$Script, [string[]]$Arguments) {
    # Safety net: without -GameDir the scripts would find the real game.
    $i = [array]::IndexOf($Arguments, '-GameDir')
    if ($i -lt 0 -or $i + 1 -ge $Arguments.Count -or -not (Test-UnderFakeRoot $Arguments[$i + 1])) {
        throw "refusing to run $Script without a -GameDir under $FakeRoot"
    }
    Assert-PackageArguments $Script $Arguments
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

# -Sl '' leaves -StreamlineDir to the script's default; -Mode '' leaves -Mode
# to the script's default (Auto); -Lua '' leaves -LuaApp to the script's
# default (the repository's apps\lua\AcDlssg).
function Install([string]$Game, [string]$Dll, [switch]$Force, [string]$Sl = $slReal, [string]$Mode = '',
    [string]$Lua = '') {
    $a = @('-GameDir', $Game)
    if ($Dll) { $a += @('-Dll', $Dll) }
    if ($Sl) { $a += @('-StreamlineDir', $Sl) }
    if ($Mode) { $a += @('-Mode', $Mode) }
    if ($Lua) { $a += @('-LuaApp', $Lua) }
    if ($Force) { $a += '-Force' }
    return Invoke-Tool $installScript $a
}

function Uninstall([string]$Game, [switch]$RemoveData, [switch]$Force) {
    $a = @('-GameDir', $Game)
    if ($RemoveData) { $a += '-RemoveData' }
    if ($Force) { $a += '-Force' }
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

# A fake game folder with a fake ReShade dxgi.dll, or with no dxgi.dll at all
# (-NoDxgi, a game without ReShade). The space in the name exercises quoting.
function New-FakeGame([string]$Case, [switch]$NoDxgi) {
    $game = Join-Path $FakeRoot "games\$Case\assetto corsa"
    New-Item -ItemType Directory -Path $game -Force | Out-Null
    Write-Text (Join-Path $game 'acs.exe') 'not a real executable'
    if (-not $NoDxgi) { Copy-Item -LiteralPath $fakeReShade -Destination (Join-Path $game 'dxgi.dll') }
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
    Check ($r.Text -match 'mode: reshade \(auto') 'the default -Mode Auto picks ReShade mode for a ReShade dxgi.dll'
    Check ((Get-Manifest $game).mode -eq 'reshade') 'manifest records mode reshade'
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
    $r = Install $game $dllV1 -Mode ReShade
    Check (Test-Refused $r) 'install -Mode ReShade refuses when dxgi.dll is missing'
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
# Standalone mode: the bridge installed as <game>\dxgi.dll, without ReShade.

function Get-GameDxgi([string]$Game) { return Join-Path $Game 'dxgi.dll' }

Invoke-Case 'SA1: standalone (auto, no dxgi.dll): install, re-run, upgrade, uninstall' {
    $game = New-FakeGame 'SA1' -NoDxgi
    $dxgi = Get-GameDxgi $game
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    Check ($r.Text -match 'mode: standalone \(auto: no dxgi\.dll in the game folder\)') 'Auto picks standalone mode and says why'
    Check ((Test-Path -LiteralPath $dxgi) -and (Get-Sha $dxgi) -eq (Get-Sha $dllV1)) 'the bridge is copied to <game>\dxgi.dll with the same hash'
    Check (-not (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'no ac-dlssg.dll in the game folder'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ReShade.ini'))) 'no ReShade.ini written'
    Check (Test-SlMatches $game $slReal) 'ac-dlssg\sl holds the Streamline files'
    Check (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg\ac-dlssg.ini')) 'ac-dlssg.ini written'
    Check (Test-NoLeftovers @($game, (Get-SlDir $game))) 'no .new files left'
    $m = Get-Manifest $game
    Check ($m.mode -eq 'standalone' -and $m.state -eq 'installed') 'manifest records mode standalone, state installed'
    Check ($m.dll.path -eq 'dxgi.dll' -and $m.dll.sha256 -eq (Get-Sha $dllV1)) 'manifest records dxgi.dll with its hash'
    Check ($null -eq $m.reshade) 'manifest has no ReShade record'
    $slAt = $r.Text.IndexOf('copied ac-dlssg\sl\')
    Check ($slAt -ge 0 -and $slAt -lt $r.Text.IndexOf('copied dxgi.dll')) 'Streamline is copied before dxgi.dll'

    $r = Install $game $dllV1
    Check ($r.Code -eq 0 -and $r.Text -match 'dxgi\.dll is already this version') 're-run exits 0 and keeps dxgi.dll'
    $r = Install $game $dllV2 -Sl $slV2 -Mode Standalone
    Check ($r.Code -eq 0) 'upgrade with -Mode Standalone exits 0'
    Check ((Get-Sha $dxgi) -eq (Get-Sha $dllV2)) 'upgrade replaces dxgi.dll'
    Check (Test-SlMatches $game $slV2) 'upgrade replaces the Streamline files'
    Check ((Get-Manifest $game).dll.sha256 -eq (Get-Sha $dllV2)) 'manifest records the new hash'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg\install\backup'))) 'no backup left after the upgrade'
    Check (Test-NoLeftovers @($game, (Get-SlDir $game))) 'no .new files left after the upgrade'

    $fakeAcs = Join-Path $game 'acs.exe'
    Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\timeout.exe') -Destination $fakeAcs -Force
    $proc = Start-Process -FilePath $fakeAcs -ArgumentList '/t', '120', '/nobreak' -WindowStyle Hidden -PassThru
    try {
        for ($i = 0; $i -lt 30 -and -not (Get-Process -Name acs -ErrorAction SilentlyContinue | Where-Object { $_.Id -eq $proc.Id }); $i++) {
            Start-Sleep -Milliseconds 100
        }
        $r = Install $game $dllV1
        Check ((Test-Refused $r) -and $r.Text -match 'acs\.exe is running') 'install refuses while acs.exe runs'
        $r = Uninstall $game
        Check ((Test-Refused $r) -and $r.Text -match 'acs\.exe is running') 'uninstall refuses while acs.exe runs'
        Check ((Get-Sha $dxgi) -eq (Get-Sha $dllV2)) 'dxgi.dll untouched while acs.exe runs'
    } finally {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        [void]$proc.WaitForExit(5000)
    }

    $log = Join-Path $game 'ac-dlssg\logs\bridge.log'
    Write-Text $log "fake log`r`n"
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check (-not (Test-Path -LiteralPath $dxgi)) 'uninstall deletes dxgi.dll'
    Check (-not (Test-Path -LiteralPath (Get-SlDir $game))) 'uninstall deletes ac-dlssg\sl'
    Check (-not (Test-Path -LiteralPath (Get-ManifestPath $game))) 'uninstall removes the manifest'
    Check ((Test-Path -LiteralPath $log) -and (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg\ac-dlssg.ini'))) 'uninstall keeps logs and the config'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ReShade.ini'))) 'no ReShade.ini appeared'
}

Invoke-Case 'SA2: -Mode against what the game folder holds' {
    $game = New-FakeGame 'SA2a'
    $ini = Join-Path $game 'ReShade.ini'
    Write-Bytes $ini (Get-Utf8Bytes $simpleIni)
    $r = Install $game $dllV1 -Mode Standalone
    Check ((Test-Refused $r) -and $r.Text -match 'is ReShade') 'install -Mode Standalone refuses to replace ReShade''s dxgi.dll'
    Check ((Get-Sha (Get-GameDxgi $game)) -eq (Get-Sha $fakeReShade)) 'ReShade''s dxgi.dll untouched'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg'))) 'no data folder created'
    Check (Test-SameBytes (Read-Bytes $ini) (Get-Utf8Bytes $simpleIni)) 'ReShade.ini unchanged'

    $game = New-FakeGame 'SA2b' -NoDxgi
    $r = Install $game $dllV1 -Mode ReShade
    Check ((Test-Refused $r) -and $r.Text -match 'ReShade is not installed') 'install -Mode ReShade refuses without ReShade'
    Check (-not (Test-Path -LiteralPath (Get-GameDxgi $game))) 'no dxgi.dll created'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg'))) 'no data folder created'
}

Invoke-Case 'SA3: a foreign dxgi.dll is refused' {
    $game = New-FakeGame 'SA3' -NoDxgi
    $dxgi = Get-GameDxgi $game
    Copy-Item -LiteralPath $fakeOther -Destination $dxgi
    foreach ($mode in @('', 'Standalone')) {
        $r = Install $game $dllV1 -Mode $mode
        Check ((Test-Refused $r) -and $r.Text -match 'Not ReShade' -and $r.Text -match 'not installed by') "install$(if ($mode) { " -Mode $mode" }) refuses a foreign dxgi.dll and names it"
    }
    $r = Install $game $dllV1 -Force
    Check (Test-Refused $r) '-Force does not replace a foreign dxgi.dll'
    Check ((Get-Sha $dxgi) -eq (Get-Sha $fakeOther)) 'the foreign dxgi.dll is untouched'
    # A bridge copied by hand, without a manifest, is foreign too.
    Copy-Item -LiteralPath $dllV1 -Destination $dxgi -Force
    $r = Install $game $dllV1
    Check (Test-Refused $r) 'install refuses a dxgi.dll that no manifest records'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg'))) 'no data folder created'
}

Invoke-Case 'SA4: a standalone dxgi.dll changed outside the script' {
    $game = New-FakeGame 'SA4' -NoDxgi
    $dxgi = Get-GameDxgi $game
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    Write-Bytes $dxgi (Read-Bytes $dllV2)
    $manifestBefore = Read-Bytes (Get-ManifestPath $game)
    $r = Install $game $dllV1
    Check ((Test-Refused $r) -and $r.Text -match (Get-Sha $dllV2) -and $r.Text -match (Get-Sha $dllV1)) 'upgrade refuses and names both hashes'
    Check ((Get-Sha $dxgi) -eq (Get-Sha $dllV2)) 'the changed dxgi.dll is untouched'
    Check (Test-SameBytes (Read-Bytes (Get-ManifestPath $game)) $manifestBefore) 'the manifest is untouched'
    $r = Uninstall $game
    Check ((Test-Refused $r) -and $r.Text -match (Get-Sha $dllV2)) 'uninstall refuses and names the hash'
    Check ((Get-Sha $dxgi) -eq (Get-Sha $dllV2)) 'dxgi.dll is still there'
    Check ((Test-Path -LiteralPath (Get-ManifestPath $game)) -and (Test-SlMatches $game $slReal)) 'the manifest and the Streamline files are still there'
    $r = Uninstall $game -Force
    Check ($r.Code -eq 0) 'uninstall -Force exits 0'
    Check ((Get-Sha $dxgi) -eq (Get-Sha $dllV2)) 'uninstall -Force leaves the foreign dxgi.dll in place'
    Check ($r.Text -match 'left in place') 'uninstall -Force says so'
    Check (-not (Test-Path -LiteralPath (Get-SlDir $game)) -and -not (Test-Path -LiteralPath (Get-ManifestPath $game))) 'uninstall -Force removes the rest'

    # ReShade installed over the standalone bridge: the mode no longer fits.
    $game = New-FakeGame 'SA4b' -NoDxgi
    $dxgi = Get-GameDxgi $game
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'second install exits 0'
    Copy-Item -LiteralPath $fakeReShade -Destination $dxgi -Force
    Write-Bytes (Join-Path $game 'ReShade.ini') (Get-Utf8Bytes $simpleIni)
    $r = Install $game $dllV1
    Check ((Test-Refused $r) -and $r.Text -match 'dev-uninstall\.ps1') 'install refuses to switch from standalone to ReShade mode'
    Check ((Get-Sha $dxgi) -eq (Get-Sha $fakeReShade)) 'ReShade''s dxgi.dll untouched'
    $r = Uninstall $game
    Check ((Test-Refused $r) -and $r.Text -match 'ReShade') 'uninstall refuses to delete ReShade''s dxgi.dll'
    $r = Uninstall $game -Force -RemoveData
    Check ($r.Code -eq 0 -and (Get-Sha $dxgi) -eq (Get-Sha $fakeReShade)) 'uninstall -Force keeps ReShade''s dxgi.dll'
}

Invoke-Case 'SA5: a ReShade-mode install must be undone before a standalone one' {
    $game = New-FakeGame 'SA5'
    $ini = Join-Path $game 'ReShade.ini'
    Write-Bytes $ini (Get-Utf8Bytes $simpleIni)
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'ReShade-mode install exits 0'
    Remove-Item -LiteralPath (Get-GameDxgi $game)
    $r = Install $game $dllV1
    Check ((Test-Refused $r) -and $r.Text -match 'reshade mode' -and $r.Text -match 'dev-uninstall\.ps1') 'install refuses to switch from ReShade to standalone mode'
    Check (-not (Test-Path -LiteralPath (Get-GameDxgi $game))) 'no dxgi.dll created'
    $r = Uninstall $game
    Check ($r.Code -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'the ReShade-mode uninstall still works'
    $r = Install $game $dllV1
    Check ($r.Code -eq 0 -and (Get-Sha (Get-GameDxgi $game)) -eq (Get-Sha $dllV1)) 'then a standalone install works'
    $r = Uninstall $game -RemoveData
    Check ($r.Code -eq 0 -and -not (Test-Path -LiteralPath (Get-GameDxgi $game))) 'and its uninstall'
}

Invoke-Case 'SA6: access denied in the game folder asks for an elevated PowerShell' {
    $game = New-FakeGame 'SA6' -NoDxgi
    $sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User
    $rights = [System.Security.AccessControl.FileSystemRights]'CreateFiles, CreateDirectories'
    $rule = New-Object System.Security.AccessControl.FileSystemAccessRule($sid, $rights, 'None', 'None', 'Deny')
    $acl = Get-Acl -LiteralPath $game
    $acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $game -AclObject $acl
    try {
        $r = Install $game $dllV1
        Check ($r.Code -ne 0 -and $r.Text -match 'elevated PowerShell') 'install fails and asks for an elevated PowerShell'
        Check (-not (Test-Path -LiteralPath (Get-GameDxgi $game))) 'no dxgi.dll created'
        Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg'))) 'no data folder created'
    } finally {
        $acl = Get-Acl -LiteralPath $game
        [void]$acl.RemoveAccessRule($rule)
        Set-Acl -LiteralPath $game -AclObject $acl
    }
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0 once the folder is writable'
    $r = Uninstall $game -RemoveData
    Check ($r.Code -eq 0) 'uninstall exits 0'
}

# ---------------------------------------------------------------------------
# M3: the CSP Lua app that publishes the camera, installed as
# <game>\apps\lua\AcDlssg (spec 6.6, 12), and the M3 keys of ac-dlssg.ini.

$luaReal = Get-NormalizedPath (Join-Path $tools '..\apps\lua\AcDlssg')
$luaRealFiles = @(Get-ChildItem -LiteralPath $luaReal -File -Recurse | ForEach-Object { $_.FullName.Substring($luaReal.Length + 1) } |
        Sort-Object)

# A copy of the app under -FakeRoot, optionally with a changed AcDlssg.lua (a newer build).
function New-LuaVariant([string]$Name, [switch]$Changed) {
    $dir = Join-Path $FakeRoot "lua\$Name\AcDlssg"
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    Copy-Item -Path (Join-Path $luaReal '*') -Destination $dir -Recurse -Force
    if ($Changed) { [IO.File]::AppendAllText((Join-Path $dir 'AcDlssg.lua'), "-- changed for the upgrade test`r`n") }
    return $dir
}

function Get-LuaDir([string]$Game) { return Join-Path $Game 'apps\lua\AcDlssg' }

# True when <game>\apps\lua\AcDlssg holds exactly the files of $Source with the same hashes.
function Test-LuaMatches([string]$Game, [string]$Source) {
    $dir = Get-LuaDir $Game
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { return $false }
    $names = @(Get-ChildItem -LiteralPath $dir -File -Recurse -Force | ForEach-Object { $_.FullName.Substring($dir.Length + 1) } | Sort-Object)
    if (($names -join '|') -ne ($luaRealFiles -join '|')) { return $false }
    foreach ($n in $names) {
        if ((Get-Sha (Join-Path $dir $n)) -ne (Get-Sha (Join-Path $Source $n))) { return $false }
    }
    return $true
}

function Get-LuaRecordedHash($Manifest, [string]$Name) {
    $rec = @($Manifest.luaApp.files | Where-Object { $_.path -eq "apps\lua\AcDlssg\$Name" })
    if ($rec.Count -ne 1) { return $null }
    return [string]$rec[0].sha256
}

$luaV2 = New-LuaVariant 'v2' -Changed

Invoke-Case 'LA1: Lua app: fresh install, re-run, upgrade, uninstall' {
    $game = New-FakeGame 'LA1' -NoDxgi
    $lua = Get-LuaDir $game
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install with the default -LuaApp exits 0'
    Check (Test-LuaMatches $game $luaReal) 'apps\lua\AcDlssg holds the repository app''s files with the same hashes'
    $m = Get-Manifest $game
    Check (Test-SamePath $m.luaApp.source $luaReal) 'manifest records apps\lua\AcDlssg of the repository as the source'
    $bad = @($luaRealFiles | Where-Object { (Get-LuaRecordedHash $m $_) -ne (Get-Sha (Join-Path $luaReal $_)) })
    Check (@($m.luaApp.files).Count -eq $luaRealFiles.Count -and $bad.Count -eq 0) 'manifest records every app file with its hash'
    Check ((@($m.luaApp.createdDirs) -join '|') -eq 'apps\lua\AcDlssg|apps\lua|apps') 'manifest records the folders the install created, deepest first'
    $slAt = $r.Text.LastIndexOf('copied ac-dlssg\sl\')
    $luaAt = $r.Text.IndexOf('copied apps\lua\AcDlssg\')
    Check ($slAt -ge 0 -and $luaAt -gt $slAt -and $luaAt -lt $r.Text.IndexOf('copied dxgi.dll')) 'the app is copied after Streamline and before the bridge (spec 12 order)'
    Check (Test-NoLeftovers @($lua)) 'no .new files left in the app folder'

    $r = Install $game $dllV1
    Check ($r.Code -eq 0 -and $r.Text -notmatch 'copied apps' -and $r.Text -match 'AcDlssg\.lua is already this version') 'a re-run copies no app file'
    $r = Install $game $dllV2 -Lua $luaV2
    Check ($r.Code -eq 0) 'upgrade to a changed app exits 0'
    Check (Test-LuaMatches $game $luaV2) 'the changed app file is replaced'
    Check ($r.Text -match 'copied apps\\lua\\AcDlssg\\AcDlssg\.lua' -and $r.Text -match 'manifest\.ini is already this version') 'only the changed app file is copied'
    Check ((Get-LuaRecordedHash (Get-Manifest $game) 'AcDlssg.lua') -eq (Get-Sha (Join-Path $luaV2 'AcDlssg.lua'))) 'manifest records the new hash'
    Check ((@((Get-Manifest $game).luaApp.createdDirs) -join '|') -eq 'apps\lua\AcDlssg|apps\lua|apps') 'the upgrade keeps the first install''s folder record'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg\install\backup'))) 'no backup left after the upgrade'

    Write-Text (Join-Path $game 'ac-dlssg\logs\bridge.log') "fake log`r`n"
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'apps'))) 'uninstall removes apps\lua\AcDlssg and the folders the install created'
    Check ($r.Text -match "deleted $([regex]::Escape($lua))") 'uninstall reports the app files it deleted'
    Check (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg\logs\bridge.log')) 'uninstall keeps the logs'
}

Invoke-Case 'LA2: a CSP apps\lua folder that exists stays, and another app is untouched' {
    $game = New-FakeGame 'LA2'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-Utf8Bytes $simpleIni
    Write-Bytes $ini $original
    $other = Join-Path $game 'apps\lua\OtherApp\manifest.ini'
    Write-Text $other "[ABOUT]`r`nNAME = Other`r`n"
    $r = Install $game $dllV1
    Check ($r.Code -eq 0 -and (Test-LuaMatches $game $luaReal)) 'install exits 0 and installs the app next to another one'
    Check ((@((Get-Manifest $game).luaApp.createdDirs) -join '|') -eq 'apps\lua\AcDlssg') 'only apps\lua\AcDlssg is recorded as created'
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check (-not (Test-Path -LiteralPath (Get-LuaDir $game))) 'uninstall removes apps\lua\AcDlssg'
    Check ((Test-Path -LiteralPath $other) -and (Test-Path -LiteralPath (Join-Path $game 'apps\lua'))) 'the other app and apps\lua stay'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'round trip restores a byte-identical ReShade.ini'
}

Invoke-Case 'LA3: a foreign apps\lua\AcDlssg folder is refused' {
    $game = New-FakeGame 'LA3' -NoDxgi
    $foreign = Join-Path (Get-LuaDir $game) 'AcDlssg.lua'
    Write-Text $foreign "-- someone else's app`r`n"
    foreach ($force in @($false, $true)) {
        $r = if ($force) { Install $game $dllV1 -Force } else { Install $game $dllV1 }
        Check ((Test-Refused $r) -and $r.Text -match 'apps\\lua\\AcDlssg' -and $r.Text -match 'not installed by') "install$(if ($force) { ' -Force' }) refuses a foreign apps\lua\AcDlssg and names it"
    }
    Check ((Get-Sha $foreign) -eq (Get-Sha256OfBytes (Get-Utf8Bytes "-- someone else's app`r`n"))) 'the foreign app is untouched'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg')) -and -not (Test-Path -LiteralPath (Get-GameDxgi $game))) 'nothing else was installed'

    # An M2 install (no luaApp in the manifest) is upgraded: the app is added,
    # unless a foreign folder is in the way.
    $game = New-FakeGame 'LA3b' -NoDxgi
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $m = Get-Manifest $game
    $m.PSObject.Properties.Remove('luaApp')
    [IO.File]::WriteAllText((Get-ManifestPath $game), ($m | ConvertTo-Json -Depth 8), $utf8)
    Remove-Item -LiteralPath (Join-Path $game 'apps') -Recurse -Force
    Write-Text (Join-Path (Get-LuaDir $game) 'manifest.ini') "foreign`r`n"
    $manifestBefore = Read-Bytes (Get-ManifestPath $game)
    $r = Install $game $dllV2
    Check ((Test-Refused $r) -and $r.Text -match 'apps\\lua\\AcDlssg') 'an upgrade of an M2 install refuses a foreign app folder'
    Check (Test-SameBytes (Read-Bytes (Get-ManifestPath $game)) $manifestBefore) 'the manifest is untouched'
    Remove-Item -LiteralPath (Join-Path $game 'apps') -Recurse -Force
    $r = Install $game $dllV2
    Check ($r.Code -eq 0 -and (Test-LuaMatches $game $luaReal)) 'without it the upgrade of an M2 install adds the app'
    $r = Uninstall $game -RemoveData
    Check ($r.Code -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $game 'apps'))) 'and its uninstall removes it'
}

Invoke-Case 'LA4: an app file changed outside the script' {
    $game = New-FakeGame 'LA4' -NoDxgi
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $file = Join-Path (Get-LuaDir $game) 'AcDlssg.lua'
    [IO.File]::AppendAllText($file, "-- edited by hand`r`n")
    $edited = Get-Sha $file
    $r = Install $game $dllV1
    Check ((Test-Refused $r) -and $r.Text -match 'AcDlssg\.lua was changed outside this script' -and $r.Text -match $edited) 'the upgrade refuses and names the hash'
    Check ((Get-Sha $file) -eq $edited) 'the edited file is untouched'
    $r = Install $game $dllV1 -Force
    Check ($r.Code -eq 0 -and (Test-LuaMatches $game $luaReal)) 'with -Force the upgrade replaces it'
    $backups = @(Get-ChildItem -LiteralPath (Join-Path $game 'ac-dlssg\install\backup') -File -ErrorAction SilentlyContinue)
    Check ($backups.Count -eq 1 -and (Get-Sha $backups[0].FullName) -eq $edited) 'the edited file is kept in install\backup'
    [IO.File]::AppendAllText($file, "-- edited again`r`n")
    $r = Uninstall $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check ((Test-Path -LiteralPath $file) -and $r.Text -match 'WARNING: .*AcDlssg\.lua is not the file') 'a changed app file is kept with a warning'
    Check (-not (Test-Path -LiteralPath (Join-Path (Get-LuaDir $game) 'manifest.ini'))) 'the unchanged app file is deleted'
}

Invoke-Case 'LA5: -LuaApp checks and the rollback of a failed install' {
    $game = New-FakeGame 'LA5' -NoDxgi
    $r = Install $game $dllV1 -Lua (Join-Path $FakeRoot 'lua\none\AcDlssg')
    Check ((Test-Refused $r) -and $r.Text -match 'Lua app') 'install refuses a missing -LuaApp'
    $partial = New-LuaVariant 'partial'
    Remove-Item -LiteralPath (Join-Path $partial 'manifest.ini')
    $r = Install $game $dllV1 -Lua $partial
    Check ((Test-Refused $r) -and $r.Text -match 'manifest\.ini') 'install refuses an app folder without manifest.ini'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg')) -and -not (Test-Path -LiteralPath (Join-Path $game 'apps'))) 'nothing was installed'

    # A fresh ReShade-mode install whose ReShade.ini write fails: everything,
    # the app and the folders it created included, is rolled back.
    $game = New-FakeGame 'LA5b'
    $ini = Join-Path $game 'ReShade.ini'
    Write-Bytes $ini (Get-Utf8Bytes $simpleIni)
    $lock = [IO.File]::Open($ini, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $r = Install $game $dllV1
    } finally {
        $lock.Dispose()
    }
    Check ($r.Code -ne 0 -and $r.Text -match 'FAILED:') 'the install fails'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'apps'))) 'the app and the folders the install created are rolled back'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg')) -and -not (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'nothing else is left'
}

Invoke-Case 'CF: the ac-dlssg.ini the install writes has the M3 keys with their defaults' {
    $game = New-FakeGame 'CF' -NoDxgi
    $r = Install $game $dllV1
    Check ($r.Code -eq 0) 'install exits 0'
    $cfg = Join-Path $game 'ac-dlssg\ac-dlssg.ini'
    $lines = @([IO.File]::ReadAllLines($cfg))
    foreach ($kv in @('proxy_without_fg=0', 'tag_without_fg=0', 'fg_vram_headroom_mib=0', 'camera_flip_handedness=0',
            'camera_negate_side=0')) {
        $i = [array]::IndexOf($lines, $kv)
        Check ($i -gt 0 -and $lines[$i - 1].StartsWith(';')) "config has $kv under a comment"
    }
    $bridge = [array]::IndexOf($lines, '[bridge]')
    Check ($bridge -ge 0 -and [array]::IndexOf($lines, 'camera_negate_side=0') -gt $bridge) 'the keys are in [bridge]'
    Check (@($lines | Where-Object { $_ -match '^[a-z_]+=' } | ForEach-Object { ($_ -split '=')[0] } | Group-Object | Where-Object { $_.Count -gt 1 }).Count -eq 0) 'no key twice'

    # An existing ini is kept; the install names the M3 keys it lacks.
    $game = New-FakeGame 'CF2' -NoDxgi
    $old = Join-Path $game 'ac-dlssg\ac-dlssg.ini'
    $oldBytes = Get-Utf8Bytes "[bridge]`r`nenabled=1`r`nproxy_without_fg=1`r`n"
    Write-Bytes $old $oldBytes
    $r = Install $game $dllV1
    Check ($r.Code -eq 0 -and (Test-SameBytes (Read-Bytes $old) $oldBytes)) 'an existing ini is kept byte for byte'
    Check ($r.Text -match 'tag_without_fg' -and $r.Text -match 'camera_negate_side' -and $r.Text -notmatch 'lacks [^\r\n]*proxy_without_fg') 'the install names the M3 keys the existing ini lacks'
    $r = Uninstall $game -RemoveData
    Check ($r.Code -eq 0) 'uninstall exits 0'
}

# ---------------------------------------------------------------------------
# The friend test package (tools\make-test-package.ps1): built from the fake
# bridge DLL into -FakeRoot, then used from its own layout, with no repo.

function Get-ZipEntryNames([string]$Zip) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try { return @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') }) } finally { $archive.Dispose() }
}

# The package is built once, from the fake bridge DLL; PK checks the build,
# and the SP cases each use their own copy of it (their own deps\ folder).
$script:testPackage = $null
function Get-TestPackage {
    if ($script:testPackage) { return $script:testPackage }
    $out = Join-Path $FakeRoot 'package'
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $text = @(& powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $packageScript -Dll $dllV1 `
                -OutDir $out -Version '9.8.7' 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $eap
    }
    $script:testPackage = [pscustomobject]@{ Code = $code; Text = $text; Dir = (Join-Path $out 'ac-dlssg-9.8.7-test') }
    return $script:testPackage
}

function New-PackageCopy([string]$Name) {
    $p = Get-TestPackage
    if ($p.Code -ne 0) { throw 'make-test-package failed; see the PK case' }
    $dest = Join-Path $FakeRoot "packages\$Name\$(Split-Path -Leaf $p.Dir)"
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    # As built: without files\deps and the log zips that the PK case adds.
    foreach ($item in @(Get-ChildItem -LiteralPath $p.Dir -Force)) {
        if ($item.PSIsContainer -and @('files', 'tools') -contains $item.Name) {
            $sub = Join-Path $dest $item.Name
            New-Item -ItemType Directory -Path $sub -Force | Out-Null
            foreach ($child in @(Get-ChildItem -LiteralPath $item.FullName -Force | Where-Object { $_.Name -ne 'deps' -and $_.Name -notlike 'ac-dlssg-logs-*' })) {
                Copy-Item -LiteralPath $child.FullName -Destination $sub -Recurse
            }
        } else {
            Copy-Item -LiteralPath $item.FullName -Destination $dest -Recurse
        }
    }
    return $dest
}

# Runs a package .bat through cmd.exe, with -FakeRoot as the current folder
# and an empty stdin; the same safety net as Invoke-Tool.
function Invoke-Bat([string]$Bat, [string[]]$Arguments) {
    $i = [array]::IndexOf($Arguments, '-GameDir')
    if ($i -lt 0 -or $i + 1 -ge $Arguments.Count -or -not (Test-UnderFakeRoot $Arguments[$i + 1])) {
        throw "refusing to run $Bat without a -GameDir under $FakeRoot"
    }
    Assert-PackageArguments $Bat $Arguments
    $id = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $stdin = Join-Path $tempDir "bat-$id-in.txt"
    $stdout = Join-Path $tempDir "bat-$id-out.txt"
    $stderr = Join-Path $tempDir "bat-$id-err.txt"
    [IO.File]::WriteAllText($stdin, '')
    $argText = (@($Arguments | ForEach-Object { ConvertTo-CommandLineArg $_ })) -join ' '
    $proc = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\cmd.exe') -ArgumentList ('/d /c ""' + $Bat + '" ' + $argText + '"') `
        -WorkingDirectory $FakeRoot -RedirectStandardInput $stdin -RedirectStandardOutput $stdout -RedirectStandardError $stderr -NoNewWindow -PassThru
    # Opened now, so that the exit code is still there after the wait.
    [void]$proc.Handle
    if (-not $proc.WaitForExit(600000)) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        throw "$Bat did not end within 10 minutes"
    }
    $oem = [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.OEMCodePage)
    $text = [IO.File]::ReadAllText($stdout, $oem) + [IO.File]::ReadAllText($stderr, $oem)
    foreach ($line in ($text -split "`r?`n")) { if ($line) { Write-Host "      | $line" } }
    return [pscustomobject]@{ Code = $proc.ExitCode; Text = $text }
}

# dlssg_for_sm86 fixtures: a fake version.dll and dlssg_sm86.ini, and pins
# files for them, correct ones and ones that each get one value wrong.
$spoofNames = @('version.dll', 'dlssg_sm86.ini')
$spoofSrc = Join-Path $FakeRoot 'spoof\src'
New-FakePeDll (Join-Path $spoofSrc 'version.dll') @('GetFileVersionInfoW', 'VerQueryValueW') -Salt '-spoof'
Write-Text (Join-Path $spoofSrc 'dlssg_sm86.ini') "; fake dlssg_for_sm86 settings for the tests`r`n[general]`r`nlog=1`r`n"

function New-SpoofPinsFile([string]$Name, [hashtable]$Wrong = @{}) {
    $pins = [ordered]@{}
    foreach ($n in $spoofNames) {
        $f = Join-Path $spoofSrc $n
        $pins[$n] = [ordered]@{ gitSha1 = (Get-GitBlobSha1 $f); sha256 = (Get-Sha $f); size = (Get-Item -LiteralPath $f).Length }
    }
    foreach ($n in $Wrong.Keys) { foreach ($k in $Wrong[$n].Keys) { $pins[$n][$k] = $Wrong[$n][$k] } }
    $path = Join-Path $FakeRoot "spoof\pins-$Name.json"
    Write-Text $path ($pins | ConvertTo-Json -Depth 4)
    return $path
}
$spoofPins = New-SpoofPinsFile 'good'

function Get-SpoofArgs([string]$Gpu, [string]$Pins = $spoofPins) {
    return @('-GpuDeviceIds', $Gpu, '-SpoofSourceDir', $spoofSrc, '-SpoofPins', $Pins)
}

function Invoke-PackageInstall([string]$Pkg, [string]$Game, [string[]]$Extra) {
    return Invoke-Tool (Join-Path $Pkg 'scripts\install.ps1') (@('-GameDir', $Game, '-StreamlineDir', $slReal, '-NoPause', '-NoElevate') + @($Extra))
}

function Invoke-PackageUninstall([string]$Pkg, [string]$Game, [switch]$RemoveData) {
    $a = @('-GameDir', $Game, '-NoPause', '-NoElevate')
    if ($RemoveData) { $a += '-RemoveData' }
    return Invoke-Tool (Join-Path $Pkg 'scripts\uninstall.ps1') $a
}

function Get-SpoofRecord($Manifest, [string]$Name) {
    if (-not $Manifest.PSObject.Properties['spoof'] -or -not $Manifest.spoof) { return $null }
    $rec = @($Manifest.spoof.files | Where-Object { $_.path -eq $Name })
    if ($rec.Count -ne 1) { return $null }
    return $rec[0]
}

function Test-SpoofInstalled([string]$Game) {
    foreach ($n in $spoofNames) {
        $p = Join-Path $Game $n
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return $false }
        if ((Get-Sha $p) -ne (Get-Sha (Join-Path $spoofSrc $n))) { return $false }
    }
    return $true
}

function Test-NoSpoofFiles([string]$Game) {
    return @($spoofNames | Where-Object { Test-Path -LiteralPath (Join-Path $Game $_) }).Count -eq 0
}

function Get-StagedSpoofDir([string]$Pkg) { return Join-Path $Pkg 'files\deps\dlssg_for_sm86-0.3.5' }

# Every file below $Dir with its hash, for "nothing changed" checks.
function Get-TreeState([string]$Dir) {
    return (@(Get-ChildItem -LiteralPath $Dir -Recurse -Force | Sort-Object FullName | ForEach-Object {
                if ($_.PSIsContainer) { "$($_.FullName)|dir" } else { "$($_.FullName)|$(Get-Sha $_.FullName)" }
            }) -join "`n")
}

$spoofNoticePatterns = @('sdli1995', 'Coldwood1026', 'no LICENSE file', 'nvngx_dlssg\.dll', 'section 4\.d', 'NVIDIA RTX SDKs License',
    'github\.com/sdli1995/dlssg_for_sm86')

Invoke-Case 'PK: the friend test package' {
    $p = Get-TestPackage
    $p.Text | ForEach-Object { Write-Host "      | $_" }
    Check ($p.Code -eq 0) 'make-test-package exits 0'
    $pkg = $p.Dir
    $zip = "$pkg.zip"
    Check ((Test-Path -LiteralPath $pkg -PathType Container) -and (Test-Path -LiteralPath $zip -PathType Leaf)) 'the package folder and the zip next to it exist'
    $expected = @('install.bat', 'files/ac-dlssg.dll', 'scripts/install.ps1', 'scripts/uninstall.ps1', 'scripts/collect-logs.ps1',
        'scripts/collect-sysinfo.ps1', 'scripts/dev-common.ps1', 'scripts/dev-install.ps1', 'scripts/dev-uninstall.ps1',
        'scripts/fetch-deps.ps1', 'tools/uninstall.bat', 'tools/collect-logs.bat', 'tools/README-test.txt') +
        @($luaRealFiles | ForEach-Object { 'files/apps/lua/AcDlssg/' + $_.Replace('\', '/') })
    $files = @(Get-ChildItem -LiteralPath $pkg -Recurse -File | ForEach-Object { $_.FullName.Substring($pkg.Length + 1).Replace('\', '/') } | Sort-Object)
    Check (($files -join '|') -eq (($expected | Sort-Object) -join '|')) "the package holds exactly the expected files ($($files -join ', '))"
    $dlls = @($files | Where-Object { $_ -like '*.dll' })
    Check ($dlls.Count -eq 1 -and $dlls[0] -eq 'files/ac-dlssg.dll') 'the package holds no DLL other than files\ac-dlssg.dll'
    $rootFiles = @(Get-ChildItem -LiteralPath $pkg -File -Force | ForEach-Object { $_.Name })
    $rootDirs = @(Get-ChildItem -LiteralPath $pkg -Directory -Force | ForEach-Object { $_.Name } | Sort-Object)
    Check (($rootFiles -join '|') -eq 'install.bat' -and ($rootDirs -join '|') -eq 'files|scripts|tools') "the package root holds only install.bat and the folders files, scripts, tools ($(@($rootFiles + $rootDirs) -join ', '))"
    $bats = @(Get-ChildItem -LiteralPath $pkg -Recurse -File -Filter '*.bat')
    Check (@($bats | Where-Object { $b = Read-Bytes $_.FullName; @($b | Where-Object { $_ -gt 0x7E }).Count -gt 0 -or $utf8.GetString($b) -match '[^\r]\n' }).Count -eq 0) 'the .bat files are ASCII with CRLF line ends'
    $entries = @(Get-ZipEntryNames $zip | Where-Object { -not $_.EndsWith('/') })
    $zipDlls = @($entries | Where-Object { $_ -like '*.dll' })
    Check ($zipDlls.Count -eq 1 -and $zipDlls[0] -eq 'ac-dlssg-9.8.7-test/files/ac-dlssg.dll') 'the zip holds no DLL other than files\ac-dlssg.dll'
    Check (@($entries | Where-Object { $_ -match '(?i)nvngx|dlssg_sm86|sl\.[a-z_]+\.dll|version\.dll' }).Count -eq 0) 'no NVIDIA or dlssg_for_sm86 file in the zip'
    Check ($entries.Count -eq $expected.Count) 'the zip holds the same files as the folder'
    Check ((Get-Sha (Join-Path $pkg 'files\ac-dlssg.dll')) -eq (Get-Sha $dllV1)) 'the package DLL is the given build'
    $readme = Read-Bytes (Join-Path $pkg 'tools\README-test.txt')
    $readmeText = $utf8.GetString($readme)
    Check ((Test-HasBom $readme) -and $readmeText -match '[Ѐ-ӿ]') 'README-test.txt is UTF-8 with a BOM, in Russian'
    Check ($readmeText -match 'install\.bat' -and $readmeText -match 'tools\\collect-logs\.bat' -and $readmeText -match 'tools\\uninstall\.bat' -and
        $readmeText -notmatch '\.ps1' -and $readmeText -match '9\.8\.7' -and
        $readmeText -match (Get-Sha $dllV1)) 'README-test.txt names install.bat and the two tools\ files (no .ps1), the version and the DLL hash'
    # Russian patterns as \u escapes: Windows PowerShell reads this BOM-less file in the ANSI code page.
    Check ($readmeText -match '\u0433\u0435\u043d\u0435\u0440\u0430\u0446\u0438[\u044f\u044e] \u043a\u0430\u0434\u0440\u043e\u0432' -and
        $readmeText -notmatch '\u0435\u0449\u0451\s+\u0432\u044b\u043a\u043b\u044e\u0447\u0435\u043d\u0430') 'README-test.txt says the build contains frame generation'
    Check ($readmeText -match 'RTX 30' -and $readmeText -match 'dlssg_for_sm86' -and $readmeText -match '(?i)ctrl\s*\+\s*f10' -and
        $readmeText -match 'apps\\lua\\AcDlssg') 'README-test.txt covers RTX 30, dlssg_for_sm86, Ctrl+F10 and the Lua app'
    $urls = @([regex]::Matches($readmeText, 'https?://[^\s)\u00bb"]+') | ForEach-Object { $_.Value.TrimEnd('.', ',') } | Sort-Object -Unique)
    Check (($urls -join ' ') -eq 'https://github.com/sdli1995/dlssg_for_sm86') "the only link is the dlssg_for_sm86 repository, no binary ($($urls -join ', '))"
    Check ($readmeText -notmatch '(?i)defender|\u0430\u043d\u0442\u0438\u0432\u0438\u0440\u0443\u0441|\u0438\u0441\u043a\u043b\u044e\u0447\u0435\u043d\u0438|\u043e\u0442\u043a\u043b\u044e\u0447|smartscreen|smart app control') 'README-test.txt asks for no security feature to be turned off'

    # install.ps1 from the package layout, standalone, Streamline from
    # -StreamlineDir and dlssg_for_sm86 from the fixtures (no download), on a
    # laptop with an Intel GPU and an RTX 3050 Ti Laptop GPU.
    $game = New-FakeGame 'PK' -NoDxgi
    $r = Invoke-Bat (Join-Path $pkg 'install.bat') (@('-GameDir', $game, '-StreamlineDir', $slReal, '-NoPause', '-NoElevate') +
        (Get-SpoofArgs '8086:9A49,10DE:25A0'))
    Check ($r.Code -eq 0) 'install.bat, started from another folder, exits 0'
    Check ($r.Text -match 'mode: standalone') 'it installs in standalone mode (Auto, no dxgi.dll)'
    Check ((Get-Sha (Get-GameDxgi $game)) -eq (Get-Sha $dllV1)) 'the package DLL is <game>\dxgi.dll'
    Check (Test-SlMatches $game $slReal) 'the Streamline files are installed'
    Check ((Get-Manifest $game).mode -eq 'standalone') 'the manifest records standalone mode'
    Check (Test-LuaMatches $game $luaReal) 'the package installs the Lua app into apps\lua\AcDlssg'
    Check (Test-SpoofInstalled $game) 'the RTX 3050 Ti Laptop GPU gets dlssg_for_sm86 next to acs.exe'
    Check ($r.Text -match '10DE:25A0' -and $r.Text -notmatch '8086:9A49') 'it lists the NVIDIA adapter only'
    Check ($r.Text -match '(?i)license' -and $r.Text -match '(?i)means you accept') 'it says that installing means accepting the licenses'
    Check ($r.Text -notmatch '(?i)type y|\(y/n\)|press enter to accept') 'it asks nothing'

    # collect-logs.ps1: read-only, one zip next to the script. The Assetto
    # Corsa documents folder is a fake one (-AcDocsDir) with CSP's log, in
    # which CSP writes the Lua app's lines.
    Write-Text (Join-Path $game 'ac-dlssg\logs\bridge.log') "fake bridge log`r`n"
    Write-Text (Join-Path $game 'ac-dlssg\logs\sl.log') "fake Streamline log`r`n"
    Write-Text (Join-Path $game 'dlssg_sm86\logs\loader_4242.jsonl') "{`"event`":`"fake`"}`r`n"
    Write-Text (Join-Path $game 'dlssg_sm86\logs\backend_4242.jsonl') "{`"event`":`"fake backend`"}`r`n"
    $acDocs = Join-Path $FakeRoot 'documents\Assetto Corsa'
    $cspLog = @(
        ('  98172  4350 Loading app: `' + (Get-LuaDir $game) + '`'),
        '  98200     M [Lua: App: AC DLSS-G Camera] AcDlssg: writing the camera from render.onSceneReady',
        '  98201     M [Lua: App: Other] something else',
        '  98300     M Unrelated CSP line',
        '  98400     M [Lua: App: AC DLSS-G Camera] AcDlssg: at scene ready ac.getSim().cameraPosition differs')
    Write-Text (Join-Path $acDocs 'logs\custom_shaders_patch.log') (($cspLog -join "`r`n") + "`r`n")
    Write-Text (Join-Path $acDocs 'logs\log.txt') "fake AC log`r`n"
    $before = @(Get-ChildItem -LiteralPath $game, $acDocs -Recurse -Force -File | ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" })
    $r = Invoke-Bat (Join-Path $pkg 'tools\collect-logs.bat') @('-GameDir', $game, '-AcDocsDir', $acDocs, '-SkipSysinfo', '-NoPause')
    Check ($r.Code -eq 0) 'tools\collect-logs.bat exits 0'
    $after = @(Get-ChildItem -LiteralPath $game, $acDocs -Recurse -Force -File | ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" })
    Check (($before -join "`n") -eq ($after -join "`n")) 'collect-logs.ps1 changes nothing in the game or documents folder'
    $logZips = @(Get-ChildItem -LiteralPath (Join-Path $pkg 'tools') -Filter 'ac-dlssg-logs-*.zip' -File)
    Check ($logZips.Count -eq 1) 'collect-logs.bat writes one zip next to itself, in tools\'
    Check (@(Get-ChildItem -LiteralPath $pkg -Filter 'ac-dlssg-logs-*' -Recurse | Where-Object { $_.DirectoryName -ne (Join-Path $pkg 'tools') }).Count -eq 0) 'and nothing anywhere else'
    if ($logZips.Count -eq 1) {
        $names = @(Get-ZipEntryNames $logZips[0].FullName)
        foreach ($n in @('ac-dlssg/logs/bridge.log', 'ac-dlssg/logs/sl.log', 'ac-dlssg/ac-dlssg.ini',
                'ac-dlssg/install/dev-manifest.json', 'game-files.txt', 'collect-logs.txt', 'documents/log.txt',
                'documents/custom_shaders_patch.log', 'documents/acdlssg-lua-app.txt',
                'dlssg_sm86/logs/loader_4242.jsonl', 'dlssg_sm86/logs/backend_4242.jsonl')) {
            Check (@($names | Where-Object { $_ -like "*/$n" -or $_ -eq $n }).Count -eq 1) "the log zip holds $n"
        }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [System.IO.Compression.ZipFile]::OpenRead($logZips[0].FullName)
        try {
            $entry = $archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -eq 'documents/acdlssg-lua-app.txt' } | Select-Object -First 1
            $luaLines = @()
            if ($entry) {
                $reader = New-Object System.IO.StreamReader($entry.Open(), $utf8)
                try { $luaLines = @($reader.ReadToEnd() -split "`r?`n" | Where-Object { $_ -and -not $_.StartsWith('#') }) } finally { $reader.Dispose() }
            }
        } finally {
            $archive.Dispose()
        }
        Check ($luaLines.Count -eq 3 -and @($luaLines | Where-Object { $_ -match 'Other|Unrelated' }).Count -eq 0) "acdlssg-lua-app.txt holds the app's three CSP log lines and nothing else ($($luaLines.Count) lines)"
        Remove-Item -LiteralPath $logZips[0].FullName
    }
    Check (@(Get-ChildItem -LiteralPath (Join-Path $pkg 'tools') -Directory | Where-Object { $_.Name -like 'ac-dlssg-logs-*' }).Count -eq 0) 'no staging folder left'

    $r = Invoke-Bat (Join-Path $pkg 'tools\uninstall.bat') @('-GameDir', $game, '-NoPause', '-NoElevate')
    Check ($r.Code -eq 0 -and -not (Test-Path -LiteralPath (Get-GameDxgi $game))) 'tools\uninstall.bat removes dxgi.dll'
    Check (Test-NoSpoofFiles $game) 'and the dlssg_for_sm86 files it installed'

    # Without -StreamlineDir the package uses <package>\files\deps as
    # fetch-deps.ps1 stages it; already staged, nothing is downloaded.
    $deps = Join-Path $pkg 'files\deps'
    New-Item -ItemType Directory -Path $deps -Force | Out-Null
    $staged = Join-Path $tools '..\deps\streamline-2.14.1'
    Copy-Item -LiteralPath $staged -Destination (Join-Path $deps 'streamline-2.14.1') -Recurse
    Copy-Item -LiteralPath "$staged.sha256" -Destination (Join-Path $deps 'streamline-2.14.1.sha256')
    $r = Invoke-Tool (Join-Path $pkg 'scripts\install.ps1') @('-GameDir', $game, '-NoSpoof', '-NoPause', '-NoElevate')
    Check ($r.Code -eq 0) 'install.ps1 without -StreamlineDir exits 0 with <package>\files\deps staged'
    Check ($r.Text -match 'present and verified' -and $r.Text -notmatch 'downloading') 'it verifies the staged Streamline and downloads nothing'
    Check ($r.Text -match 'nvngx_dlss\.license\.txt') 'it names the NVIDIA license files'
    Check ($r.Text -match '(?i)means you accept' -and $r.Text -notmatch '(?i)type y|\(y/n\)') 'it says that installing means accepting them, and asks nothing'
    Check ($r.Text -match 'skipped \(-NoSpoof\)' -and (Test-NoSpoofFiles $game)) '-NoSpoof skips dlssg_for_sm86'
    Check (Test-SlMatches $game (Join-Path $deps 'streamline-2.14.1\bin\x64')) 'the staged Streamline files are installed'
    $r = Invoke-Tool (Join-Path $pkg 'scripts\uninstall.ps1') @('-GameDir', $game, '-RemoveData', '-NoPause', '-NoElevate')
    Check ($r.Code -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg'))) 'uninstall.ps1 -RemoveData removes everything'
}

# ---------------------------------------------------------------------------
# dlssg_for_sm86 (spec 10): the package install.ps1 installs it on RTX 30.

Invoke-Case 'SP0: GPU table, dlssg_for_sm86 pins, and install.ps1 asks nothing (static)' {
    # The installer's device-ID table is a port of kRanges in src/gpu_info.cpp.
    $cpp = [IO.File]::ReadAllText((Join-Path $tools '..\src\gpu_info.cpp'))
    $consts = @{}
    foreach ($m in [regex]::Matches($cpp, 'constexpr uint32_t (\w+) = (0x[0-9A-Fa-f]+);')) {
        $consts[$m.Groups[1].Value] = [Convert]::ToInt32($m.Groups[2].Value, 16)
    }
    $toId = { param($t) if ($consts.ContainsKey($t)) { $consts[$t] } else { [Convert]::ToInt32($t, 16) } }
    $cppRanges = @(foreach ($m in [regex]::Matches($cpp, '\{\s*(\w+),\s*(\w+),\s*GpuArch::(\w+)\s*\}')) {
            '{0:X4}-{1:X4} {2}' -f (& $toId $m.Groups[1].Value), (& $toId $m.Groups[2].Value), $m.Groups[3].Value
        })
    $psRanges = @($script:AcdbGpuRanges | ForEach-Object { '{0:X4}-{1:X4} {2}' -f $_.First, $_.Last, $_.Arch })
    Check ($cppRanges.Count -ge 8 -and ($cppRanges -join '|') -eq ($psRanges -join '|')) "the installer's GPU table is kRanges of src/gpu_info.cpp ($($psRanges -join ', '))"
    Check ($script:AcdbFirstTuringId -eq $consts['kFirstTuringId'] -and $script:AcdbGa100First -eq $consts['kGa100First'] -and
        $script:AcdbGa100Last -eq $consts['kGa100Last']) 'the first Turing ID and the GA100 range match src/gpu_info.cpp'
    foreach ($t in @(
            @('10DE:2206', 'Ampere', $true, 'GA102, RTX 3080'),
            @('10DE:2520', 'Ampere', $true, 'GA106, RTX 3060 Laptop'),
            @('10DE:25A0', 'Ampere', $true, 'GA107, RTX 3050 Ti Laptop'),
            @('10DE:20B0', 'Ampere', $false, 'GA100, A100, SM80'),
            @('10DE:2684', 'Ada', $false, 'AD102, RTX 4090'),
            @('10DE:2B85', 'Blackwell', $false, 'GB202, RTX 5090'),
            @('10DE:1E84', 'Turing', $false, 'TU104, RTX 2070 SUPER'),
            @('10DE:2182', 'Turing', $false, 'TU116, GTX 1660 Ti'),
            @('10DE:1B80', 'OlderNvidia', $false, 'GP104, GTX 1080'),
            @('10DE:3000', 'Unknown', $false, 'not in the table'),
            @('8086:2206', 'NonNvidia', $false, 'Intel'))) {
        $a = @(Get-GpuAdapters @($t[0]))
        Check ($a.Count -eq 1 -and $a[0].Arch -eq $t[1] -and $a[0].Sm86 -eq $t[2]) "$($t[0]) ($($t[3])) is $($t[1])$(if ($t[2]) { ', SM86' })"
    }
    $two = @(Get-GpuAdapters @('8086:9A49,10DE:25A0'))
    Check ($two.Count -eq 2 -and $two[1].Id -eq '10DE:25A0' -and $two[1].Sm86) '-GpuDeviceIds "a,b" is two adapters'
    $pnp = ConvertFrom-PnpDeviceId 'PCI\VEN_10DE&DEV_2206&SUBSYS_38971462&REV_A1\4&2B5B5B5B&0&0008'
    Check ($pnp -and $pnp.VendorId -eq 0x10DE -and $pnp.DeviceId -eq 0x2206) 'a Win32_VideoController PNPDeviceID gives vendor 10DE, device 2206'
    Check ($null -eq (ConvertFrom-PnpDeviceId 'ROOT\BASICDISPLAY\0000')) 'a PNPDeviceID without VEN_ and DEV_ gives nothing'
    $threw = $false
    try { [void](Get-GpuAdapters @('RTX 3080')) } catch { $threw = $true }
    Check $threw 'a -GpuDeviceIds value that is not VVVV:DDDD is refused'

    # The pins of spec 10, and the URL form the GitHub contents API gives.
    $pins = Get-SpoofPins ''
    Check ((@($pins.Keys) -join '|') -eq 'version.dll|dlssg_sm86.ini') 'exactly the two files version.dll and dlssg_sm86.ini'
    Check ($pins['version.dll'].gitSha1 -eq 'efd92261f2b74e0a0fb927d74bce7a1c0c2413f7' -and
        $pins['version.dll'].sha256 -eq 'c3934a09399f022504227c72df0bf8c0de55f9a08880dddde898c5262cefa838' -and
        $pins['version.dll'].size -eq 30021920) 'version.dll: git blob SHA-1, SHA-256 and size of 0.3.5'
    Check ($pins['dlssg_sm86.ini'].gitSha1 -eq '2c97d64f2239b7d511f7d0a36c16e149dd3329f6') 'dlssg_sm86.ini: git blob SHA-1 of 0.3.5'
    $urls = @($pins.Keys | ForEach-Object { Get-SpoofUrl $_ })
    Check ($urls[0] -eq 'https://raw.githubusercontent.com/sdli1995/dlssg_for_sm86/9621db573e07ed54f50c15bbb585ed9a7bdfac28/version.dll' -and
        $urls[1] -eq 'https://raw.githubusercontent.com/sdli1995/dlssg_for_sm86/9621db573e07ed54f50c15bbb585ed9a7bdfac28/dlssg_sm86.ini') "the download URLs are raw files of commit 9621db5 ($($urls -join ', '))"
    Check (@($urls | Where-Object { $_ -match '(?i)archive|alternatives|zipball|tarball' }).Count -eq 0) 'never the repository archive or alternatives/'
    $hello = Join-Path $FakeRoot 'spoof\hello.txt'
    Write-Text $hello "hello`n"
    Check ((Get-GitBlobSha1 $hello) -eq 'ce013625030ba8dba906f756967f9e9ca394464a') 'Get-GitBlobSha1 gives git''s blob id'

    # No prompt: the package scripts and the scripts they run ask nothing;
    # the only Read-Host is the last line, "Press Enter to exit".
    $entry = @((Join-Path $tools 'package\install.ps1'), (Join-Path $tools 'package\uninstall.ps1'), (Join-Path $tools 'collect-logs.ps1'))
    $helpers = @((Join-Path $tools 'dev-install.ps1'), (Join-Path $tools 'dev-uninstall.ps1'), (Join-Path $tools 'dev-common.ps1'),
        (Join-Path $tools 'fetch-deps.ps1'), (Join-Path $tools 'collect-sysinfo.ps1'))
    $prompts = @('Read-Host', 'Get-Credential', 'Out-GridView', 'pause', 'choice', 'choice.exe')
    $promptMembers = @('PromptForChoice', 'PromptForCredential', 'Prompt', 'ReadLine', 'ReadKey')
    foreach ($f in @($entry + $helpers)) {
        $tokens = $null
        $errors = $null
        $ast = [System.Management.Automation.Language.Parser]::ParseFile($f, [ref]$tokens, [ref]$errors)
        $leaf = Split-Path -Leaf $f
        Check (@($errors).Count -eq 0) "$leaf parses"
        $commandAsts = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] }, $true))
        $readHosts = @($commandAsts | Where-Object { $_.GetCommandName() -eq 'Read-Host' })
        $bad = @($commandAsts | ForEach-Object { $_.GetCommandName() } | Where-Object { $_ -and $_ -ne 'Read-Host' -and $prompts -contains $_ } | Sort-Object -Unique)
        $members = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.InvokeMemberExpressionAst] }, $true))
        $bad += @($members | Where-Object { $promptMembers -contains $_.Member.Value } | ForEach-Object { $_.Member.Value })
        Check ($bad.Count -eq 0) "$leaf has no prompt ($($bad -join ', '))"
        if ($helpers -contains $f) {
            Check ($readHosts.Count -eq 0) "$leaf has no Read-Host"
        } else {
            $text = [IO.File]::ReadAllText($f)
            Check ($readHosts.Count -eq 1 -and $readHosts[0].Extent.Text -match "Read-Host 'Press Enter to exit'") "$leaf has one Read-Host, 'Press Enter to exit'"
            Check ($text -match '(?s)IsInputRedirected.{0,200}Read-Host|NoPause.{0,300}Read-Host') "$leaf skips it with -NoPause or redirected input"
            $tail = ($text.TrimEnd() -split "`r?`n") | Select-Object -Last 2
            Check (($tail[1].Trim() -eq 'exit $code') -and ($tail[0].Trim() -eq 'Wait-BeforeClose' -or $tail[0].Trim() -eq '}')) "$leaf waits for Enter only at its very end"
            Check ($text -notmatch '(?i)type y|AcceptNvidiaLicenses') "$leaf has no license question"
        }
    }
    foreach ($f in @('package\install.ps1', 'package\uninstall.ps1', 'dev-common.ps1', 'dev-install.ps1', 'dev-uninstall.ps1', 'fetch-deps.ps1',
            'make-test-package.ps1', 'collect-logs.ps1', 'package\install.bat', 'package\uninstall.bat', 'package\collect-logs.bat')) {
        $bytes = Read-Bytes (Join-Path $tools $f)
        Check ($bytes.Length -gt 0 -and @($bytes | Where-Object { $_ -gt 0x7E -or ($_ -lt 0x20 -and $_ -ne 9 -and $_ -ne 10 -and $_ -ne 13) }).Count -eq 0) "$f is ASCII"
    }
}

Invoke-Case 'SP1: RTX 30: dlssg_for_sm86 is fetched, verified, installed, recorded, and removed again' {
    $pkg = New-PackageCopy 'SP1'
    $game = New-FakeGame 'SP1' -NoDxgi
    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:2206')
    Check ($r.Code -eq 0) 'install exits 0'
    Check ($r.Text -match '10DE:2206' -and $r.Text -match 'SM86') 'it names the RTX 30 (SM86) adapter'
    $missing = @($spoofNoticePatterns | Where-Object { $r.Text -notmatch $_ })
    Check ($missing.Count -eq 0) "it prints the dlssg_for_sm86 notice (missing: $($missing -join ', '))"
    $noticeAt = $r.Text.IndexOf('Coldwood1026')
    $fetchAt = $r.Text.IndexOf('fetch-deps:')
    Check ($noticeAt -ge 0 -and $fetchAt -gt $noticeAt) 'the notice comes before the download'
    Check ($r.Text -match 'copying .*-SpoofSourceDir' -and $r.Text -notmatch 'downloading https') 'the fixtures stand in for the download'
    Check ($r.Text -notmatch '(?i)type y|\(y/n\)') 'it asks nothing'
    Check (Test-SpoofInstalled $game) 'version.dll and dlssg_sm86.ini are next to acs.exe with the fixture hashes'
    $staged = Get-StagedSpoofDir $pkg
    Check (@($spoofNames | Where-Object { (Get-Sha (Join-Path $staged $_)) -eq (Get-Sha (Join-Path $spoofSrc $_)) }).Count -eq 2) 'both files are staged in <package>\files\deps\dlssg_for_sm86-0.3.5'
    Check (-not (Test-Path -LiteralPath (Join-Path $pkg 'files\deps\download'))) 'no download left in <package>\files\deps\download'
    $m = Get-Manifest $game
    foreach ($n in $spoofNames) {
        $rec = Get-SpoofRecord $m $n
        Check ($rec -and $rec.origin -eq 'installed' -and $rec.sha256 -eq (Get-Sha (Join-Path $spoofSrc $n))) "the manifest records $n as installed, with its hash"
    }
    Check ($m.spoof.version -eq '0.3.5' -and $m.spoof.repository -eq 'sdli1995/dlssg_for_sm86' -and
        $m.spoof.commit -eq '9621db573e07ed54f50c15bbb585ed9a7bdfac28') 'the manifest records the version, repository and commit'
    $slAt = $r.Text.LastIndexOf('copied ac-dlssg\sl\')
    $spoofAt = $r.Text.IndexOf('copied version.dll')
    $luaAt = $r.Text.IndexOf('copied apps\lua\AcDlssg\')
    Check ($slAt -ge 0 -and $spoofAt -gt $slAt -and $luaAt -gt $spoofAt -and $r.Text.IndexOf('copied dxgi.dll') -gt $luaAt) 'spec 12 order: Streamline, the spoof files, the Lua app, the bridge'
    Check (Test-NoLeftovers @($game)) 'no .new files left'

    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:2206')
    Check ($r.Code -eq 0) 're-run exits 0'
    Check ($r.Text -notmatch 'fetch-deps:' -and $r.Text -notmatch 'Coldwood1026') 'the re-run downloads nothing and prints no notice'
    $m = Get-Manifest $game
    Check (@($spoofNames | Where-Object { (Get-SpoofRecord $m $_).origin -eq 'installed' }).Count -eq 2) 'the re-run keeps both files recorded as installed (not as found)'

    Write-Text (Join-Path $game 'dlssg_sm86\logs\loader_1.jsonl') "{}`r`n"
    Write-Text (Join-Path $env:LOCALAPPDATA 'DlssgSm86\cache\kernels.bin') 'fake cache'
    $r = Invoke-PackageUninstall $pkg $game -RemoveData
    Check ($r.Code -eq 0) 'uninstall -RemoveData exits 0'
    Check (Test-NoSpoofFiles $game) 'uninstall removes version.dll and dlssg_sm86.ini'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'dlssg_sm86'))) '-RemoveData deletes <game>\dlssg_sm86'
    Check (-not (Test-Path -LiteralPath (Join-Path $env:LOCALAPPDATA 'DlssgSm86'))) '-RemoveData deletes %LOCALAPPDATA%\DlssgSm86 (a fake one)'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg')) -and -not (Test-Path -LiteralPath (Get-GameDxgi $game)) -and
        (Test-Path -LiteralPath (Join-Path $game 'acs.exe'))) 'the rest is gone too, acs.exe stays'
}

Invoke-Case 'SP2: RTX 40 and RTX 50 need no spoof' {
    $pkg = New-PackageCopy 'SP2'
    foreach ($gpu in @('10DE:2684', '8086:46A6,10DE:2B85', '10DE:2206,10DE:2684')) {
        $game = New-FakeGame "SP2-$($gpu.Replace(':', '').Replace(',', '-'))" -NoDxgi
        $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs $gpu)
        Check ($r.Code -eq 0) "$gpu`: install exits 0"
        Check ($r.Text -match 'RTX 40 or newer' -and $r.Text -match 'not needed') "$gpu`: it says dlssg_for_sm86 is not needed"
        Check (Test-NoSpoofFiles $game) "$gpu`: no spoof file installed"
        Check ($r.Text -notmatch 'fetch-deps:' -and $r.Text -notmatch 'Coldwood1026') "$gpu`: no notice and no download"
        Check ($null -eq (Get-SpoofRecord (Get-Manifest $game) 'version.dll')) "$gpu`: the manifest has no spoof record"
    }
    Check (-not (Test-Path -LiteralPath (Get-StagedSpoofDir $pkg))) 'nothing is staged'
    # -RemoveData leaves dlssg_for_sm86 data that the install did not put there.
    Write-Text (Join-Path $game 'dlssg_sm86\logs\loader_2.jsonl') "{}`r`n"
    Write-Text (Join-Path $env:LOCALAPPDATA 'DlssgSm86\cache\kernels.bin') 'fake cache'
    $r = Invoke-PackageUninstall $pkg $game -RemoveData
    Check ($r.Code -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg'))) 'uninstall -RemoveData exits 0'
    Check ((Test-Path -LiteralPath (Join-Path $game 'dlssg_sm86\logs\loader_2.jsonl')) -and
        (Test-Path -LiteralPath (Join-Path $env:LOCALAPPDATA 'DlssgSm86\cache\kernels.bin'))) 'the spoof data it did not install stays'
    Remove-Item -LiteralPath (Join-Path $env:LOCALAPPDATA 'DlssgSm86') -Recurse -Force
}

Invoke-Case 'SP3: RTX 20 is not supported, and gets no spoof' {
    $pkg = New-PackageCopy 'SP3'
    $game = New-FakeGame 'SP3' -NoDxgi
    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:1E84')
    Check ($r.Code -eq 0) 'install exits 0 (the bridge is installed)'
    Check ($r.Text -match 'RTX 20' -and $r.Text -match 'not supported') 'it says frame generation is not supported on RTX 20'
    Check (Test-NoSpoofFiles $game) 'no spoof file installed'
    Check ($r.Text -notmatch 'fetch-deps:' -and $r.Text -notmatch 'Coldwood1026') 'no notice and no download'
    $game = New-FakeGame 'SP3b' -NoDxgi
    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '8086:9A49')
    Check ($r.Code -eq 0 -and $r.Text -match 'no NVIDIA GPU' -and (Test-NoSpoofFiles $game)) 'no NVIDIA GPU: install exits 0 without the spoof'
}

Invoke-Case 'SP4: a download that fails its pins stops the install with nothing changed' {
    $pkg = New-PackageCopy 'SP4'
    $staged = Get-StagedSpoofDir $pkg
    $cases = @(
        @('bad-sha1', @{ 'version.dll' = @{ gitSha1 = ('0' * 40) } }, 'version\.dll has git blob SHA-1'),
        @('bad-sha256', @{ 'version.dll' = @{ sha256 = ('0' * 64) } }, 'version\.dll has SHA-256'),
        @('bad-size', @{ 'version.dll' = @{ size = 30021920 } }, 'version\.dll is \d+ bytes'),
        @('bad-ini', @{ 'dlssg_sm86.ini' = @{ gitSha1 = ('1' * 40) } }, 'dlssg_sm86\.ini has git blob SHA-1'))
    foreach ($c in $cases) {
        $game = New-FakeGame "SP4-$($c[0])" -NoDxgi
        $before = Get-TreeState $game
        $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:2206' (New-SpoofPinsFile $c[0] $c[1]))
        Check ((Test-Refused $r) -and $r.Text -match $c[2]) "$($c[0]): the install refuses and names the mismatch"
        Check ($r.Text -match 'Nothing was installed') "$($c[0]): it says nothing was installed"
        Check ((Get-TreeState $game) -eq $before) "$($c[0]): the game folder is unchanged"
        Check (@(Get-ChildItem -LiteralPath (Join-Path $pkg 'files\deps') -Recurse -File -ErrorAction SilentlyContinue).Count -eq 0) "$($c[0]): the download is deleted"
    }
    Check (-not (Test-Path -LiteralPath (Join-Path $staged 'version.dll'))) 'no spoof file is staged'
}

Invoke-Case 'SP5: a pinned version.dll already in the game folder is recorded as found and survives the uninstall' {
    $pkg = New-PackageCopy 'SP5'
    $game = New-FakeGame 'SP5' -NoDxgi
    $vd = Join-Path $game 'version.dll'
    Copy-Item -LiteralPath (Join-Path $spoofSrc 'version.dll') -Destination $vd
    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:2206')
    Check ($r.Code -eq 0) 'install exits 0'
    Check ($r.Text -match 'version\.dll' -and $r.Text -match 'already in the game folder') 'it says the pinned version.dll is already there'
    Check (-not (Test-Path -LiteralPath (Join-Path (Get-StagedSpoofDir $pkg) 'version.dll'))) 'version.dll is not downloaded'
    Check (Test-SpoofInstalled $game) 'the missing dlssg_sm86.ini is fetched and installed next to it'
    $m = Get-Manifest $game
    Check ((Get-SpoofRecord $m 'version.dll').origin -eq 'found' -and (Get-SpoofRecord $m 'version.dll').sha256 -eq (Get-Sha $vd)) 'the manifest records version.dll as found, with its hash'
    Check ((Get-SpoofRecord $m 'dlssg_sm86.ini').origin -eq 'installed') 'and dlssg_sm86.ini as installed'
    Write-Text (Join-Path $game 'dlssg_sm86\logs\loader_3.jsonl') "{}`r`n"
    $r = Invoke-PackageUninstall $pkg $game -RemoveData
    Check ($r.Code -eq 0) 'uninstall -RemoveData exits 0'
    Check ((Test-Path -LiteralPath $vd) -and (Get-Sha $vd) -eq (Get-Sha (Join-Path $spoofSrc 'version.dll'))) 'the found version.dll survives the uninstall'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'dlssg_sm86.ini'))) 'the dlssg_sm86.ini it installed is removed'
    Check (Test-Path -LiteralPath (Join-Path $game 'dlssg_sm86\logs\loader_3.jsonl')) 'the data of a spoof that is still there stays'

    # Both files already there: nothing is fetched, both are found.
    $game = New-FakeGame 'SP5b' -NoDxgi
    foreach ($n in $spoofNames) { Copy-Item -LiteralPath (Join-Path $spoofSrc $n) -Destination (Join-Path $game $n) }
    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:2206')
    Check ($r.Code -eq 0 -and $r.Text -notmatch 'fetch-deps:' -and $r.Text -notmatch 'Coldwood1026') 'both there: no download, no notice'
    $m = Get-Manifest $game
    Check (@($spoofNames | Where-Object { (Get-SpoofRecord $m $_).origin -eq 'found' }).Count -eq 2) 'both are recorded as found'
    $r = Invoke-PackageUninstall $pkg $game
    Check ($r.Code -eq 0 -and (Test-SpoofInstalled $game)) 'the uninstall leaves both'
}

Invoke-Case 'SP6: a foreign version.dll is left alone, with a warning' {
    $pkg = New-PackageCopy 'SP6'
    $game = New-FakeGame 'SP6' -NoDxgi
    $vd = Join-Path $game 'version.dll'
    Write-Text $vd 'the version.dll of another mod'
    $foreignSha = Get-Sha $vd
    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:2206')
    Check ($r.Code -eq 0) 'install exits 0'
    Check ($r.Text -match 'WARNING: .*version\.dll' -and $r.Text -match 'left untouched' -and $r.Text -match $foreignSha) 'it warns clearly and names the hash'
    Check ((Get-Sha $vd) -eq $foreignSha) 'the foreign version.dll is untouched'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'dlssg_sm86.ini'))) 'no dlssg_sm86.ini is installed next to it'
    Check ($r.Text -notmatch 'fetch-deps:') 'nothing is downloaded'
    Check ($null -eq (Get-SpoofRecord (Get-Manifest $game) 'version.dll')) 'the manifest has no spoof record'
    $r = Invoke-PackageUninstall $pkg $game -RemoveData
    Check ($r.Code -eq 0 -and (Get-Sha $vd) -eq $foreignSha) 'the uninstall leaves it too'
}

Invoke-Case 'SP7: -NoSpoof, and the uninstall removes only what the install put there' {
    $pkg = New-PackageCopy 'SP7'
    $game = New-FakeGame 'SP7' -NoDxgi
    $r = Invoke-PackageInstall $pkg $game @('-GpuDeviceIds', '10DE:2206', '-NoSpoof')
    Check ($r.Code -eq 0 -and $r.Text -match 'skipped \(-NoSpoof\)') '-NoSpoof: install exits 0 and says so'
    Check ((Test-NoSpoofFiles $game) -and $r.Text -notmatch 'fetch-deps:' -and $r.Text -notmatch 'Coldwood1026') '-NoSpoof: no spoof, no notice, no download'

    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:2206')
    Check ($r.Code -eq 0 -and (Test-SpoofInstalled $game)) 'a later run without -NoSpoof installs it'
    $r = Invoke-PackageInstall $pkg $game @('-GpuDeviceIds', '10DE:2206', '-NoSpoof')
    Check ($r.Code -eq 0 -and (Test-SpoofInstalled $game)) 'a -NoSpoof re-run leaves the installed spoof alone'
    Check ((Get-SpoofRecord (Get-Manifest $game) 'version.dll').origin -eq 'installed') 'and keeps its record'
    $ini = Join-Path $game 'dlssg_sm86.ini'
    [IO.File]::AppendAllText($ini, "; tuned by the user`r`n")
    $tuned = Get-Sha $ini
    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:2206')
    Check ($r.Code -eq 0 -and (Get-Sha $ini) -eq $tuned -and $r.Text -match 'dlssg_sm86\.ini differs') 'a re-run keeps a dlssg_sm86.ini the user changed'
    $r = Invoke-PackageUninstall $pkg $game
    Check ($r.Code -eq 0) 'uninstall exits 0'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'version.dll'))) 'the installed version.dll is removed'
    Check ((Get-Sha $ini) -eq $tuned -and $r.Text -match 'WARNING: .*dlssg_sm86\.ini is not the file') 'the changed dlssg_sm86.ini is kept with a warning'
}

Invoke-Case 'SP8: ReShade mode with the spoof: a byte-identical round trip' {
    $pkg = New-PackageCopy 'SP8'
    $game = New-FakeGame 'SP8'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-RealisticIni 'EnableProxyLibrary=0' 'ProxyLibrary='
    Write-Bytes $ini $original
    $before = Get-TreeState $game
    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:2206')
    Check ($r.Code -eq 0 -and $r.Text -match 'mode: reshade') 'install exits 0 in ReShade mode'
    Check ((Test-SpoofInstalled $game) -and (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'the spoof and the bridge are installed'
    $r = Invoke-PackageUninstall $pkg $game -RemoveData
    Check ($r.Code -eq 0) 'uninstall -RemoveData exits 0'
    Check ((Get-TreeState $game) -eq $before) 'the game folder is exactly as before, ReShade.ini byte for byte'
}

Invoke-Case 'SP9: without -NoPause, install.ps1 with redirected input ends without waiting' {
    $pkg = New-PackageCopy 'SP9'
    $notGame = Join-Path $FakeRoot 'games\SP9\not a game'
    New-Item -ItemType Directory -Path $notGame -Force | Out-Null
    $stdin = Join-Path $FakeRoot 'games\SP9\stdin.txt'
    $stdout = Join-Path $FakeRoot 'games\SP9\stdout.txt'
    $stderr = Join-Path $FakeRoot 'games\SP9\stderr.txt'
    Write-Text $stdin "`r`n"
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $pkg 'scripts\install.ps1')`"", '-GameDir', "`"$notGame`"", '-NoSpoof',
        '-NoElevate')
    $proc = Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -RedirectStandardInput $stdin -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr -NoNewWindow -PassThru
    $ended = $proc.WaitForExit(60000)
    if (-not $ended) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    $out = [IO.File]::ReadAllText($stdout)
    foreach ($line in ($out -split "`r?`n")) { if ($line) { Write-Host "      | $line" } }
    Check $ended 'install.ps1 ends by itself'
    Check ($out -match 'REFUSED:' -and $out -notmatch '(?i)press enter') 'it refuses the folder and does not wait for Enter'
}

# ---------------------------------------------------------------------------
# Upgrades through the package: over any earlier install they need nothing
# from the user (dev-install.ps1 -AutoUpgrade).

function Set-Manifest([string]$Game, $Manifest) {
    [IO.File]::WriteAllText((Get-ManifestPath $Game), ($Manifest | ConvertTo-Json -Depth 8), $utf8)
}

Invoke-Case 'UP1: upgrade of an older package install: changed files are replaced, obsolete ones removed' {
    $pkg = New-PackageCopy 'UP1'
    $game = New-FakeGame 'UP1' -NoDxgi
    $r = Invoke-PackageInstall $pkg $game @('-NoSpoof')
    Check ($r.Code -eq 0) 'the first install exits 0'
    # What an older package leaves: a schema 3 manifest without spoof, an
    # older bridge build it recorded, an app file that build changed after
    # recording it, and files the new build no longer ships.
    $m = Get-Manifest $game
    $m.schema = 3
    [void]$m.PSObject.Properties.Remove('spoof')
    Copy-Item -LiteralPath $dllV2 -Destination (Get-GameDxgi $game) -Force
    $m.dll.sha256 = Get-Sha $dllV2
    $oldSl = Join-Path (Get-SlDir $game) 'sl.old.dll'
    Write-Text $oldSl 'an obsolete Streamline file'
    $m.streamline.files = @($m.streamline.files) + @([pscustomobject]@{ path = 'ac-dlssg\sl\sl.old.dll'; sha256 = (Get-Sha $oldSl) })
    $oldLua = Join-Path (Get-LuaDir $game) 'lib\old.lua'
    Write-Text $oldLua "-- obsolete`r`n"
    $m.luaApp.files = @($m.luaApp.files) + @([pscustomobject]@{ path = 'apps\lua\AcDlssg\lib\old.lua'; sha256 = (Get-Sha $oldLua) })
    [IO.File]::AppendAllText($oldLua, "-- changed after it was recorded`r`n")
    $changedOld = Get-Sha $oldLua
    $appFile = Join-Path (Get-LuaDir $game) 'AcDlssg.lua'
    [IO.File]::AppendAllText($appFile, "-- changed by an older build`r`n")
    $changedApp = Get-Sha $appFile
    Set-Manifest $game $m

    $r = Invoke-PackageInstall $pkg $game @('-NoSpoof')
    Check ($r.Code -eq 0 -and $r.Text -notmatch 'REFUSED') 'the upgrade exits 0 without refusing or asking'
    Check ((Get-Sha (Get-GameDxgi $game)) -eq (Get-Sha $dllV1)) 'the older bridge build is replaced'
    Check (Test-LuaMatches $game $luaReal) 'the Lua app is exactly the new one (the changed file replaced, the obsolete one and its folder gone)'
    Check (Test-SlMatches $game $slReal) 'ac-dlssg\sl is exactly the new Streamline (the obsolete file removed)'
    $m = Get-Manifest $game
    Check ($m.schema -eq 4 -and $m.dll.sha256 -eq (Get-Sha $dllV1)) 'the manifest is schema 4 and records the new bridge'
    Check (@($m.streamline.files | Where-Object { $_.path -like '*sl.old.dll' }).Count -eq 0 -and
        @($m.luaApp.files | Where-Object { $_.path -like '*old.lua' }).Count -eq 0) 'the manifest no longer records the obsolete files'
    $backups = @(Get-ChildItem -LiteralPath (Join-Path $game 'ac-dlssg\install\backup') -File -ErrorAction SilentlyContinue | ForEach-Object { Get-Sha $_.FullName } | Sort-Object)
    Check (($backups -join '|') -eq ((@($changedApp, $changedOld) | Sort-Object) -join '|')) 'install\backup keeps exactly the two changed files'
    $r = Invoke-PackageUninstall $pkg $game -RemoveData
    Check ($r.Code -eq 0 -and @(Get-ChildItem -LiteralPath $game -Force).Count -eq 1) 'uninstall -RemoveData leaves only acs.exe'
}

Invoke-Case 'UP2: upgrade of a schema 1 ReShade-mode install (no Streamline, no Lua app)' {
    $pkg = New-PackageCopy 'UP2'
    $game = New-FakeGame 'UP2'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-RealisticIni 'EnableProxyLibrary=0' 'ProxyLibrary='
    Write-Bytes $ini $original
    $r = Invoke-PackageInstall $pkg $game @('-NoSpoof')
    Check ($r.Code -eq 0 -and $r.Text -match 'mode: reshade') 'the first install exits 0 in ReShade mode'
    $afterFirst = Read-Bytes $ini
    $m = Get-Manifest $game
    $m.schema = 1
    foreach ($name in @('mode', 'streamline', 'luaApp', 'spoof')) { [void]$m.PSObject.Properties.Remove($name) }
    Set-Manifest $game $m
    Remove-Item -LiteralPath (Get-SlDir $game) -Recurse -Force
    Remove-Item -LiteralPath (Join-Path $game 'apps') -Recurse -Force
    $r = Invoke-PackageInstall $pkg $game @('-NoSpoof')
    Check ($r.Code -eq 0) 'the upgrade exits 0'
    Check ((Test-SlMatches $game $slReal) -and (Test-LuaMatches $game $luaReal)) 'it adds Streamline and the Lua app'
    Check (Test-SameBytes (Read-Bytes $ini) $afterFirst) 'ReShade.ini is unchanged'
    $m = Get-Manifest $game
    Check ($m.mode -eq 'reshade' -and $m.schema -eq 4) 'the manifest records ReShade mode, schema 4'
    $r = Invoke-PackageUninstall $pkg $game
    Check ($r.Code -eq 0 -and (Test-SameBytes (Read-Bytes $ini) $original)) 'the uninstall restores a byte-identical ReShade.ini'
}

Invoke-Case 'UP3: ReShade removed since: the upgrade switches to standalone mode in one run' {
    $pkg = New-PackageCopy 'UP3'
    $game = New-FakeGame 'UP3'
    $ini = Join-Path $game 'ReShade.ini'
    $original = Get-RealisticIni 'EnableProxyLibrary=0' 'ProxyLibrary='
    Write-Bytes $ini $original
    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:2206')
    Check ($r.Code -eq 0 -and $r.Text -match 'mode: reshade' -and (Test-SpoofInstalled $game)) 'the first install exits 0 in ReShade mode, with the spoof'
    Remove-Item -LiteralPath (Get-GameDxgi $game)
    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:2206')
    Check ($r.Code -eq 0 -and $r.Text -match 'switches to standalone mode') 'the upgrade exits 0 and says it switches to standalone mode'
    Check (Test-SameBytes (Read-Bytes $ini) $original) 'the [PROXY] keys go back: ReShade.ini is byte-identical to before the first install'
    Check (-not (Test-Path -LiteralPath (Join-Path $game $ourDll))) 'the ReShade-mode ac-dlssg.dll is removed'
    Check ((Get-Sha (Get-GameDxgi $game)) -eq (Get-Sha $dllV1)) 'the bridge is now <game>\dxgi.dll'
    $m = Get-Manifest $game
    Check ($m.mode -eq 'standalone' -and $null -eq $m.reshade -and $m.dll.path -eq 'dxgi.dll') 'the manifest records standalone mode'
    Check ((Test-SpoofInstalled $game) -and (Get-SpoofRecord $m 'version.dll').origin -eq 'installed') 'the spoof stays, recorded as installed'
    Check ((Test-SlMatches $game $slReal) -and (Test-LuaMatches $game $luaReal)) 'Streamline and the Lua app stay'
    Check (-not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg\install\backup'))) 'no backup left'
    $r = Invoke-PackageUninstall $pkg $game -RemoveData
    $left = @(Get-ChildItem -LiteralPath $game -Force | ForEach-Object { $_.Name } | Sort-Object)
    Check ($r.Code -eq 0 -and ($left -join '|') -eq 'acs.exe|ReShade.ini' -and (Test-SameBytes (Read-Bytes $ini) $original)) "the uninstall leaves acs.exe and the original ReShade.ini ($($left -join ', '))"
}

Invoke-Case 'UP4: ReShade installed over the standalone bridge: the upgrade switches to ReShade mode' {
    $pkg = New-PackageCopy 'UP4'
    $game = New-FakeGame 'UP4' -NoDxgi
    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:2206')
    Check ($r.Code -eq 0 -and $r.Text -match 'mode: standalone') 'the first install exits 0 in standalone mode'
    Copy-Item -LiteralPath $fakeReShade -Destination (Get-GameDxgi $game) -Force
    $ini = Join-Path $game 'ReShade.ini'
    Write-Bytes $ini (Get-Utf8Bytes $simpleIni)
    $r = Invoke-PackageInstall $pkg $game (Get-SpoofArgs '10DE:2206')
    Check ($r.Code -eq 0 -and $r.Text -match 'switches to reshade mode') 'the upgrade exits 0 and says it switches to ReShade mode'
    Check ((Get-Sha (Get-GameDxgi $game)) -eq (Get-Sha $fakeReShade)) 'ReShade''s dxgi.dll is untouched'
    Check ((Get-Sha (Join-Path $game $ourDll)) -eq (Get-Sha $dllV1)) 'the bridge is <game>\ac-dlssg.dll'
    Check (Test-SameBytes (Read-Bytes $ini) (Get-Utf8Bytes "[PROXY]`r`nEnableProxyLibrary=1`r`nProxyLibrary=$ourDll`r`n")) 'ReShade.ini loads it'
    $m = Get-Manifest $game
    Check ($m.mode -eq 'reshade' -and $m.reshade -and $m.dll.path -eq $ourDll) 'the manifest records ReShade mode'
    Check (Test-SpoofInstalled $game) 'the spoof stays'
    $r = Invoke-PackageUninstall $pkg $game
    Check ($r.Code -eq 0 -and (Test-SameBytes (Read-Bytes $ini) (Get-Utf8Bytes $simpleIni))) 'the uninstall restores ReShade.ini'
    Check ((Get-Sha (Get-GameDxgi $game)) -eq (Get-Sha $fakeReShade) -and -not (Test-Path -LiteralPath (Join-Path $game $ourDll)) -and
        (Test-NoSpoofFiles $game)) 'ReShade stays; the bridge and the spoof are gone'
}

Invoke-Case 'UP5: files that are clearly not ours still stop the package install' {
    $pkg = New-PackageCopy 'UP5'
    $game = New-FakeGame 'UP5' -NoDxgi
    Copy-Item -LiteralPath $fakeOther -Destination (Get-GameDxgi $game)
    $r = Invoke-PackageInstall $pkg $game @('-NoSpoof')
    Check ((Test-Refused $r) -and $r.Text -match 'Not ReShade') 'a foreign dxgi.dll is refused'
    Check ((Get-Sha (Get-GameDxgi $game)) -eq (Get-Sha $fakeOther) -and -not (Test-Path -LiteralPath (Join-Path $game 'ac-dlssg'))) 'and nothing changed'
    $game = New-FakeGame 'UP5b' -NoDxgi
    Write-Text (Join-Path (Get-LuaDir $game) 'AcDlssg.lua') "-- someone else's app`r`n"
    $r = Invoke-PackageInstall $pkg $game @('-NoSpoof')
    Check ((Test-Refused $r) -and $r.Text -match 'apps\\lua\\AcDlssg') 'a foreign apps\lua\AcDlssg is refused'
}

# ---------------------------------------------------------------------------
# Administrator rights and the .bat launchers.

Invoke-Case 'EL1: arguments passed on to the elevated run keep spaces, trailing backslashes and Cyrillic' {
    $cyr = -join [char[]](0x0418, 0x0433, 0x0440, 0x044B)
    $bound = [ordered]@{ GameDir = "C:\Games\$cyr AC\"; NoSpoof = [System.Management.Automation.SwitchParameter]$true; NoPause = [System.Management.Automation.SwitchParameter]$false; GpuDeviceIds = @('8086:9A49', '10DE:25A0') }
    $list = @(Get-ForwardArguments $bound @{ Elevated = $true; GameDir = "D:\Steam Library\$cyr\" })
    Check (($list -join ' ') -eq "-GameDir D:\Steam Library\$cyr\ -NoSpoof -GpuDeviceIds 8086:9A49,10DE:25A0 -Elevated") "the forwarded arguments ($($list -join ' '))"
    $echo = Join-Path $FakeRoot 'el1\echo-args.ps1'
    Write-Text $echo "param([string]`$GameDir, [switch]`$NoSpoof, [string[]]`$GpuDeviceIds, [switch]`$Elevated)`r`n[IO.File]::WriteAllText(`$env:ACDB_ECHO, (@(`$GameDir, [bool]`$NoSpoof, (`$GpuDeviceIds -join ';'), [bool]`$Elevated) -join '|'), (New-Object Text.UTF8Encoding(`$false)))`r`n"
    $env:ACDB_ECHO = Join-Path $FakeRoot 'el1\echo.txt'
    try {
        $text = (@(@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $echo) + $list | ForEach-Object { ConvertTo-CommandLineArg $_ })) -join ' '
        $proc = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList $text -NoNewWindow -PassThru
        [void]$proc.WaitForExit(60000)
        $got = [IO.File]::ReadAllText($env:ACDB_ECHO, $utf8)
    } finally {
        Remove-Item Env:\ACDB_ECHO -ErrorAction SilentlyContinue
    }
    Check ($got -eq "D:\Steam Library\$cyr\|True|8086:9A49,10DE:25A0|True") "a child PowerShell reads them back ($got)"
}

Invoke-Case 'EL2: a game folder this account may not write into: install and uninstall refuse with -NoElevate' {
    $pkg = New-PackageCopy 'EL2'
    $game = New-FakeGame 'EL2' -NoDxgi
    $r = Invoke-PackageInstall $pkg $game @('-NoSpoof')
    Check ($r.Code -eq 0) 'install exits 0 while the folder is writable'
    $before = Get-TreeState $game
    $sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User
    $rights = [System.Security.AccessControl.FileSystemRights]'CreateFiles, CreateDirectories'
    $rule = New-Object System.Security.AccessControl.FileSystemAccessRule($sid, $rights, 'None', 'None', 'Deny')
    $acl = Get-Acl -LiteralPath $game
    $acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $game -AclObject $acl
    try {
        $r = Invoke-PackageInstall $pkg $game @('-NoSpoof')
        Check ((Test-Refused $r) -and $r.Text -match 'may not write into' -and $r.Text -match 'NoElevate') 'install refuses and names -NoElevate instead of asking for administrator rights'
        $r = Invoke-PackageUninstall $pkg $game
        Check ((Test-Refused $r) -and $r.Text -match 'may not change' -and $r.Text -match 'NoElevate') 'uninstall does the same'
        Check ((Get-TreeState $game) -eq $before) 'nothing changed in the game folder, and no probe file is left'
    } finally {
        $acl = Get-Acl -LiteralPath $game
        [void]$acl.RemoveAccessRule($rule)
        Set-Acl -LiteralPath $game -AclObject $acl
    }
    $r = Invoke-PackageUninstall $pkg $game -RemoveData
    Check ($r.Code -eq 0) 'uninstall exits 0 once the folder is writable'
}

Invoke-Case 'BAT: install.bat, uninstall.bat and collect-logs.bat from a folder with spaces and Cyrillic' {
    $p = Get-TestPackage
    $cyr = -join [char[]](0x043F, 0x0430, 0x043A, 0x0435, 0x0442)
    $dest = Join-Path $FakeRoot "packages\BAT $cyr & co\$(Split-Path -Leaf $p.Dir)"
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    foreach ($item in @(Get-ChildItem -LiteralPath (New-PackageCopy 'BAT-src') -Force)) { Copy-Item -LiteralPath $item.FullName -Destination $dest -Recurse }
    $game = New-FakeGame "BAT $cyr" -NoDxgi
    $r = Invoke-Bat (Join-Path $dest 'install.bat') (@('-GameDir', $game, '-StreamlineDir', $slReal, '-NoPause', '-NoElevate') + (Get-SpoofArgs '10DE:2206'))
    Check ($r.Code -eq 0) 'install.bat exits 0'
    Check ((Get-Sha (Get-GameDxgi $game)) -eq (Get-Sha $dllV1) -and (Test-SpoofInstalled $game) -and (Test-LuaMatches $game $luaReal)) 'the bridge, the spoof and the Lua app are installed'
    Check (Test-Path -LiteralPath (Join-Path (Get-StagedSpoofDir $dest) 'version.dll')) 'the download is staged in files\deps of that package'
    $r = Invoke-Bat (Join-Path $dest 'tools\collect-logs.bat') @('-GameDir', $game, '-AcDocsDir', (Join-Path $FakeRoot 'documents\none'), '-SkipSysinfo', '-NoPause')
    Check ($r.Code -eq 0 -and @(Get-ChildItem -LiteralPath (Join-Path $dest 'tools') -Filter 'ac-dlssg-logs-*.zip').Count -eq 1) 'collect-logs.bat writes its zip into tools\'
    $r = Invoke-Bat (Join-Path $dest 'tools\uninstall.bat') @('-GameDir', $game, '-RemoveData', '-NoPause', '-NoElevate')
    Check ($r.Code -eq 0 -and @(Get-ChildItem -LiteralPath $game -Force).Count -eq 1) 'uninstall.bat leaves only acs.exe'
    $r = Invoke-Bat (Join-Path $dest 'install.bat') @('-GameDir', (Join-Path $FakeRoot 'games\BAT none'), '-NoSpoof', '-NoPause', '-NoElevate')
    Check ($r.Code -ne 0 -and $r.Text -match 'REFUSED') 'install.bat passes the exit code of a refusal on'
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
    $defaultSteam = Join-Path ${env:ProgramFiles(x86)} 'Steam'
    Check (@(Get-SteamRoots | Where-Object { Test-SamePath $_ $defaultSteam }).Count -ge 1) 'Steam roots include the default C:\Program Files (x86)\Steam'

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
