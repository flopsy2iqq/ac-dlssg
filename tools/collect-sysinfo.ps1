<#
.SYNOPSIS
  Collects the facts ac-dlssg needs about a PC or laptop into one text report.

.DESCRIPTION
  Read-only: nothing on the system is changed and nothing is sent anywhere.
  The report (ac-dlssg-sysinfo.txt) and the DirectX diagnostic it is built
  from (dxdiag.txt) are written into a folder "ac-dlssg-sysinfo" next to this
  script. Send ac-dlssg-sysinfo.txt to whoever asked for it.

  It records: computer model, BIOS, Windows version, CPU, every GPU (driver,
  hybrid-graphics role, hardware-accelerated GPU scheduling, dedicated
  memory), displays (panel, mode, which GPU drives them), power source,
  nvidia-smi output, and Assetto Corsa's CSP, ReShade and video settings.
  No administrator rights are needed.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File collect-sysinfo.ps1
#>
$ErrorActionPreference = 'Continue'
$out = Join-Path $PSScriptRoot 'ac-dlssg-sysinfo'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$report = New-Object System.Collections.Generic.List[string]
function Add([string]$line) { $report.Add($line); Write-Host $line }
function Section([string]$name) { Add ''; Add "=== $name" }

Add "ac-dlssg system report, $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

Section 'Computer'
$cs = Get-CimInstance Win32_ComputerSystem
$csp = Get-CimInstance Win32_ComputerSystemProduct
$bios = Get-CimInstance Win32_BIOS
Add "manufacturer: $($cs.Manufacturer)"
Add "model: $($cs.Model)  (product: $($csp.Name), SKU: $($cs.SystemSKUNumber))"
Add "BIOS: $($bios.SMBIOSBIOSVersion), $($bios.ReleaseDate)"
Add "RAM: $([math]::Round($cs.TotalPhysicalMemory / 1GB, 1)) GB"

Section 'Windows'
$os = Get-CimInstance Win32_OperatingSystem
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
Add "$($os.Caption) $($cv.DisplayVersion), build $($os.BuildNumber).$($cv.UBR)"

Section 'CPU'
Get-CimInstance Win32_Processor | ForEach-Object { Add "$($_.Name.Trim())" }

Section 'Power'
$bat = Get-CimInstance Win32_Battery
if ($bat) {
    $status = @{ 1 = 'on battery'; 2 = 'on AC power'; 3 = 'fully charged'; 6 = 'charging' }[[int]$bat.BatteryStatus]
    Add "battery: $($bat.EstimatedChargeRemaining)%, status $($bat.BatteryStatus) ($status)"
} else {
    Add 'no battery (desktop)'
}

Section 'GPUs (WMI)'
Get-CimInstance Win32_VideoController | ForEach-Object {
    $ids = if ($_.PNPDeviceID -match 'VEN_([0-9A-F]{4})&DEV_([0-9A-F]{4})') { "vendor 0x$($Matches[1]) device 0x$($Matches[2])" } else { $_.PNPDeviceID }
    Add "$($_.Name): driver $($_.DriverVersion) ($($_.DriverDate)), $ids, mode $($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution) @ $($_.CurrentRefreshRate) Hz"
}

Section 'Hardware-accelerated GPU scheduling (registry)'
try {
    $hw = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' -Name HwSchMode -ErrorAction Stop).HwSchMode
    Add "HwSchMode = $hw (2 = on, 1 = off)"
} catch {
    Add 'HwSchMode not set (off, or not offered on this system)'
}

Section 'nvidia-smi'
$smi = Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'
if (Test-Path $smi) {
    & $smi --query-gpu=name,driver_version,vbios_version,pci.device_id,memory.total,power.default_limit,power.max_limit --format=csv 2>&1 | ForEach-Object { Add "$_" }
} else {
    Add 'nvidia-smi not found'
}

Section 'DirectX diagnostic (display devices)'
$dx = Join-Path $out 'dxdiag.txt'
if (Test-Path $dx) { [IO.File]::Delete($dx) }
Write-Host '(running dxdiag, this takes up to a minute)'
Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\dxdiag.exe') -ArgumentList "/t `"$dx`"" -Wait -WindowStyle Hidden
for ($i = 0; $i -lt 60 -and -not (Test-Path $dx); $i++) { Start-Sleep -Seconds 1 }
if (Test-Path $dx) {
    $keys = 'Card name', 'Manufacturer', 'Chip type', 'Dedicated Memory', 'Current Mode', 'Native Mode', 'Monitor Name',
            'Monitor Model', 'Output Type', 'Monitor Capabilities', 'Advanced Color', 'Hybrid Graphics GPU', 'Driver Version',
            'Driver Model', 'Hardware Scheduling', 'Graphics Preemption', 'Miracast', 'MPO Caps', 'Detachable'
    $inDisplay = $false
    foreach ($line in Get-Content $dx) {
        if ($line -match '^\s*-+\s*$') { continue }
        if ($line -match '^(Display Devices|Render Devices)') { $inDisplay = $true; Add "--- $($line.Trim())"; continue }
        if ($line -match '^(Sound Devices|Video Capture Devices|DirectInput Devices|System Devices)') { $inDisplay = $false; continue }
        if (-not $inDisplay) { continue }
        foreach ($k in $keys) {
            if ($line -match "^\s*$([regex]::Escape($k)):\s*(.*)$") { Add ("  {0}: {1}" -f $k, $Matches[1].Trim()); break }
        }
        if ($line -match '^\s*Card name:') { }
    }
} else {
    Add 'dxdiag did not produce a report'
}

Section 'Assetto Corsa'
$steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
$acDir = $null
if ($steam) {
    $libs = @($steam -replace '/', '\')
    $vdf = Join-Path $libs[0] 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) { Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $libs += ($_.Matches[0].Groups[1].Value -replace '\\\\', '\') } }
    foreach ($l in ($libs | Select-Object -Unique)) {
        $cand = Join-Path $l 'steamapps\common\assettocorsa'
        if (Test-Path (Join-Path $cand 'acs.exe')) { $acDir = $cand; break }
    }
}
if (-not $acDir) {
    Add 'Assetto Corsa not found through Steam'
} else {
    Add "game folder: $acDir"
    foreach ($f in 'dwrite.dll', 'dxgi.dll', 'version.dll', 'ac-dlssg.dll') {
        $p = Join-Path $acDir $f
        if (Test-Path $p) {
            $v = (Get-Item $p).VersionInfo
            Add ("  {0}: {1} {2} ({3})" -f $f, $v.ProductName, $v.ProductVersion, $v.FileDescription)
        } else {
            Add "  ${f}: not present"
        }
    }
    $rs = Join-Path $acDir 'ReShade.ini'
    if (Test-Path $rs) { Get-Content $rs | Select-String -Pattern '^(EnableProxyLibrary|ProxyLibrary|DisabledAddons)=' | ForEach-Object { Add "  ReShade.ini $($_.Line)" } }
    Get-ChildItem $acDir -Filter '*.addon64' -ErrorAction SilentlyContinue | ForEach-Object { Add "  ReShade add-on: $($_.Name)" }
}
$docs = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Assetto Corsa\cfg'
$video = Join-Path $docs 'video.ini'
if (Test-Path $video) {
    Get-Content $video | Select-String -Pattern '^(WIDTH|HEIGHT|REFRESH|FULLSCREEN|VSYNC|AASAMPLES|FPS_CAP_MS|MODE|FILTER|WORLD_DETAIL|SHADOW_MAP_SIZE)=' | ForEach-Object { Add "  video.ini $($_.Line)" }
} else {
    Add "  no $video"
}
$ga = Join-Path $docs 'extension\graphics_adjustments.ini'
if (Test-Path $ga) { Get-Content $ga | Select-String -Pattern '^(ACTIVE|OLD_IMPLEMENTATION|QUALITY_DLSS|DLSS_PRESET)=' | ForEach-Object { Add "  graphics_adjustments.ini $($_.Line)" } }
$dt = Join-Path $docs 'extension\dxgi_tweaks.ini'
if (Test-Path $dt) { Get-Content $dt | Select-String -Pattern '^[A-Z_]+=' | ForEach-Object { Add "  dxgi_tweaks.ini $($_.Line)" } }

$file = Join-Path $out 'ac-dlssg-sysinfo.txt'
[IO.File]::WriteAllLines($file, $report, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ''
Write-Host "Report written to $file"
