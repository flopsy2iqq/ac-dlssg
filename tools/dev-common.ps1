<#
.SYNOPSIS
  Helpers shared by dev-install.ps1 and dev-uninstall.ps1 (dot-sourced).

.DESCRIPTION
  Defines functions only; it changes nothing when dot-sourced.

  ReShade.ini is edited at the byte level. Each byte is mapped to one char
  (ISO-8859-1), so lines that are not touched are written back exactly as they
  were, whatever their encoding. Values this project writes are encoded as
  UTF-8 first.
#>

$script:AcdbDllName = 'ac-dlssg.dll'
$script:AcdbDataDirName = 'ac-dlssg'
# DLLs the bridge may import (spec 6.1, 12): present in System32 on every
# Windows 10/11 x64 installation. Never VERSION.dll (the spoof's name), the VC
# runtime, d3d11/d3d12/dxgi or d3dcompiler.
$script:AcdbImportAllowList = @('KERNEL32.dll', 'USER32.dll', 'ADVAPI32.dll', 'SHELL32.dll', 'ole32.dll')
# The Streamline runtime the bridge loads from <game>\ac-dlssg\sl (spec 6.3,
# 12); each DLL must carry NVIDIA's Authenticode signature.
$script:AcdbSlDirName = 'sl'
$script:AcdbSlDlls = @('sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll')
$script:AcdbSlSignerCn = 'NVIDIA Corporation'
$script:AcdbGameExe = 'acs.exe'
$script:AcdbSteamAppId = '244210'
$script:Latin1 = [System.Text.Encoding]::GetEncoding(28591)
$script:Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$script:IniTrimChars = [char[]]@([char]32, [char]9)

# A refusal is an expected stop with a message for the user; anything else is
# reported as an unexpected error with its script position.
function Stop-Refused([string]$Message) {
    $ex = New-Object System.Exception $Message
    $ex.Data['acdb.refusal'] = $true
    throw $ex
}

function Test-IsRefusal($ErrorRecord) {
    return [bool]$ErrorRecord.Exception.Data['acdb.refusal']
}

function Get-Sha256OfFile([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Get-Sha256OfBytes([byte[]]$Bytes) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($Bytes)) -replace '-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

# Empty when the file carries a valid Authenticode signature whose signer's
# common name is NVIDIA Corporation; otherwise the reason.
function Get-NvidiaSignatureProblem([string]$Path) {
    $sig = Get-AuthenticodeSignature -LiteralPath $Path
    if ($sig.Status -ne 'Valid') { return "signature status $($sig.Status) ($($sig.StatusMessage))" }
    if (-not $sig.SignerCertificate) { return 'no signer certificate' }
    $cn = $sig.SignerCertificate.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
    if ($cn -ne $script:AcdbSlSignerCn) { return "signed by '$cn', not '$($script:AcdbSlSignerCn)'" }
    return ''
}

function Get-NormalizedPath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    if ($full.Length -gt 3) { $full = $full.TrimEnd('\', '/') }
    return $full
}

function Test-SamePath([string]$A, [string]$B) {
    return [string]::Equals((Get-NormalizedPath $A), (Get-NormalizedPath $B), [System.StringComparison]::OrdinalIgnoreCase)
}

# Resolves a path written in a ReShade setting relative to $BaseDir. Relative
# names are joined to $BaseDir, as std::filesystem::path operator/ does.
function Resolve-PathSetting([string]$BaseDir, [string]$Value, [string]$What) {
    if ($Value.Contains('%')) {
        Stop-Refused "$What '$Value' contains an environment variable, which this script does not expand. Use a plain path."
    }
    if ($Value -match '^[A-Za-z]:[\\/]' -or $Value -match '^[\\/]{2}') {
        return Get-NormalizedPath $Value
    }
    if ($Value -match '^[\\/]') {
        return Get-NormalizedPath ([System.IO.Path]::GetPathRoot((Get-NormalizedPath $BaseDir)).TrimEnd('\') + $Value)
    }
    if ($Value -match '^[A-Za-z]:') {
        Stop-Refused "$What '$Value' is relative to the current directory of drive $($Value.Substring(0, 2)), which this script does not support. Use an absolute path."
    }
    return Get-NormalizedPath (Join-Path $BaseDir $Value)
}

# True when the error, or any exception inside it, is an access-denied
# failure: writing into a game folder under Program Files without elevation.
function Test-AccessDenied($ErrorRecord) {
    if ($ErrorRecord.CategoryInfo -and $ErrorRecord.CategoryInfo.Category -eq 'PermissionDenied') { return $true }
    $e = $ErrorRecord.Exception
    while ($e) {
        if ($e -is [System.UnauthorizedAccessException]) { return $true }
        if ($e -is [System.IO.IOException] -and ($e.HResult -band 0xFFFF) -eq 5) { return $true }
        $e = $e.InnerException
    }
    return $false
}

function Write-AccessDeniedHint([string]$GameDir) {
    Write-Host '  Windows denied access to the game folder. When Assetto Corsa is under C:\Program Files (x86),'
    Write-Host '  only an elevated PowerShell may change it: open the Start menu, right-click Windows PowerShell,'
    Write-Host '  choose "Run as administrator", and run the same command again there.'
    if ($GameDir) { Write-Host "  (game folder: $GameDir)" }
}

# ReShade's dxgi.dll is recognised by its version resource ProductName.
function Get-DxgiProductName([string]$Path) {
    try {
        $name = (Get-Item -LiteralPath $Path).VersionInfo.ProductName
        if ($name) { return $name.Trim() }
    } catch { }
    return ''
}

function Get-RunningGame {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($script:AcdbGameExe)
    return @(Get-Process -Name $name -ErrorAction SilentlyContinue)
}

# ---------------------------------------------------------------------------
# Steam discovery

# The Steam folders from the registry, then Steam's default folder
# (C:\Program Files (x86)\Steam) in case the registry has none.
function Get-SteamRoots {
    $roots = @()
    foreach ($key in @(
            @{ Path = 'HKCU:\Software\Valve\Steam'; Name = 'SteamPath' },
            @{ Path = 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam'; Name = 'InstallPath' },
            @{ Path = 'HKLM:\SOFTWARE\Valve\Steam'; Name = 'InstallPath' })) {
        try {
            $value = (Get-ItemProperty -LiteralPath $key.Path -Name $key.Name -ErrorAction Stop).($key.Name)
            if ($value) { $roots += ($value -replace '/', '\') }
        } catch {
            continue
        }
    }
    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    if ($programFilesX86) {
        $default = Join-Path $programFilesX86 'Steam'
        if (-not @($roots | Where-Object { Test-SamePath $_ $default }).Count) { $roots += $default }
    }
    return $roots
}

# Library folders listed in steamapps\libraryfolders.vdf, in both the current
# format ("path" "D:\\SteamLibrary") and the pre-2021 one ("1" "D:\\SteamLibrary").
function Get-SteamLibraries([string]$SteamRoot) {
    $libraries = @($SteamRoot)
    $vdf = Join-Path $SteamRoot 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf -PathType Leaf) {
        $text = [System.IO.File]::ReadAllText($vdf)
        $pattern = '(?m)^\s*"(?:path|\d+)"\s+"((?:[^"\\]|\\.)*)"\s*$'
        foreach ($m in [regex]::Matches($text, $pattern)) {
            $libraries += ($m.Groups[1].Value -replace '\\(.)', '$1')
        }
    }
    return $libraries
}

function Get-AppInstallDirName([string]$Library) {
    $acf = Join-Path $Library "steamapps\appmanifest_$($script:AcdbSteamAppId).acf"
    if (Test-Path -LiteralPath $acf -PathType Leaf) {
        $m = [regex]::Match([System.IO.File]::ReadAllText($acf), '(?m)^\s*"installdir"\s+"((?:[^"\\]|\\.)*)"')
        if ($m.Success -and $m.Groups[1].Value) { return ($m.Groups[1].Value -replace '\\(.)', '$1') }
    }
    return 'assettocorsa'
}

# With -SteamPath only that Steam folder is searched; otherwise the Steam
# folders from the registry. Returns $null when acs.exe is not found.
function Find-AssettoCorsaDir([string]$SteamPath) {
    $roots = if ($SteamPath) { @($SteamPath) } else { @(Get-SteamRoots) }
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        foreach ($library in @(Get-SteamLibraries $root)) {
            $candidate = Join-Path $library ('steamapps\common\' + (Get-AppInstallDirName $library))
            if (Test-Path -LiteralPath (Join-Path $candidate $script:AcdbGameExe) -PathType Leaf) {
                return Get-NormalizedPath $candidate
            }
        }
    }
    return $null
}

function Resolve-GameDir([string]$GameDir) {
    if (-not $GameDir) {
        $found = Find-AssettoCorsaDir
        if (-not $found) {
            Stop-Refused 'Assetto Corsa was not found through Steam''s libraryfolders.vdf. Pass -GameDir <folder that contains acs.exe>.'
        }
        return $found
    }
    if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) {
        Stop-Refused "-GameDir '$GameDir' is not a folder."
    }
    $full = Get-NormalizedPath (Resolve-Path -LiteralPath $GameDir).ProviderPath
    if (-not (Test-Path -LiteralPath (Join-Path $full $script:AcdbGameExe) -PathType Leaf)) {
        Stop-Refused "'$full' has no $($script:AcdbGameExe); it is not the Assetto Corsa folder."
    }
    return $full
}

# ---------------------------------------------------------------------------
# PE checks for the bridge DLL

function Get-PeInfo([string]$Path) {
    $b = [System.IO.File]::ReadAllBytes($Path)
    if ($b.Length -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { throw 'no MZ header' }
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ($pe -lt 0 -or $pe + 24 -gt $b.Length -or $b[$pe] -ne 0x50 -or $b[$pe + 1] -ne 0x45 -or $b[$pe + 2] -ne 0 -or $b[$pe + 3] -ne 0) {
        throw 'no PE signature'
    }
    $machine = [BitConverter]::ToUInt16($b, $pe + 4)
    $sectionCount = [BitConverter]::ToUInt16($b, $pe + 6)
    $optSize = [BitConverter]::ToUInt16($b, $pe + 20)
    $characteristics = [BitConverter]::ToUInt16($b, $pe + 22)
    $opt = $pe + 24
    $magic = [BitConverter]::ToUInt16($b, $opt)
    if ($magic -eq 0x20B) {
        $dirCountOff = $opt + 108; $dirOff = $opt + 112
        $imageBase = [BitConverter]::ToUInt64($b, $opt + 24)
    } elseif ($magic -eq 0x10B) {
        $dirCountOff = $opt + 92; $dirOff = $opt + 96
        $imageBase = [uint64][BitConverter]::ToUInt32($b, $opt + 28)
    } else {
        throw ('unknown optional header magic 0x{0:X}' -f $magic)
    }
    $sections = @()
    $s = $opt + $optSize
    for ($i = 0; $i -lt $sectionCount; $i++) {
        $o = $s + 40 * $i
        $sections += [pscustomobject]@{
            VirtualSize    = [BitConverter]::ToUInt32($b, $o + 8)
            VirtualAddress = [BitConverter]::ToUInt32($b, $o + 12)
            RawSize        = [BitConverter]::ToUInt32($b, $o + 16)
            RawPointer     = [BitConverter]::ToUInt32($b, $o + 20)
        }
    }
    $toOffset = {
        param([uint32]$Rva)
        foreach ($sec in $sections) {
            $size = [Math]::Max([uint32]$sec.VirtualSize, [uint32]$sec.RawSize)
            if ($Rva -ge $sec.VirtualAddress -and $Rva -lt $sec.VirtualAddress + $size) {
                return [int]($Rva - $sec.VirtualAddress + $sec.RawPointer)
            }
        }
        throw ('RVA 0x{0:X} is outside every section' -f $Rva)
    }
    $readName = {
        param([uint32]$Rva)
        $start = & $toOffset $Rva
        $end = $start
        while ($end -lt $b.Length -and $b[$end] -ne 0 -and $end - $start -lt 1024) { $end++ }
        return [System.Text.Encoding]::ASCII.GetString($b, $start, $end - $start)
    }
    $dirCount = [BitConverter]::ToUInt32($b, $dirCountOff)
    $dirRva = {
        param([int]$Index)
        if ($dirCount -le $Index) { return [uint32]0 }
        return [BitConverter]::ToUInt32($b, $dirOff + 8 * $Index)
    }

    $exports = @()
    $exportRva = & $dirRva 0
    if ($exportRva -ne 0) {
        $e = & $toOffset $exportRva
        $nameCount = [BitConverter]::ToUInt32($b, $e + 24)
        if ($nameCount -gt 65536) { throw 'implausible export count' }
        if ($nameCount -gt 0) {
            $names = & $toOffset ([BitConverter]::ToUInt32($b, $e + 32))
            for ($i = 0; $i -lt $nameCount; $i++) {
                $exports += & $readName ([BitConverter]::ToUInt32($b, $names + 4 * $i))
            }
        }
    }

    # IMAGE_IMPORT_DESCRIPTOR (20 bytes, Name at +12) up to an all-zero entry.
    $imports = @()
    $importRva = & $dirRva 1
    if ($importRva -ne 0) {
        $d = & $toOffset $importRva
        for ($i = 0; $i -lt 4096; $i++) {
            if ($d + 20 -gt $b.Length) { throw 'import directory runs past the end of the file' }
            $nameRva = [BitConverter]::ToUInt32($b, $d + 12)
            if ($nameRva -eq 0) { break }
            $imports += & $readName $nameRva
            $d += 20
        }
    }

    # IMAGE_DELAYLOAD_DESCRIPTOR (32 bytes, DllNameRVA at +4) up to DllNameRVA 0.
    # Attributes bit 0 clear is the old format, which stores addresses, not RVAs.
    $delayImports = @()
    $delayRva = & $dirRva 13
    if ($delayRva -ne 0) {
        $d = & $toOffset $delayRva
        for ($i = 0; $i -lt 4096; $i++) {
            if ($d + 32 -gt $b.Length) { throw 'delay-import directory runs past the end of the file' }
            $name = [uint64][BitConverter]::ToUInt32($b, $d + 4)
            if ($name -eq 0) { break }
            if (([BitConverter]::ToUInt32($b, $d) -band 1) -eq 0) { $name -= $imageBase }
            $delayImports += & $readName ([uint32]$name)
            $d += 32
        }
    }

    return [pscustomobject]@{
        Machine      = $machine
        IsDll        = ($characteristics -band 0x2000) -ne 0
        Exports      = $exports
        Imports      = $imports
        DelayImports = $delayImports
    }
}

# Problems that would make ReShade crash or misbehave with this DLL as its
# ProxyLibrary. An empty result means the DLL is acceptable.
function Test-BridgeDll([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @("file not found: $Path") }
    try {
        $info = Get-PeInfo $Path
    } catch {
        return @("not a valid PE file ($($_.Exception.Message))")
    }
    $problems = @()
    if ($info.Machine -ne 0x8664) { $problems += ('not an x64 image (machine 0x{0:X})' -f $info.Machine) }
    if (-not $info.IsDll) { $problems += 'not a DLL' }
    if ($info.Exports -notcontains 'CreateDXGIFactory1') {
        $problems += 'does not export CreateDXGIFactory1; ReShade would call a null function and the game would crash'
    }
    foreach ($name in @('ReShadeVersion', 'ReShadeRegisterAddon', 'ReShadeUnregisterAddon')) {
        if ($info.Exports -contains $name) { $problems += "exports $name, which breaks ReShade when this DLL is its ProxyLibrary" }
    }
    # -notcontains compares case-insensitively, like the loader.
    foreach ($name in @($info.Imports)) {
        if ($script:AcdbImportAllowList -notcontains $name) {
            $problems += "imports $name, which is outside the allow-list ($($script:AcdbImportAllowList -join ', ')); if it failed to load, ReShade would crash the game at start"
        }
    }
    foreach ($name in @($info.DelayImports)) {
        if ($script:AcdbImportAllowList -notcontains $name) {
            $problems += "delay-imports $name, which is outside the allow-list ($($script:AcdbImportAllowList -join ', '))"
        }
    }
    return $problems
}

# ---------------------------------------------------------------------------
# INI line model

function ConvertTo-RawText([string]$Text) { return $script:Latin1.GetString($script:Utf8NoBom.GetBytes($Text)) }
function ConvertFrom-RawText([string]$Raw) { return $script:Utf8NoBom.GetString($script:Latin1.GetBytes($Raw)) }

function ConvertFrom-IniBytes([byte[]]$Bytes) {
    $bom = $Bytes.Length -ge 3 -and $Bytes[0] -eq 0xEF -and $Bytes[1] -eq 0xBB -and $Bytes[2] -eq 0xBF
    $start = 0
    if ($bom) { $start = 3 }
    $text = $script:Latin1.GetString($Bytes, $start, $Bytes.Length - $start)
    $lines = New-Object System.Collections.ArrayList
    $pos = 0
    while ($pos -lt $text.Length) {
        $nl = $text.IndexOf([char]10, $pos)
        if ($nl -lt 0) {
            [void]$lines.Add([pscustomobject]@{ Text = $text.Substring($pos); Eol = '' })
            break
        }
        $end = $nl
        $eol = "`n"
        if ($end -gt $pos -and $text[$end - 1] -eq [char]13) {
            $end--
            $eol = "`r`n"
        }
        [void]$lines.Add([pscustomobject]@{ Text = $text.Substring($pos, $end - $pos); Eol = $eol })
        $pos = $nl + 1
    }
    return [pscustomobject]@{ Bom = $bom; Lines = $lines }
}

function ConvertTo-IniBytes($Doc) {
    $sb = New-Object System.Text.StringBuilder
    foreach ($line in $Doc.Lines) { [void]$sb.Append($line.Text).Append($line.Eol) }
    $body = $script:Latin1.GetBytes($sb.ToString())
    if (-not $Doc.Bom) { return , $body }
    $all = New-Object byte[] ($body.Length + 3)
    $all[0] = 0xEF; $all[1] = 0xBB; $all[2] = 0xBF
    [Array]::Copy($body, 0, $all, 3, $body.Length)
    return , $all
}

function Read-IniDoc([string]$Path) { return ConvertFrom-IniBytes ([System.IO.File]::ReadAllBytes($Path)) }

function Get-IniLineInfo([string]$Text) {
    $t = $Text.Trim($script:IniTrimChars)
    if ($t.Length -eq 0) { return [pscustomobject]@{ Kind = 'blank'; Name = $null; Key = $null; Value = $null } }
    if ($t[0] -eq '[') {
        $close = $t.IndexOf(']')
        $name = if ($close -gt 0) { $t.Substring(1, $close - 1) } else { $t.Substring(1) }
        return [pscustomobject]@{ Kind = 'section'; Name = $name.Trim($script:IniTrimChars); Key = $null; Value = $null }
    }
    if ($t[0] -eq ';' -or $t[0] -eq '#' -or $t.StartsWith('//')) {
        return [pscustomobject]@{ Kind = 'comment'; Name = $null; Key = $null; Value = $null }
    }
    $eq = $t.IndexOf('=')
    if ($eq -lt 0) { return [pscustomobject]@{ Kind = 'other'; Name = $null; Key = $null; Value = $null } }
    return [pscustomobject]@{
        Kind  = 'key'
        Name  = $null
        Key   = $t.Substring(0, $eq).Trim($script:IniTrimChars)
        Value = $t.Substring($eq + 1).Trim($script:IniTrimChars)
    }
}

# Every block of the named section (case-insensitive): Header is the index of
# the "[NAME]" line, End the index of the next section header or the line count.
function Get-IniSections($Doc, [string]$Name) {
    $blocks = @()
    $current = $null
    for ($i = 0; $i -lt $Doc.Lines.Count; $i++) {
        $info = Get-IniLineInfo $Doc.Lines[$i].Text
        if ($info.Kind -ne 'section') { continue }
        if ($current) { $current.End = $i; $blocks += $current; $current = $null }
        if ([string]::Equals($info.Name, $Name, [System.StringComparison]::OrdinalIgnoreCase)) {
            $current = [pscustomobject]@{ Header = $i; End = $Doc.Lines.Count }
        }
    }
    if ($current) { $blocks += $current }
    return $blocks
}

function Find-IniKeyLines($Doc, $Block, [string]$Key) {
    $found = @()
    for ($i = $Block.Header + 1; $i -lt $Block.End; $i++) {
        $info = Get-IniLineInfo $Doc.Lines[$i].Text
        if ($info.Kind -eq 'key' -and [string]::Equals($info.Key, $Key, [System.StringComparison]::OrdinalIgnoreCase)) {
            $found += $i
        }
    }
    return $found
}

# Decoded value of the key line at $Index, with surrounding double quotes removed.
function Get-IniValueAt($Doc, [int]$Index) {
    $value = ConvertFrom-RawText (Get-IniLineInfo $Doc.Lines[$Index].Text).Value
    if ($value.Length -ge 2 -and $value.StartsWith('"') -and $value.EndsWith('"')) { $value = $value.Substring(1, $value.Length - 2) }
    return $value
}

# First value of section/key, like GetPrivateProfileString; $null when absent.
function Get-IniFirstValue($Doc, [string]$Section, [string]$Key) {
    foreach ($block in @(Get-IniSections $Doc $Section)) {
        $lines = @(Find-IniKeyLines $Doc $block $Key)
        if ($lines.Count -gt 0) { return Get-IniValueAt $Doc $lines[0] }
    }
    return $null
}

function Get-DominantEol($Doc) {
    $crlf = @($Doc.Lines | Where-Object { $_.Eol -eq "`r`n" }).Count
    $lf = @($Doc.Lines | Where-Object { $_.Eol -eq "`n" }).Count
    if ($lf -gt $crlf) { return "`n" }
    return "`r`n"
}

# Inserts lines before $Index. A file that ended without a line break still
# does afterwards: the previous last line gets the break and the new last line
# goes without one. Remove-IniLine undoes exactly this.
function Add-IniLines($Doc, [int]$Index, [string[]]$Texts) {
    $eol = Get-DominantEol $Doc
    $atEnd = $Index -eq $Doc.Lines.Count
    $noFinalBreak = $atEnd -and $Index -gt 0 -and $Doc.Lines[$Index - 1].Eol -eq ''
    if ($noFinalBreak) { $Doc.Lines[$Index - 1].Eol = $eol }
    for ($i = 0; $i -lt $Texts.Count; $i++) {
        $lineEol = $eol
        if ($noFinalBreak -and $i -eq $Texts.Count - 1) { $lineEol = '' }
        $Doc.Lines.Insert($Index + $i, [pscustomobject]@{ Text = $Texts[$i]; Eol = $lineEol })
    }
}

function Remove-IniLine($Doc, [int]$Index) {
    $line = $Doc.Lines[$Index]
    $Doc.Lines.RemoveAt($Index)
    if ($line.Eol -eq '' -and $Index -gt 0 -and $Index -eq $Doc.Lines.Count) { $Doc.Lines[$Index - 1].Eol = '' }
}

# ---------------------------------------------------------------------------
# ReShade specifics

function Test-NamesOurDll([string]$Value) {
    if (-not $Value) { return $false }
    $leaf = ($Value -split '[\\/]')[-1].Trim()
    return [string]::Equals($leaf, $script:AcdbDllName, [System.StringComparison]::OrdinalIgnoreCase)
}

# Anything but an empty value, 0 or false counts as enabled; this errs on the
# side of "ReShade loads the proxy".
function Test-EnabledValue([string]$Value) {
    if ($null -eq $Value) { return $false }
    $v = $Value.Trim()
    return -not ($v -eq '' -or $v -eq '0' -or $v -ieq 'false')
}

# ReShade 6.8.0's base path for a dxgi.dll in the game folder: [INSTALL]
# BasePath from <game>\ReShade.ini (relative to the game folder), else the
# RESHADE_BASE_PATH_OVERRIDE environment variable, else the game folder.
function Resolve-ReShadeBase([string]$GameDir) {
    $gameIni = Join-Path $GameDir 'ReShade.ini'
    if (Test-Path -LiteralPath $gameIni -PathType Leaf) {
        $basePath = Get-IniFirstValue (Read-IniDoc $gameIni) 'INSTALL' 'BasePath'
        if ($basePath) {
            $dir = Resolve-PathSetting $GameDir $basePath "[INSTALL] BasePath in $gameIni"
            if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
                Stop-Refused "[INSTALL] BasePath in $gameIni points to '$dir', which is not a folder. Fix BasePath first."
            }
            return [pscustomobject]@{ BasePath = $dir; Source = "[INSTALL] BasePath=$basePath in $gameIni"; Ini = Join-Path $dir 'ReShade.ini' }
        }
    }
    $override = [Environment]::GetEnvironmentVariable('RESHADE_BASE_PATH_OVERRIDE')
    if ($override) {
        $dir = Resolve-PathSetting $GameDir $override 'RESHADE_BASE_PATH_OVERRIDE'
        if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
            Stop-Refused "RESHADE_BASE_PATH_OVERRIDE points to '$dir', which is not a folder. Fix or clear the variable first."
        }
        return [pscustomobject]@{ BasePath = $dir; Source = "RESHADE_BASE_PATH_OVERRIDE=$override"; Ini = Join-Path $dir 'ReShade.ini' }
    }
    return [pscustomobject]@{ BasePath = $GameDir; Source = 'game folder'; Ini = Join-Path $GameDir 'ReShade.ini' }
}

# Every EnableProxyLibrary and ProxyLibrary value in every [PROXY] block.
function Get-ProxyKeyValues($Doc) {
    $enable = @()
    $proxy = @()
    foreach ($block in @(Get-IniSections $Doc 'PROXY')) {
        foreach ($i in @(Find-IniKeyLines $Doc $block 'EnableProxyLibrary')) { $enable += Get-IniValueAt $Doc $i }
        foreach ($i in @(Find-IniKeyLines $Doc $block 'ProxyLibrary')) { $proxy += Get-IniValueAt $Doc $i }
    }
    return [pscustomobject]@{ Enable = $enable; Proxy = $proxy }
}

# Conservative: true when any EnableProxyLibrary is on and any ProxyLibrary
# names our DLL, whichever duplicate ReShade would actually use.
function Test-DocLoadsOurDll($Doc) {
    $v = Get-ProxyKeyValues $Doc
    $enabled = @($v.Enable | Where-Object { Test-EnabledValue $_ }).Count -gt 0
    $ours = @($v.Proxy | Where-Object { Test-NamesOurDll $_ }).Count -gt 0
    return $enabled -and $ours
}

function Format-ProxyKeys($Doc) {
    $v = Get-ProxyKeyValues $Doc
    $e = if (@($v.Enable).Count) { ($v.Enable | ForEach-Object { "'$_'" }) -join ', ' } else { '(missing)' }
    $p = if (@($v.Proxy).Count) { ($v.Proxy | ForEach-Object { "'$_'" }) -join ', ' } else { '(missing)' }
    return "EnableProxyLibrary=$e ProxyLibrary=$p"
}

# ---------------------------------------------------------------------------
# File writes

function Move-OverTarget([string]$Temp, [string]$Target) {
    if (Test-Path -LiteralPath $Target -PathType Leaf) {
        # A plain $null would reach .NET as "" (an illegal path); no backup file.
        [System.IO.File]::Replace($Temp, $Target, [NullString]::Value)
    } else {
        [System.IO.File]::Move($Temp, $Target)
    }
}

# Writes <target>.new, checks its hash, then renames it over the target.
function Write-BytesViaTemp([string]$Target, [byte[]]$Bytes) {
    $temp = "$Target.new"
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force }
    [System.IO.File]::WriteAllBytes($temp, $Bytes)
    $expected = Get-Sha256OfBytes $Bytes
    if ((Get-Sha256OfFile $temp) -ne $expected) {
        Remove-Item -LiteralPath $temp -Force
        throw "hash check of $temp failed"
    }
    Move-OverTarget $temp $Target
    return $expected
}

function Copy-FileViaTemp([string]$Source, [string]$Target) {
    $temp = "$Target.new"
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force }
    Copy-Item -LiteralPath $Source -Destination $temp -Force
    $expected = Get-Sha256OfFile $Source
    if ((Get-Sha256OfFile $temp) -ne $expected) {
        Remove-Item -LiteralPath $temp -Force
        throw "hash check of $temp failed"
    }
    Move-OverTarget $temp $Target
    return $expected
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text, $script:Utf8NoBom)
}

# The id git gives a file's content: SHA-1 over "blob <size>\0" + bytes.
function Get-GitBlobSha1([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $header = [System.Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)`0")
    $sha = [System.Security.Cryptography.SHA1]::Create()
    try {
        [void]$sha.TransformBlock($header, 0, $header.Length, $null, 0)
        [void]$sha.TransformFinalBlock($bytes, 0, $bytes.Length)
        return ([BitConverter]::ToString($sha.Hash) -replace '-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

# ---------------------------------------------------------------------------
# GPU architecture from the PCI device ID: a port of src/gpu_info.cpp, which
# tools\test-dev-scripts.ps1 compares this table with. Device IDs from the
# PCI ID Repository, version 2026.09.25; each range spans the lowest to the
# highest GPU-function ID of the listed chips.

$script:AcdbNvidiaVendorId = 0x10DE
# GA100 (A100) is SM80; every other Ampere chip is SM86.
$script:AcdbGa100First = 0x2080
$script:AcdbGa100Last = 0x20FF
# The lowest Turing ID (TU102, TITAN RTX). NVIDIA IDs below it are older chips.
$script:AcdbFirstTuringId = 0x1E02
$script:AcdbGpuRanges = @(
    [pscustomobject]@{ First = 0x1E02; Last = 0x1FF9; Arch = 'Turing' }     # TU102, TU104, TU106, TU117
    [pscustomobject]@{ First = 0x2080; Last = 0x20FF; Arch = 'Ampere' }     # GA100
    [pscustomobject]@{ First = 0x2182; Last = 0x21D1; Arch = 'Turing' }     # TU116
    [pscustomobject]@{ First = 0x2200; Last = 0x223F; Arch = 'Ampere' }     # GA102
    [pscustomobject]@{ First = 0x2414; Last = 0x25FB; Arch = 'Ampere' }     # GA103, GA104, GA106, GA107
    [pscustomobject]@{ First = 0x2681; Last = 0x28F8; Arch = 'Ada' }        # AD102, AD103, AD104, AD106, AD107
    [pscustomobject]@{ First = 0x2B85; Last = 0x2DF9; Arch = 'Blackwell' }  # GB202, GB203, GB206, GB207
    [pscustomobject]@{ First = 0x2F04; Last = 0x2F58; Arch = 'Blackwell' }  # GB205
)

# NonNvidia, OlderNvidia, Turing, Ampere, Ada, Blackwell or Unknown (an
# NVIDIA ID above Turing that the table does not know).
function Get-GpuArch([int]$VendorId, [int]$DeviceId) {
    if ($VendorId -ne $script:AcdbNvidiaVendorId) { return 'NonNvidia' }
    foreach ($r in $script:AcdbGpuRanges) {
        if ($DeviceId -ge $r.First -and $DeviceId -le $r.Last) { return $r.Arch }
    }
    if ($DeviceId -lt $script:AcdbFirstTuringId) { return 'OlderNvidia' }
    return 'Unknown'
}

# An SM86 Ampere chip (GA102 to GA107, desktop and laptop): the GPUs
# dlssg_for_sm86 is for.
function Test-AmpereSm86([int]$VendorId, [int]$DeviceId) {
    return (Get-GpuArch $VendorId $DeviceId) -eq 'Ampere' -and
        ($DeviceId -lt $script:AcdbGa100First -or $DeviceId -gt $script:AcdbGa100Last)
}

# The vendor and device ID in a PNPDeviceID such as
# PCI\VEN_10DE&DEV_2206&SUBSYS_38971462&REV_A1\...; $null without them.
function ConvertFrom-PnpDeviceId([string]$PnpDeviceId) {
    $m = [regex]::Match("$PnpDeviceId", '(?i)VEN_([0-9A-F]{4})&DEV_([0-9A-F]{4})')
    if (-not $m.Success) { return $null }
    return [pscustomobject]@{
        VendorId = [Convert]::ToInt32($m.Groups[1].Value, 16)
        DeviceId = [Convert]::ToInt32($m.Groups[2].Value, 16)
    }
}

function New-GpuAdapter([int]$VendorId, [int]$DeviceId, [string]$Name) {
    return [pscustomobject]@{
        VendorId = $VendorId
        DeviceId = $DeviceId
        Id       = '{0:X4}:{1:X4}' -f $VendorId, $DeviceId
        Name     = $Name
        Arch     = Get-GpuArch $VendorId $DeviceId
        Sm86     = [bool](Test-AmpereSm86 $VendorId $DeviceId)
    }
}

# Every display adapter, from the PNPDeviceID of each Win32_VideoController.
# $DeviceIds ('VVVV:DDDD' PCI IDs in hex, one per adapter; "a,b" counts as
# two) replaces that query, for tests on a PC whose own GPU does not matter.
function Get-GpuAdapters([string[]]$DeviceIds) {
    $adapters = @()
    $ids = @($DeviceIds | ForEach-Object { "$_" -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if ($ids.Count -gt 0) {
        foreach ($id in $ids) {
            if ($id -notmatch '^([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})$') {
                Stop-Refused "-GpuDeviceIds '$id' is not a PCI vendor:device ID such as 10DE:2206."
            }
            $adapters += New-GpuAdapter ([Convert]::ToInt32($Matches[1], 16)) ([Convert]::ToInt32($Matches[2], 16)) 'an adapter from -GpuDeviceIds'
        }
        return $adapters
    }
    foreach ($vc in @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop)) {
        $pci = ConvertFrom-PnpDeviceId ([string]$vc.PNPDeviceID)
        if ($pci) { $adapters += New-GpuAdapter $pci.VendorId $pci.DeviceId ([string]$vc.Name) }
    }
    return $adapters
}

# ---------------------------------------------------------------------------
# dlssg_for_sm86 (spec 10): exactly two files of commit 9621db5 of
# sdli1995/dlssg_for_sm86, the files of its tag 0.3.5. The GitHub contents
# API lists both as plain git blobs at that commit (version.dll 30021920
# bytes, not a Git LFS pointer; the repository has no .gitattributes), so
# raw.githubusercontent.com/<repository>/<commit>/<name> serves them. Never
# the repository archive, which holds archive/0.1.0/version.dll (flagged by
# Defender), and nothing under alternatives/.

$script:AcdbSpoofProject = 'dlssg_for_sm86'
$script:AcdbSpoofVersion = '0.3.5'
$script:AcdbSpoofRepository = 'sdli1995/dlssg_for_sm86'
$script:AcdbSpoofCommit = '9621db573e07ed54f50c15bbb585ed9a7bdfac28'
$script:AcdbSpoofNames = @('version.dll', 'dlssg_sm86.ini')
# The folder under deps\ that fetch-deps.ps1 stages the files in.
$script:AcdbSpoofDirName = "dlssg_for_sm86-$($script:AcdbSpoofVersion)"

function Get-SpoofUrl([string]$Name) {
    return "https://raw.githubusercontent.com/$($script:AcdbSpoofRepository)/$($script:AcdbSpoofCommit)/$Name"
}

# Name -> { gitSha1, sha256 ($null: not pinned), size ($null: not pinned) }.
# $PinsFile, for tests only, gives other pins for local fixture files as JSON:
# {"version.dll": {"gitSha1": "...", "sha256": "...", "size": 123}, ...}.
function Get-SpoofPins([string]$PinsFile) {
    $pins = [ordered]@{}
    if (-not $PinsFile) {
        $pins['version.dll'] = [pscustomobject]@{
            gitSha1 = 'efd92261f2b74e0a0fb927d74bce7a1c0c2413f7'
            sha256  = 'c3934a09399f022504227c72df0bf8c0de55f9a08880dddde898c5262cefa838'
            size    = [long]30021920
        }
        $pins['dlssg_sm86.ini'] = [pscustomobject]@{ gitSha1 = '2c97d64f2239b7d511f7d0a36c16e149dd3329f6'; sha256 = $null; size = [long]3548 }
        return $pins
    }
    if (-not (Test-Path -LiteralPath $PinsFile -PathType Leaf)) { Stop-Refused "-SpoofPins file not found: $PinsFile" }
    $json = [System.IO.File]::ReadAllText($PinsFile) | ConvertFrom-Json
    foreach ($name in $script:AcdbSpoofNames) {
        $p = $json.PSObject.Properties[$name]
        if (-not $p -or -not $p.Value.PSObject.Properties['gitSha1'] -or -not $p.Value.gitSha1) {
            Stop-Refused "-SpoofPins $PinsFile has no gitSha1 for $name."
        }
        $v = $p.Value
        $sha256 = $null
        if ($v.PSObject.Properties['sha256'] -and $v.sha256) { $sha256 = ([string]$v.sha256).ToLowerInvariant() }
        $size = $null
        if ($v.PSObject.Properties['size'] -and $null -ne $v.size) { $size = [long]$v.size }
        $pins[$name] = [pscustomobject]@{ gitSha1 = ([string]$v.gitSha1).ToLowerInvariant(); sha256 = $sha256; size = $size }
    }
    return $pins
}

# Empty when the file matches its pins (size, git blob SHA-1, SHA-256);
# otherwise the first mismatch.
function Get-SpoofFileProblem([string]$Path, $Pin) {
    $name = Split-Path -Leaf $Path
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "$name is missing" }
    $size = (Get-Item -LiteralPath $Path).Length
    if ($null -ne $Pin.size -and $size -ne $Pin.size) { return "$name is $size bytes, expected $($Pin.size) bytes" }
    $git = Get-GitBlobSha1 $Path
    if ($git -ne $Pin.gitSha1) { return "$name has git blob SHA-1 $git, expected $($Pin.gitSha1)" }
    if ($Pin.sha256) {
        $sha = Get-Sha256OfFile $Path
        if ($sha -ne $Pin.sha256) { return "$name has SHA-256 $sha, expected $($Pin.sha256)" }
    }
    return ''
}
