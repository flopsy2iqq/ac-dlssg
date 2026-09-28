<#
.SYNOPSIS
  Deletes the D3D shader-cache folders that this work tree's test programs left
  in %LOCALAPPDATA%\D3DSCache.

.DESCRIPTION
  The Direct3D runtime creates %LOCALAPPDATA%\D3DSCache\<id> for every
  executable path that creates a device, and neither TEMP nor LOCALAPPDATA
  moves it. Each folder's .val file names the executable. This script deletes
  exactly the folders whose .val file names a path under -Root or under an
  -ExtraRoot. The default -Root is the parent of this script's tools\ folder:
  the root of the repository or work tree, which holds its build folders. It
  never reaches further up, where other projects may live. Folders of other
  programs are never touched. A folder still in use by a running test program
  cannot be deleted; it is reported and left alone.

  The NVIDIA driver's shared %LOCALAPPDATA%\NVIDIA\DXCache files cannot be
  attributed to a program and are left alone.

  Exit code 0, also when nothing was found; 1 only when a folder that belongs
  to this work tree could not be deleted.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\clean-shader-cache.ps1
#>
param(
    # The tree root: tools\ sits directly below it.
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string[]]$ExtraRoot = @()
)

$ErrorActionPreference = 'Stop'

$cache = Join-Path $env:LOCALAPPDATA 'D3DSCache'
if (-not (Test-Path -LiteralPath $cache -PathType Container)) {
    Write-Host "clean-shader-cache: no $cache"
    exit 0
}

$roots = @($Root) + @($ExtraRoot) | Where-Object { $_ } | ForEach-Object {
    [IO.Path]::GetFullPath($_).TrimEnd('\') + '\'
}
Write-Host "clean-shader-cache: removing D3DSCache folders of programs under: $($roots -join '; ')"

$removed = 0
$failed = 0
foreach ($dir in @(Get-ChildItem -LiteralPath $cache -Directory -Force)) {
    $owner = $null
    foreach ($val in @(Get-ChildItem -LiteralPath $dir.FullName -Filter '*.val' -File -Force -ErrorAction SilentlyContinue)) {
        try {
            $text = [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($val.FullName))
        } catch {
            continue
        }
        foreach ($r in $roots) {
            $at = $text.IndexOf($r, [StringComparison]::OrdinalIgnoreCase)
            if ($at -ge 0) {
                $end = $text.IndexOf('.exe', $at, [StringComparison]::OrdinalIgnoreCase)
                $owner = if ($end -gt $at) { $text.Substring($at, $end + 4 - $at) } else { $r }
                break
            }
        }
        if ($owner) { break }
    }
    if (-not $owner) { continue }
    try {
        Remove-Item -LiteralPath $dir.FullName -Recurse -Force
        $removed++
        Write-Host "  removed $($dir.Name) ($owner)"
    } catch {
        $failed++
        Write-Host "  could not remove $($dir.Name) ($owner): $($_.Exception.Message)"
    }
}
Write-Host "clean-shader-cache: $removed removed, $failed left"
exit $(if ($failed -gt 0) { 1 } else { 0 })
