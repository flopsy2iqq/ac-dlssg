<#
.SYNOPSIS
  Stages the pinned third-party files the build and the bridge need: the
  Streamline 2.14.1 SDK (headers, production DLLs, licenses) and the NVAPI
  headers.

.DESCRIPTION
  Streamline: downloads streamline-sdk-v2.14.1.zip from NVIDIA's GitHub
  release (or uses -StreamlineZip), verifies its size and SHA-256, and
  extracts into <DepsDir>\streamline-2.14.1:
    include\*.h                  the SDK headers
    bin\x64\<dll>                sl.interposer, sl.common, sl.dlss_g, sl.reflex,
                                 sl.pcl and nvngx_dlssg (production builds only,
                                 never bin\x64\development)
    license.txt, 3rd-party-licenses.md,
    bin\x64\nvngx_dlss.license.txt, bin\x64\reflex.license.txt
                                 the licenses that cover those DLLs, at their
                                 place in the zip
  and <DepsDir>\streamline-2.14.1.sha256, the SHA-256 of every extracted file.
  Each DLL must carry a valid Authenticode signature by NVIDIA Corporation.
  The downloaded zip is deleted afterwards unless -KeepZip is given; a zip
  passed with -StreamlineZip is never deleted.

  NVAPI: downloads the headers the bridge compiles against from a pinned
  commit of github.com/NVIDIA/nvapi into <DepsDir>\nvapi and verifies each
  file's git blob SHA-1.

  Spoof (only with -Only Spoof; the test package's install.ps1 asks for it on
  an RTX 30): downloads version.dll and dlssg_sm86.ini of dlssg_for_sm86
  0.3.5, the files of commit 9621db5 of github.com/sdli1995/dlssg_for_sm86
  (pins in dev-common.ps1), from raw.githubusercontent.com into
  <DepsDir>\dlssg_for_sm86-0.3.5. Each file is checked against its size and
  git blob SHA-1, version.dll also against its SHA-256, in <DepsDir>\download
  first; only when every requested file matches are they moved into place,
  and on any mismatch the downloads are deleted. Never the repository
  archive and nothing under alternatives/ (spec 10).

  Idempotent: files that are already present and verified are not downloaded
  again. Everything temporary (downloads, extraction staging, a replaced
  streamline-2.14.1.old-* folder) stays under <DepsDir>. A relative
  -DepsDir is relative to the current PowerShell location. Exit code 0 on
  success, 1 on any failure.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch-deps.ps1

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch-deps.ps1 -StreamlineZip D:\dl\streamline-sdk-v2.14.1.zip

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch-deps.ps1 -Only Nvapi -DepsDir build\deps

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch-deps.ps1 -Only Spoof -DepsDir build\deps
#>
param(
    # Where the files go; the default is deps\ at the repository root.
    [string]$DepsDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'deps'),
    # A local copy of streamline-sdk-v2.14.1.zip to use instead of downloading.
    [string]$StreamlineZip = '',
    # Keep the downloaded Streamline zip in <DepsDir>\download.
    [switch]$KeepZip,
    # Stage only these parts (default: Streamline and Nvapi; Spoof only when named).
    [ValidateSet('Streamline', 'Nvapi', 'Spoof')]
    [string[]]$Only = @('Streamline', 'Nvapi'),
    # Spoof: which of its files to stage (default: both).
    [ValidateSet('version.dll', 'dlssg_sm86.ini')]
    [string[]]$SpoofFiles = @('version.dll', 'dlssg_sm86.ini'),
    # Spoof, for tests only: copy the files from this folder instead of
    # downloading them; they go through the same checks.
    [string]$SpoofSourceDir = '',
    # Spoof, for tests only: a JSON file with the pins of those files (see
    # Get-SpoofPins in dev-common.ps1).
    [string]$SpoofPins = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
# Shared helpers: Get-GitBlobSha1, Get-NvidiaSignatureProblem and the
# dlssg_for_sm86 pins.
. (Join-Path $PSScriptRoot 'dev-common.ps1')

# --- Pinned sources -------------------------------------------------------

# The zip's size and SHA-256, and the SHA-256 of each file the install
# copies from its bin\x64, are pinned in dev-common.ps1.
$SlVersion = $script:AcdbSlVersion
$SlZipName = "streamline-sdk-v$SlVersion.zip"
$SlZipUrl = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$SlVersion/$SlZipName"
$SlZipSize = $script:AcdbSlZipSize
$SlZipSha256 = $script:AcdbSlZipSha256
$SlDlls = @('sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll')
# Paths in the zip, relative to the SDK root; bin/x64 ones stay in bin\x64.
$SlLicenses = @('license.txt', '3rd-party-licenses.md', 'bin/x64/nvngx_dlss.license.txt', 'bin/x64/reflex.license.txt')
$SlSignerCn = 'NVIDIA Corporation'

$NvapiCommit = '70d337db9186e968eab622f7e786de7e437faf3d'
$NvapiBaseUrl = "https://raw.githubusercontent.com/NVIDIA/nvapi/$NvapiCommit"
# File name -> git blob SHA-1 at $NvapiCommit. nvapi.h includes the
# nvapi_lite_* headers, so they are needed to compile it.
$NvapiFiles = [ordered]@{
    'License.txt'           = '34fa4289b1e2e1934b535f022d2b1ae51338791d'
    'NvApiDriverSettings.h' = 'eac9de10c66587b25fc06822d98f89a69544ae90'
    'nvapi.h'               = '4d42b0b5ed1221d87bf56d3189b79aa7df0a071d'
    'nvapi_interface.h'     = '93c4590ef44fcc7620bf830a1d1533f4ab94d5b6'
    'nvapi_lite_common.h'   = 'a70d464632c66b62bf7fa0cbbb92d0921f400925'
    'nvapi_lite_salstart.h' = 'c5099206f1964983cface88230041c855910a8d5'
    'nvapi_lite_salend.h'   = '0e4de0c3e6939f22007e77061118ff70edc09503'
    'nvapi_lite_sli.h'      = 'c55643ae20dcc35c5c6817dea561d2edb4a9337f'
    'nvapi_lite_surround.h' = 'c0686b004a3d878e1f2e6241d8354ac668ddde8f'
    'nvapi_lite_stereo.h'   = '7b6e642d07f75da220dbd3cd31532cb518be162e'
    'nvapi_lite_d3dext.h'   = '615f965f675036a982c13f4ec3ec7e9da43f70a7'
}

# --- Helpers ---------------------------------------------------------------

function Stop-Fetch([string]$Message) {
    throw (New-Object System.Exception $Message)
}

function Get-Sha256Hex([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Remove-IfPresent([string]$Path) {
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
}

function Remove-IfEmptyDir([string]$Path) {
    if ((Test-Path -LiteralPath $Path -PathType Container) -and
        -not (Get-ChildItem -LiteralPath $Path -Force | Select-Object -First 1)) {
        Remove-Item -LiteralPath $Path -Force
    }
}

# Downloads to <OutFile>.partial with TLS 1.2 and three attempts, then renames.
function Invoke-Download([string]$Url, [string]$OutFile) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $partial = "$OutFile.partial"
    $parent = Split-Path -Parent $OutFile
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    $lastError = $null
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            Remove-IfPresent $partial
            Invoke-WebRequest -Uri $Url -OutFile $partial -UseBasicParsing
            Remove-IfPresent $OutFile
            Move-Item -LiteralPath $partial -Destination $OutFile
            return
        } catch {
            $lastError = $_.Exception.Message
            Write-Host "  download attempt $attempt of 3 failed: $lastError"
            Remove-IfPresent $partial
            if ($attempt -lt 3) { Start-Sleep -Seconds (2 * $attempt) }
        }
    }
    Stop-Fetch "download of $Url failed: $lastError"
}

# --- NVAPI -----------------------------------------------------------------

function Invoke-NvapiFetch {
    $dir = Join-Path $DepsDir 'nvapi'
    $downloadDir = Join-Path $DepsDir 'download'
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $fetched = 0
    foreach ($name in $NvapiFiles.Keys) {
        $expected = $NvapiFiles[$name]
        $target = Join-Path $dir $name
        if ((Test-Path -LiteralPath $target -PathType Leaf) -and ((Get-GitBlobSha1 $target) -eq $expected)) {
            Write-Host "  nvapi\$name present and verified"
            continue
        }
        $temp = Join-Path $downloadDir $name
        Invoke-Download "$NvapiBaseUrl/$name" $temp
        $actual = Get-GitBlobSha1 $temp
        if ($actual -ne $expected) {
            Remove-IfPresent $temp
            Stop-Fetch "nvapi\$name has git blob SHA-1 $actual, expected $expected"
        }
        Remove-IfPresent $target
        Move-Item -LiteralPath $temp -Destination $target
        $fetched++
        Write-Host "  nvapi\$name downloaded and verified"
    }
    Remove-IfEmptyDir $downloadDir
    Write-Host "NVAPI: $($NvapiFiles.Count) files in $dir ($fetched downloaded)"
}

# --- Streamline ------------------------------------------------------------

# The manifest sits next to the folder, so the folder holds only SDK files.
function Get-StreamlineManifestPath([string]$Dir) { return "$Dir.sha256" }

# True when a previous run staged everything and nothing changed since.
function Test-StreamlineStaged([string]$Dir) {
    $manifest = Get-StreamlineManifestPath $Dir
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { return $false }
    $lines = @(Get-Content -LiteralPath $manifest)
    if ($lines.Count -lt 1 -or $lines[0] -ne "# zip $SlZipSha256") { return $false }
    $listed = @{}
    foreach ($line in ($lines | Select-Object -Skip 1)) {
        if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { return $false }
        $file = Join-Path $Dir $Matches[2]
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return $false }
        if ((Get-Sha256Hex $file) -ne $Matches[1]) { return $false }
        $listed[$Matches[2].ToLowerInvariant()] = $true
    }
    if (-not $listed.ContainsKey('include\sl.h')) { return $false }
    foreach ($license in $SlLicenses) {
        if (-not $listed.ContainsKey($license.Replace('/', '\').ToLowerInvariant())) { return $false }
    }
    foreach ($dll in $SlDlls) {
        $rel = "bin\x64\$dll"
        if (-not $listed.ContainsKey($rel.ToLowerInvariant())) { return $false }
        if (Get-NvidiaSignatureProblem (Join-Path $Dir $rel)) { return $false }
    }
    return $true
}

# Best effort: folders replaced by earlier runs; one still in use stays until
# a later run.
function Remove-OldStreamlineDirs([string]$Dir) {
    $parent = Split-Path -Parent $Dir
    $leaf = Split-Path -Leaf $Dir
    foreach ($o in @(Get-ChildItem -LiteralPath $parent -Directory -Filter "$leaf.old-*" -ErrorAction SilentlyContinue)) {
        try { Remove-Item -LiteralPath $o.FullName -Recurse -Force -ErrorAction Stop } catch { }
    }
}

function Invoke-StreamlineFetch {
    $dir = Join-Path $DepsDir "streamline-$SlVersion"
    if (Test-StreamlineStaged $dir) {
        Write-Host "Streamline: $dir present and verified"
        return
    }

    # Pick the zip: the caller's (never deleted), or ours in <DepsDir>\download.
    $ownZip = $false
    if ($StreamlineZip) {
        if (-not (Test-Path -LiteralPath $StreamlineZip -PathType Leaf)) { Stop-Fetch "-StreamlineZip not found: $StreamlineZip" }
        $zip = (Resolve-Path -LiteralPath $StreamlineZip).Path
        Write-Host "  using $zip"
    } else {
        $ownZip = $true
        $zip = Join-Path (Join-Path $DepsDir 'download') $SlZipName
        $reuse = (Test-Path -LiteralPath $zip -PathType Leaf) -and ((Get-Item -LiteralPath $zip).Length -eq $SlZipSize) -and
            ((Get-Sha256Hex $zip) -eq $SlZipSha256)
        if ($reuse) {
            Write-Host "  reusing $zip"
        } else {
            Write-Host "  downloading $SlZipUrl ($SlZipSize bytes)"
            Invoke-Download $SlZipUrl $zip
        }
    }

    $size = (Get-Item -LiteralPath $zip).Length
    $sha = Get-Sha256Hex $zip
    if ($size -ne $SlZipSize -or $sha -ne $SlZipSha256) {
        if ($ownZip) { Remove-IfPresent $zip }
        Stop-Fetch "$zip is $size bytes with SHA-256 $sha; expected $SlZipSize bytes with SHA-256 $SlZipSha256"
    }
    Write-Host "  $SlZipName verified (SHA-256 $sha)"

    $stage = Join-Path $DepsDir "streamline-$SlVersion.staging"
    Remove-IfPresent $stage
    try {
        Expand-StreamlineZip $zip $stage
        $problems = @()
        foreach ($dll in $SlDlls) {
            $problem = Get-NvidiaSignatureProblem (Join-Path $stage "bin\x64\$dll")
            if ($problem) { $problems += "$dll`: $problem" } else { Write-Host "  $dll signed by $SlSignerCn" }
        }
        if ($problems.Count -gt 0) { Stop-Fetch ("signature check failed: " + ($problems -join '; ')) }
        # The pinned zip holds exactly the pinned runtime files.
        foreach ($name in $script:AcdbSlRuntimeSha256.Keys) {
            $sha = Get-Sha256Hex (Join-Path $stage "bin\x64\$name")
            if ($sha -ne $script:AcdbSlRuntimeSha256[$name]) { $problems += "bin\x64\$name has SHA-256 $sha, pinned $($script:AcdbSlRuntimeSha256[$name])" }
        }
        if ($problems.Count -gt 0) { Stop-Fetch ("the extracted files differ from their pins in dev-common.ps1: " + ($problems -join '; ')) }

        $stageRoot = (Resolve-Path -LiteralPath $stage).Path.TrimEnd('\') + '\'
        $manifestLines = @("# zip $SlZipSha256")
        foreach ($f in @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName)) {
            $manifestLines += "$(Get-Sha256Hex $f.FullName)  $($f.FullName.Substring($stageRoot.Length))"
        }

        # Swap by renames only, so $dir is always either the old or the new
        # complete folder; deleting it first could stop half-way on a file in
        # use. A file held open without delete sharing makes the rename fail
        # before anything changed; a DLL mapped by a running process does not,
        # and its old folder is removed by a later run. The manifest is
        # written last, so it never describes a folder that is not there.
        $manifestPath = Get-StreamlineManifestPath $dir
        Remove-IfPresent $manifestPath
        Remove-OldStreamlineDirs $dir
        $old = $null
        if (Test-Path -LiteralPath $dir) {
            $old = "$dir.old-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
            Move-Item -LiteralPath $dir -Destination $old
        }
        try {
            Move-Item -LiteralPath $stage -Destination $dir
        } catch {
            if ($old) { Move-Item -LiteralPath $old -Destination $dir }
            throw
        }
        [System.IO.File]::WriteAllLines($manifestPath, [string[]]$manifestLines)
        Remove-OldStreamlineDirs $dir
    } finally {
        Remove-IfPresent $stage
    }

    if ($ownZip -and -not $KeepZip) {
        Remove-IfPresent $zip
        Remove-IfEmptyDir (Split-Path -Parent $zip)
    }
    Write-Host "Streamline: staged in $dir"
}

# Extracts include\*.h, the production DLLs and $SlLicenses into $Stage.
function Expand-StreamlineZip([string]$Zip, [string]$Stage) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    New-Item -ItemType Directory -Path (Join-Path $Stage 'include') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $Stage 'bin\x64') -Force | Out-Null
    $stageFull = [System.IO.Path]::GetFullPath($Stage).TrimEnd('\') + '\'

    $archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $entries = @($archive.Entries | ForEach-Object { [pscustomobject]@{ Entry = $_; Path = $_.FullName.Replace('\', '/') } })
        # The SDK may sit in a top-level folder; find it from the interposer.
        $root = $null
        foreach ($e in $entries) {
            if ($e.Path -match '^((?:[^/]+/)*?)bin/x64/sl\.interposer\.dll$') {
                if ($null -eq $root -or $Matches[1].Length -lt $root.Length) { $root = $Matches[1] }
            }
        }
        if ($null -eq $root) { Stop-Fetch 'the zip has no bin/x64/sl.interposer.dll' }

        $dllSet = @{}
        foreach ($dll in $SlDlls) { $dllSet[$dll.ToLowerInvariant()] = $true }
        $licenseSet = @{}
        foreach ($license in $SlLicenses) { $licenseSet[$license.ToLowerInvariant()] = $true }
        $extracted = @{}
        foreach ($e in $entries) {
            if (-not $e.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { continue }
            $rel = $e.Path.Substring($root.Length)
            $dest = $null
            if ($rel -match '^include/([^/]+\.h)$') {
                $dest = "include\$($Matches[1])"
            } elseif ($rel -match '^bin/x64/([^/]+)$' -and $dllSet.ContainsKey($Matches[1].ToLowerInvariant())) {
                $dest = "bin\x64\$($Matches[1])"
            } elseif ($licenseSet.ContainsKey($rel.ToLowerInvariant())) {
                $dest = $rel.Replace('/', '\')
            }
            if (-not $dest) { continue }
            $full = [System.IO.Path]::GetFullPath((Join-Path $Stage $dest))
            if (-not $full.StartsWith($stageFull, [StringComparison]::OrdinalIgnoreCase)) { Stop-Fetch "unsafe zip entry name: $($e.Path)" }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e.Entry, $full, $true)
            $extracted[$dest.ToLowerInvariant()] = $true
        }
    } finally {
        $archive.Dispose()
    }

    $missing = @($SlDlls | Where-Object { -not $extracted.ContainsKey("bin\x64\$_".ToLowerInvariant()) })
    $missing += @($SlLicenses | Where-Object { -not $extracted.ContainsKey($_.Replace('/', '\').ToLowerInvariant()) })
    if (-not $extracted.ContainsKey('include\sl.h')) { $missing += 'include\sl.h' }
    if ($missing.Count -gt 0) { Stop-Fetch ("the zip lacks: " + ($missing -join ', ')) }
    Write-Host "  extracted $($extracted.Count) files ($($SlLicenses.Count) license files)"
}

# --- dlssg_for_sm86 ---------------------------------------------------------

function Invoke-SpoofFetch {
    $pins = Get-SpoofPins $SpoofPins
    $dir = Join-Path $DepsDir $script:AcdbSpoofDirName
    $downloadDir = Join-Path $DepsDir 'download'
    $source = ''
    if ($SpoofSourceDir) { $source = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($SpoofSourceDir) }
    Write-Host ("  $($script:AcdbSpoofProject) $($script:AcdbSpoofVersion): $($SpoofFiles -join ', ') of commit " +
        "$($script:AcdbSpoofCommit) of github.com/$($script:AcdbSpoofRepository)")
    $todo = @()
    foreach ($name in $SpoofFiles) {
        $target = Join-Path $dir $name
        if (Test-Path -LiteralPath $target -PathType Leaf) {
            $problem = Get-SpoofFileProblem $target $pins[$name]
            if (-not $problem) {
                Write-Host "  $name present and verified"
                continue
            }
            Write-Host "  $target does not match its pins ($problem); deleted, fetching it again"
            Remove-IfPresent $target
        }
        $todo += $name
    }
    # Everything fetched stays in <DepsDir>\download until all of it is verified.
    $pending = @()
    try {
        foreach ($name in $todo) {
            $temp = Join-Path $downloadDir $name
            $pending += $temp
            if ($source) {
                $from = Join-Path $source $name
                Write-Host "  copying $from (-SpoofSourceDir, no download)"
                if (-not (Test-Path -LiteralPath $from -PathType Leaf)) { Stop-Fetch "-SpoofSourceDir has no $name" }
                New-Item -ItemType Directory -Path $downloadDir -Force | Out-Null
                Copy-Item -LiteralPath $from -Destination $temp -Force
            } else {
                $size = ''
                if ($null -ne $pins[$name].size) { $size = " ($($pins[$name].size) bytes)" }
                Write-Host "  downloading $(Get-SpoofUrl $name)$size"
                Invoke-Download (Get-SpoofUrl $name) $temp
            }
            $problem = Get-SpoofFileProblem $temp $pins[$name]
            if ($problem) { Stop-Fetch "the downloaded $problem; the download was deleted" }
            $what = "git blob SHA-1 $($pins[$name].gitSha1)"
            if ($pins[$name].sha256) { $what += ", SHA-256 $($pins[$name].sha256)" }
            Write-Host "  $name verified ($what)"
        }
        if ($todo.Count -gt 0) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        foreach ($name in $todo) { Move-Item -LiteralPath (Join-Path $downloadDir $name) -Destination (Join-Path $dir $name) }
        $pending = @()
    } finally {
        foreach ($f in $pending) {
            Remove-IfPresent $f
            Remove-IfPresent "$f.partial"
        }
        Remove-IfEmptyDir $downloadDir
    }
    Write-Host "$($script:AcdbSpoofProject): $($SpoofFiles.Count) files in $dir ($($todo.Count) fetched)"
}

# --- Main ------------------------------------------------------------------

try {
    # Relative to the PowerShell location, as for every other path argument
    # ([IO.Path]::GetFullPath would use the process directory, which
    # Set-Location does not change).
    $DepsDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($DepsDir)
    New-Item -ItemType Directory -Path $DepsDir -Force | Out-Null
    Write-Host "fetch-deps: $DepsDir"
    if ($Only -contains 'Nvapi') { Invoke-NvapiFetch }
    if ($Only -contains 'Streamline') { Invoke-StreamlineFetch }
    if ($Only -contains 'Spoof') { Invoke-SpoofFetch }
    Write-Host 'fetch-deps: OK'
    exit 0
} catch {
    Write-Host "fetch-deps: FAILED: $($_.Exception.Message)"
    exit 1
}
