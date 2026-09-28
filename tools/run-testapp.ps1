<#
.SYNOPSIS
  Runs the test application scenarios (spec 11) and prints a PASS/FAIL table.

.DESCRIPTION
  Runs build\tools\testapp\<Config>\testapp.exe once per scenario, in
  sequence, each with a timeout. The test app loads ac-dlssg.dll from
  its own directory the way ReShade loads its ProxyLibrary; the build copies
  the DLL and the fixtures there, and the staged Streamline DLLs to
  ac-dlssg\sl, the layout of <game>\ac-dlssg\sl.

  M2: a proxy presents through Streamline's proxy chain with Reflex and the
  PCL markers; the test app checks that in the bridge log (--expect-proxy).
  Streamline allows one lifetime per process and the first proxy's final
  Release shuts it down, so with --recreate the second chain is a pass-through
  with the reason "Streamline already shut down".

  For the default scenario the script also prints the bridge's "stats:" lines
  and checks the GPU budget of success criterion 3 (spec 2): the average of
  bridge_gpu_ms d3d11 + d3d12 over those lines must not exceed 0.5 ms. The
  default scenario presents 6000 frames rather than the test app's 600, so
  that the uncapped run (1000-2200 fps through the bridge on the reference
  machine) lasts well over the bridge's one-second statistics period. The Bridge ms column shows the same average for every scenario that
  logged statistics.

  For every run the test app's output and the bridge log are kept in
  <BuildDir>\testapp-logs\<Config>\<scenario>.out.txt and
  <scenario>.bridge.log. The test app runs with TEMP and TMP pointing at
  <BuildDir>\tmp, so that nothing it or the driver stack writes lands outside
  the build tree; the D3D shader-cache folders that the runtime creates in
  %LOCALAPPDATA%\D3DSCache regardless are removed at the end
  (clean-shader-cache.ps1). The exit code is the number of failed scenarios;
  a skipped scenario is not a failure.

  The test app's window stays hidden (--hidden): DXGI still paces and
  presents its chains, but nothing appears on screen. -Visible shows it
  instead, without taking the focus, for a check of the real on-screen path.

  The reshade scenarios run the production chain: testapp.exe -> ReShade's
  dxgi.dll -> [PROXY] ProxyLibrary=ac-dlssg.dll. They run in their own folder,
  <BuildDir>\testapp-reshade\<Config>, which holds testapp.exe, ac-dlssg.dll,
  the fixtures, a copy of ReShade's dxgi.dll and a generated ReShade.ini
  (proxy library on, no effect or texture search paths, the effect cache in
  that folder, tutorial done), the Streamline DLLs in ac-dlssg\sl; no add-ons
  or shaders. There testapp.exe loads
  ReShade at process start through d3d11.dll's dxgi.dll import, as acs.exe
  does, and --via-dxgi makes it take its factory from ReShade. Besides the
  test app's own checks, the run folder's ReShade.log must show that ReShade
  loaded ac-dlssg.dll as its export module, saw exactly the test app's swap
  chains (not the bridge's D3D12 or hidden chains) and initialised one effect
  runtime on each, re-created after each ResizeBuffers on the wrapper whose
  address the test app printed, and must not show "Failed to create back
  buffer render targets!" or "Skipping swap chain". ReShade.log is kept as
  <scenario>.reshade.log next to the other logs. ReShade checks
  api.github.com for updates once per process, synchronously, while it sets up
  its runtime on the new swap chain; a slow network therefore delays the swap
  chain's creation by seconds, but not the frames.

  The standalone scenarios run the bridge without ReShade, installed as the
  game's dxgi.dll. They run in <BuildDir>\testapp-standalone\<Config>, which
  holds testapp.exe, the bridge copied as dxgi.dll (and no ac-dlssg.dll), the
  fixtures, the Streamline DLLs in ac-dlssg\sl and ac-dlssg\ac-dlssg.ini with
  the defaults dev-install.ps1 writes (log_level=debug). With --standalone the
  test app never loads the bridge by path: d3d11.dll's dxgi.dll import binds
  it at process start, the app's factory comes from LoadLibraryW(L"dxgi.dll")
  by name, and the app checks that d3d11.dll's and Streamline's DXGI imports
  are bound to the bridge. They pass the same log checks as the default
  scenario, and the bridge log must say "mode: standalone" and read that
  ac-dlssg.ini.

  -ReShadeDll names ReShade's dxgi.dll. By default it is the dxgi.dll in the
  Assetto Corsa folder found through Steam's libraryfolders.vdf; it is only
  read and copied. A DLL whose version resource ProductName is not "ReShade",
  or no DLL at all, makes the reshade scenarios SKIP rather than fail.

  -Scenario runs only the named scenarios.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\run-testapp.ps1 -Config Debug,Release
#>
param(
    [string[]]$Config = @('Debug'),
    [string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build'),
    [int]$TimeoutSeconds = 120,
    [double]$BudgetMs = 0.5,
    [string]$ReShadeDll = '',
    [string[]]$Scenario = @(),
    [switch]$Visible
)

$ErrorActionPreference = 'Stop'
$invariant = [System.Globalization.CultureInfo]::InvariantCulture
# Steam discovery (Find-AssettoCorsaDir); dot-sourcing it only defines functions.
. (Join-Path $PSScriptRoot 'dev-common.ps1')

# "powershell -File" passes "-Config Debug,Release" as one string.
$Config = @($Config | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$Scenario = @($Scenario | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

$scenarios = @(
    @{ Name = 'default';      Args = @('--frames', '6000', '--expect-proxy'); Budget = $true },
    @{ Name = 'vsync';        Args = @('--vsync', '--expect-proxy') },
    @{ Name = 'resize';       Args = @('--resize', '--expect-proxy') },
    @{ Name = 'test-present'; Args = @('--test-present', '--expect-proxy') },
    # The second chain passes through with "Streamline already shut down"
    # (see the description), as in every scenario with --recreate.
    @{ Name = 'recreate';     Args = @('--recreate', '--expect-proxy') },
    @{ Name = 'stall';        Args = @('--stall', '--expect-proxy') },
    # Since M2 the re-created chain passes through ("Streamline already shut
    # down"), so only the first chain has the debug stall (its 30th frame) and
    # the resize back at frame 400 goes to DXGI's chain. A resize that falls
    # into a stall is deferred (resize_pending in d3d12_presenter.cpp); the
    # test app notes it when the log shows one.
    @{ Name = 'stall-resize'; Args = @('--stall', '--resize', '--recreate', '--expect-proxy') },
    # The refusal path, with every chain operation, as a baseline against real DXGI.
    @{ Name = 'passthrough';  Args = @('--fixture', 'passthrough', '--expect-passthrough', '--resize', '--recreate',
                                       '--test-present') },
    @{ Name = 'combined';     Args = @('--vsync', '--resize', '--recreate', '--test-present', '--expect-proxy') },
    # The production chain through ReShade (see the description). More frames
    # than the default, so that the run lasts past the bridge's first
    # statistics second and ReShade's first second of overlay messages.
    @{ Name = 'reshade';        Args = @('--via-dxgi', '--frames', '3000', '--expect-proxy'); ReShade = $true },
    @{ Name = 'reshade-resize'; Args = @('--via-dxgi', '--resize', '--test-present', '--expect-proxy'); ReShade = $true;
       Resizes = 2 },
    # The bridge as the game's dxgi.dll, without ReShade (see the description).
    @{ Name = 'standalone';          Args = @('--standalone', '--frames', '3000', '--expect-proxy'); Standalone = $true },
    @{ Name = 'standalone-combined'; Args = @('--standalone', '--vsync', '--resize', '--recreate', '--test-present',
                                              '--expect-proxy'); Standalone = $true }
)
if ($Scenario.Count) {
    $unknown = @($Scenario | Where-Object { $n = $_; -not ($scenarios | Where-Object { $_.Name -eq $n }) })
    if ($unknown.Count) { throw "unknown scenario(s): $($unknown -join ', ')" }
    $scenarios = @($scenarios | Where-Object { $Scenario -contains $_.Name })
}

function Format-Ms([double]$value) { $value.ToString('0.000', $invariant) }

# ReShade's dxgi.dll for the reshade scenarios: -ReShadeDll, else the one in
# the Assetto Corsa folder. Path is $null, with the reason, when there is none.
function Resolve-ReShadeDll([string]$Requested) {
    $path = $Requested
    if (-not $path) {
        $game = Find-AssettoCorsaDir
        if (-not $game) {
            return [pscustomobject]@{ Path = $null; Version = ''
                Reason = 'Assetto Corsa not found through Steam''s libraryfolders.vdf; pass -ReShadeDll' }
        }
        $path = Join-Path $game 'dxgi.dll'
    }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        return [pscustomobject]@{ Path = $null; Version = ''; Reason = "no ReShade dxgi.dll at $path" }
    }
    $path = (Resolve-Path -LiteralPath $path).ProviderPath
    $vi = (Get-Item -LiteralPath $path).VersionInfo
    if (-not $vi.ProductName -or $vi.ProductName.Trim() -ne 'ReShade') {
        return [pscustomobject]@{ Path = $null; Version = ''
            Reason = "$path is not ReShade (version resource ProductName '$($vi.ProductName)')" }
    }
    return [pscustomobject]@{ Path = $path; Version = $vi.FileVersion; Reason = '' }
}

# The run folder of the reshade scenarios. Existing files are replaced, and
# nothing else in the folder is deleted.
function Initialize-ReShadeRunDir([string]$RunDir, [string]$TestAppDir, [string]$ReShadePath) {
    New-Item -ItemType Directory -Force -Path $RunDir | Out-Null
    foreach ($file in @('testapp.exe', 'ac-dlssg.dll')) {
        Copy-Item -LiteralPath (Join-Path $TestAppDir $file) -Destination (Join-Path $RunDir $file) -Force
    }
    Copy-Item -Path (Join-Path $PSScriptRoot 'testapp\fixtures\*') -Destination $RunDir -Recurse -Force
    # The Streamline DLLs the build staged next to testapp.exe.
    $slDir = Join-Path $RunDir 'ac-dlssg\sl'
    New-Item -ItemType Directory -Force -Path $slDir | Out-Null
    Copy-Item -Path (Join-Path $TestAppDir 'ac-dlssg\sl\*.dll') -Destination $slDir -Force
    Copy-Item -LiteralPath $ReShadePath -Destination (Join-Path $RunDir 'dxgi.dll') -Force
    New-Item -ItemType Directory -Force -Path (Join-Path $RunDir 'reshade-cache') | Out-Null
}

# The run folder of the standalone scenarios: the bridge is dxgi.dll there.
# Existing files are replaced; an ac-dlssg.dll left from elsewhere is removed,
# because the bridge must only be reachable as dxgi.dll.
function Initialize-StandaloneRunDir([string]$RunDir, [string]$TestAppDir) {
    New-Item -ItemType Directory -Force -Path $RunDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $TestAppDir 'testapp.exe') -Destination (Join-Path $RunDir 'testapp.exe') -Force
    Copy-Item -LiteralPath (Join-Path $TestAppDir 'ac-dlssg.dll') -Destination (Join-Path $RunDir 'dxgi.dll') -Force
    Remove-Item -LiteralPath (Join-Path $RunDir 'ac-dlssg.dll') -Force -ErrorAction SilentlyContinue
    Copy-Item -Path (Join-Path $PSScriptRoot 'testapp\fixtures\*') -Destination $RunDir -Recurse -Force
    $slDir = Join-Path $RunDir 'ac-dlssg\sl'
    New-Item -ItemType Directory -Force -Path $slDir | Out-Null
    Copy-Item -Path (Join-Path $TestAppDir 'ac-dlssg\sl\*.dll') -Destination $slDir -Force
    # The defaults of dev-install.ps1's ac-dlssg.ini.
    $ini = @('; ac-dlssg settings for the standalone test scenarios.', '[bridge]', 'enabled=1', 'start_with_fg=1',
        'hotkey=ctrl+f10', 'log_level=debug', '')
    [System.IO.File]::WriteAllText((Join-Path $RunDir 'ac-dlssg\ac-dlssg.ini'), ($ini -join "`r`n"), $utf8NoBom)
}

# Bridge log checks of a standalone scenario on top of the test app's own.
function Test-StandaloneLog([string]$LogPath, [string]$RunDir) {
    $problems = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) {
        $problems.Add('no bridge log')
        return $problems
    }
    $lines = @([System.IO.File]::ReadAllLines($LogPath))
    $bridge = [regex]::Escape((Join-Path $RunDir 'dxgi.dll'))
    if (-not @($lines | Where-Object { $_ -match "bridge module: $bridge$" }).Count) {
        $problems.Add('no "bridge module: ' + (Join-Path $RunDir 'dxgi.dll') + '" line')
    }
    if (@($lines | Where-Object { $_ -match '\] INFO mode: standalone: ' }).Count -ne 1) {
        $problems.Add('no single "mode: standalone" line')
    }
    $ini = [regex]::Escape((Join-Path $RunDir 'ac-dlssg\ac-dlssg.ini'))
    if (-not @($lines | Where-Object { $_ -match "config: $ini$" }).Count) {
        $problems.Add('the bridge did not read ac-dlssg\ac-dlssg.ini')
    }
    if (-not @($lines | Where-Object { $_ -match 'config: enabled=1 .* log_level=debug$' }).Count) {
        $problems.Add('the config line does not show enabled=1 and log_level=debug')
    }
    return $problems
}

# Written before every run: ReShade saves its settings back on exit.
function Write-ReShadeIni([string]$RunDir) {
    $lines = @(
        '[ADDON]',
        '',
        '[GENERAL]',
        'EffectSearchPaths=',
        ('IntermediateCachePath=' + (Join-Path $RunDir 'reshade-cache')),
        'NoReloadOnInit=0',
        'TextureSearchPaths=',
        '',
        '[OVERLAY]',
        'TutorialProgress=4',
        '',
        '[PROXY]',
        'EnableProxyLibrary=1',
        'ProxyLibrary=ac-dlssg.dll',
        ''
    )
    [System.IO.File]::WriteAllText((Join-Path $RunDir 'ReShade.ini'), ($lines -join "`r`n"), $utf8NoBom)
}

# ReShade.log of a reshade scenario, in ReShade 6.8.0's wording. Problems fail
# the scenario; Evidence holds the relevant lines in log order, for printing.
function Test-ReShadeLog([string]$LogPath, [string]$RunDir, [int]$Resizes, [string[]]$Wrappers = @()) {
    $problems = New-Object System.Collections.Generic.List[string]
    $evidence = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) {
        $problems.Add("no ReShade.log in $RunDir")
        return [pscustomobject]@{ Problems = $problems; Evidence = $evidence }
    }
    $lines = @([System.IO.File]::ReadAllLines($LogPath))
    $has = { param($pattern) @($lines | Where-Object { $_ -match $pattern }) }

    # ReShade resolves its DXGI exports from the ProxyLibrary, and leaves
    # System32's dxgi.dll alone ("Registering hooks for ...dxgi.dll", "> Skipped.").
    $bridge = [regex]::Escape((Join-Path $RunDir 'ac-dlssg.dll'))
    $exports = @(& $has "Installing export hooks for '")
    if (-not @($exports | Where-Object { $_ -match "Installing export hooks for '$bridge'" }).Count) {
        $problems.Add('no "Installing export hooks for ''...\ac-dlssg.dll''"')
    }
    foreach ($line in @($exports | Where-Object { $_ -notmatch "'$bridge'" })) {
        $problems.Add("export hooks for a module other than ac-dlssg.dll: $line")
    }
    foreach ($pattern in @('Failed to load ', 'Failed to create back buffer render targets', 'Skipping swap chain',
            '\| ERROR \|')) {
        foreach ($line in (& $has $pattern)) { $problems.Add("ReShade.log: $line") }
    }

    # The test app's D3D11CreateDevice resolves through ReShade, which wraps
    # the device; a swap chain created on an unwrapped device is skipped.
    $device = @(& $has 'Redirecting D3D11CreateDevice\(')
    if ($device.Count -ne 1) {
        $problems.Add("$($device.Count) ""Redirecting D3D11CreateDevice"" lines, expected the test app's one")
    }
    # ReShade must see the test app's chain and only that one. The bridge
    # creates its hidden D3D11 chain and its D3D12 chain on native factories,
    # and its D3D12 device inside ReShade's CreateSwapChainForHwnd, where
    # ReShade's D3D12CreateDevice hook passes the call through without a log line.
    $created = @(& $has 'Redirecting IDXGIFactory\d?::CreateSwapChain')
    if ($created.Count -ne 1) {
        $problems.Add("ReShade saw $($created.Count) swap chain creations, expected exactly the test app's one")
    }
    foreach ($line in (& $has 'Redirecting D3D12CreateDevice|Redirecting ID3D12')) {
        $problems.Add("ReShade intercepted the bridge's D3D12 objects: $line")
    }
    # One runtime; initialised at creation and again after every ResizeBuffers.
    $recreated = @(& $has 'Recreated runtime environment on runtime ')
    $resized = @(& $has 'Redirecting IDXGISwapChain\d?::ResizeBuffers')
    $runtimes = @($recreated | ForEach-Object {
            if ($_ -match 'on runtime ([0-9A-Fa-f]+)') { $Matches[1] } } | Sort-Object -Unique)
    if ($runtimes.Count -ne 1) {
        $problems.Add("$($runtimes.Count) runtimes in ""Recreated runtime environment"" lines, expected one")
    }
    if ($resized.Count -ne $Resizes) {
        $problems.Add("$($resized.Count) ResizeBuffers calls, expected $Resizes")
    }
    if ($recreated.Count -ne 1 + $Resizes) {
        $problems.Add("""Recreated runtime environment"" $($recreated.Count) times, expected $(1 + $Resizes) " +
            "(creation and $Resizes resizes)")
    }
    # The chain whose runtime ReShade re-created is the one the test app
    # holds: the "this" of ReShade's ResizeBuffers lines is the wrapper address
    # the test app printed ("ReShade's wrapper <address>").
    $held = @($Wrappers | Where-Object { $_ } | ForEach-Object { [Convert]::ToUInt64($_, 16) })
    if ($held.Count -ne $created.Count) {
        $problems.Add("the test app reported $($held.Count) ReShade wrappers for $($created.Count) swap chain creations")
    }
    foreach ($line in $resized) {
        if ($line -match 'this = ([0-9A-Fa-f]+)' -and $held -contains [Convert]::ToUInt64($Matches[1], 16)) { continue }
        $problems.Add("ResizeBuffers on a chain the test app does not hold: $line")
    }

    $dxgiHooks = "Registering hooks for '[^']*[\\/]dxgi\.dll'"
    $evidencePattern = "Initializing crosire's ReShade|Installing export hooks|$dxgiHooks|" +
        'Redirecting (D3D11CreateDevice\(|CreateDXGIFactory|IDXGIFactory|IDXGISwapChain|D3D12|ID3D12)|' +
        '\| (BufferCount|Flags)\s+\||d3d12|Running on |runtime environment on runtime |Skipping|Failed|\| ERROR \|'
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match $evidencePattern) { $evidence.Add($lines[$i]) }
        elseif ($i -gt 0 -and $lines[$i - 1] -match $dxgiHooks) { $evidence.Add($lines[$i]) }
    }
    return [pscustomobject]@{ Problems = $problems; Evidence = $evidence }
}

# The bridge's per-second statistics lines (d3d12_presenter.cpp), with the
# bridge GPU time d3d11 + d3d12 of each line that has both values.
function Get-BridgeStats([string]$logPath) {
    $lines = New-Object System.Collections.Generic.List[string]
    $sums = New-Object System.Collections.Generic.List[double]
    if (Test-Path -LiteralPath $logPath -PathType Leaf) {
        foreach ($line in [System.IO.File]::ReadAllLines($logPath)) {
            if ($line -notmatch ' stats: base_fps=') { continue }
            $lines.Add($line)
            if ($line -match 'bridge_gpu_ms d3d11=([0-9.]+) d3d12=([0-9.]+)') {
                $sums.Add([double]::Parse($Matches[1], $invariant) + [double]::Parse($Matches[2], $invariant))
            }
        }
    }
    $average = if ($sums.Count) { ($sums | Measure-Object -Average).Average } else { $null }
    $maximum = if ($sums.Count) { ($sums | Measure-Object -Maximum).Maximum } else { $null }
    [pscustomobject]@{ Lines = $lines; Samples = $sums.Count; Average = $average; Maximum = $maximum }
}

$tmpDir = Join-Path $BuildDir 'tmp'
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$utf8NoBom = New-Object System.Text.UTF8Encoding $false
$results = New-Object System.Collections.Generic.List[object]
$reshade = $null
if (@($scenarios | Where-Object { $_.ReShade }).Count) {
    $reshade = Resolve-ReShadeDll $ReShadeDll
    if ($reshade.Path) {
        Write-Host "ReShade for the reshade scenarios: $($reshade.Path) (version $($reshade.Version))"
    } else {
        Write-Host "ReShade for the reshade scenarios: none ($($reshade.Reason)); they are skipped"
    }
}

foreach ($cfg in $Config) {
    $testAppDir = Join-Path $BuildDir "tools\testapp\$cfg"
    $reshadeDir = Join-Path $BuildDir "testapp-reshade\$cfg"
    $reshadeReady = $false
    $standaloneDir = Join-Path $BuildDir "testapp-standalone\$cfg"
    $standaloneReady = $false
    $logDir = Join-Path $BuildDir "testapp-logs\$cfg"
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null

    foreach ($s in $scenarios) {
        $argList = @($s.Args)
        if (-not $Visible) { $argList += '--hidden' }
        $argText = $argList -join ' '
        $outFile = Join-Path $logDir "$($s.Name).out.txt"
        $keptLog = Join-Path $logDir "$($s.Name).bridge.log"
        $keptReShadeLog = Join-Path $logDir "$($s.Name).reshade.log"
        Remove-Item -LiteralPath $outFile, $keptLog, $keptReShadeLog -Force -ErrorAction SilentlyContinue

        $exeDir = if ($s.ReShade) { $reshadeDir } elseif ($s.Standalone) { $standaloneDir } else { $testAppDir }
        $exe = Join-Path $exeDir 'testapp.exe'
        $bridgeLog = Join-Path $exeDir 'ac-dlssg\logs\bridge.log'
        $reshadeLog = Join-Path $exeDir 'ReShade.log'

        if (-not (Test-Path -LiteralPath (Join-Path $testAppDir 'testapp.exe') -PathType Leaf)) {
            $results.Add([pscustomobject]@{ Config = $cfg; Scenario = $s.Name; Result = 'FAIL'; Seconds = 0.0;
                    BridgeMs = ''; Detail = "not built: $(Join-Path $testAppDir 'testapp.exe')"; Args = $argText })
            continue
        }
        if ($s.ReShade) {
            if (-not $reshade.Path) {
                $results.Add([pscustomobject]@{ Config = $cfg; Scenario = $s.Name; Result = 'SKIP'; Seconds = 0.0;
                        BridgeMs = ''; Detail = $reshade.Reason; Args = $argText })
                continue
            }
            if (-not $reshadeReady) {
                Initialize-ReShadeRunDir $reshadeDir $testAppDir $reshade.Path
                $reshadeReady = $true
            }
            Write-ReShadeIni $reshadeDir
            Remove-Item -LiteralPath $reshadeLog -Force -ErrorAction SilentlyContinue
        }
        if ($s.Standalone -and -not $standaloneReady) {
            Initialize-StandaloneRunDir $standaloneDir $testAppDir
            $standaloneReady = $true
        }

        Write-Host "[$cfg] $($s.Name): testapp $argText$(if ($s.ReShade -or $s.Standalone) { " (in $exeDir)" })"
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $exe
        $psi.Arguments = $argText
        $psi.WorkingDirectory = $exeDir
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.CreateNoWindow = $true
        $psi.EnvironmentVariables['TEMP'] = $tmpDir
        $psi.EnvironmentVariables['TMP'] = $tmpDir
        # ReShade reads RESHADE_BASE_PATH_OVERRIDE and friends; its base path
        # must be the run folder.
        foreach ($name in @($psi.EnvironmentVariables.Keys | Where-Object { $_ -like 'RESHADE_*' })) {
            $psi.EnvironmentVariables.Remove($name)
        }

        $watch = [System.Diagnostics.Stopwatch]::StartNew()
        $proc = [System.Diagnostics.Process]::Start($psi)
        # Read both streams asynchronously so that a full pipe cannot block the app.
        $stdout = $proc.StandardOutput.ReadToEndAsync()
        $stderr = $proc.StandardError.ReadToEndAsync()
        $timedOut = -not $proc.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut) {
            try { $proc.Kill() } catch { }
            $proc.WaitForExit()
        }
        $watch.Stop()
        $output = $stdout.Result + $stderr.Result
        [System.IO.File]::WriteAllText($outFile, $output, $utf8NoBom)
        if (Test-Path -LiteralPath $bridgeLog -PathType Leaf) {
            Copy-Item -LiteralPath $bridgeLog -Destination $keptLog -Force
        }
        $stats = Get-BridgeStats $keptLog

        $lines = @($output -split "`r?`n" | Where-Object { $_ -ne '' })
        $summary = @($lines | Where-Object { $_ -match '^testapp: (PASS|FAIL)' } | Select-Object -Last 1)
        if ($timedOut) {
            $result = 'TIMEOUT'
            $detail = "killed after $TimeoutSeconds s"
        } elseif ($proc.ExitCode -eq 0) {
            $result = 'PASS'
            $detail = if ($summary.Count) { $summary[0] -replace '^testapp: ', '' } else { '' }
        } else {
            $result = 'FAIL'
            $detail = "exit $($proc.ExitCode): " + $(if ($summary.Count) { $summary[0] -replace '^testapp: ', '' }
                else { ($lines | Select-Object -Last 1) })
        }

        if ($s.ReShade) {
            if (Test-Path -LiteralPath $reshadeLog -PathType Leaf) {
                Copy-Item -LiteralPath $reshadeLog -Destination $keptReShadeLog -Force
            }
            $resizes = if ($s.Resizes) { [int]$s.Resizes } else { 0 }
            $wrapperLines = @($lines | Where-Object { $_ -match "ReShade's wrapper [0-9A-Fa-f]+ " })
            $wrappers = @($wrapperLines | ForEach-Object {
                    if ($_ -match "ReShade's wrapper ([0-9A-Fa-f]+) ") { $Matches[1] } })
            $check = Test-ReShadeLog $keptReShadeLog $exeDir $resizes $wrappers
            $wrapperLines | ForEach-Object { Write-Host "  $_" }
            Write-Host "  ReShade.log evidence:"
            $check.Evidence | ForEach-Object { Write-Host "    $_" }
            $bridgeReShade = @(if (Test-Path -LiteralPath $keptLog -PathType Leaf) {
                    [System.IO.File]::ReadAllLines($keptLog) | Where-Object { $_ -match ' ReShade: ' } })
            $bridgeReShade | ForEach-Object { Write-Host "  bridge.log: $_" }
            if ($check.Problems.Count) {
                $check.Problems | ForEach-Object { Write-Host "  PROBLEM: $_" }
                if ($result -eq 'PASS') { $result = 'FAIL' }
                $detail += "; " + ($check.Problems -join '; ')
            } elseif ($result -eq 'PASS') {
                $detail += "; ReShade.log: ProxyLibrary loaded, one runtime on the proxy chain"
            }
        }

        if ($s.Standalone) {
            $problems = @(Test-StandaloneLog $keptLog $exeDir)
            @($lines | Where-Object { $_ -match '^testapp: standalone: ' }) | ForEach-Object { Write-Host "  $_" }
            if ($problems.Count) {
                $problems | ForEach-Object { Write-Host "  PROBLEM: $_" }
                if ($result -eq 'PASS') { $result = 'FAIL' }
                $detail += "; " + ($problems -join '; ')
            } elseif ($result -eq 'PASS') {
                $detail += '; bridge loaded as dxgi.dll (mode: standalone)'
            }
        }

        if ($s.Budget) {
            # Success criterion 3: both frame copies together below the budget.
            Write-Host "  bridge statistics ($($stats.Lines.Count) lines):"
            $stats.Lines | ForEach-Object { Write-Host "    $_" }
            if ($stats.Samples -eq 0) {
                if ($result -eq 'PASS') { $result = 'FAIL' }
                $detail += "; no bridge_gpu_ms statistics, the $(Format-Ms $BudgetMs) ms budget is not checked"
            } else {
                $verdict = if ($stats.Average -le $BudgetMs) { 'within' } else { 'OVER' }
                Write-Host ("  bridge GPU time d3d11+d3d12: average {0} ms, maximum {1} ms over {2} lines: {3} the {4} ms budget" -f
                    (Format-Ms $stats.Average), (Format-Ms $stats.Maximum), $stats.Samples, $verdict, (Format-Ms $BudgetMs))
                if ($stats.Average -gt $BudgetMs) {
                    if ($result -eq 'PASS') { $result = 'FAIL' }
                    $detail += "; bridge GPU time $(Format-Ms $stats.Average) ms exceeds the $(Format-Ms $BudgetMs) ms budget"
                }
            }
        }

        if ($result -ne 'PASS') {
            Write-Host "---- output of $($s.Name) ($cfg) ----"
            $lines | ForEach-Object { Write-Host "  $_" }
            Write-Host '----'
        }
        $results.Add([pscustomobject]@{ Config = $cfg; Scenario = $s.Name; Result = $result;
                Seconds = $watch.Elapsed.TotalSeconds.ToString('0.0', $invariant);
                BridgeMs = $(if ($stats.Samples) { Format-Ms $stats.Average } else { '' }); Detail = $detail;
                Args = $argText })
    }
}

Write-Host ''
Write-Host ('{0,-8} {1,-19} {2,-7} {3,7} {4,9}  {5}' -f 'Config', 'Scenario', 'Result', 'Time', 'Bridge ms', 'Detail')
Write-Host ('{0,-8} {1,-19} {2,-7} {3,7} {4,9}  {5}' -f '------', '--------', '------', '----', '---------', '------')
foreach ($r in $results) {
    Write-Host ('{0,-8} {1,-19} {2,-7} {3,6}s {4,9}  {5}' -f $r.Config, $r.Scenario, $r.Result, $r.Seconds, $r.BridgeMs,
        $r.Detail)
}
$skipped = @($results | Where-Object { $_.Result -eq 'SKIP' }).Count
$failures = @($results | Where-Object { $_.Result -ne 'PASS' -and $_.Result -ne 'SKIP' }).Count
# The D3D runtime's per-executable shader cache lands in %LOCALAPPDATA%;
# nothing of the test runs may stay outside the work tree.
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'clean-shader-cache.ps1')
Write-Host ''
Write-Host ("Bridge ms: average bridge_gpu_ms d3d11 + d3d12 from the bridge's stats lines; the default scenario " +
    "fails above $(Format-Ms $BudgetMs) ms (spec 2, criterion 3).")
Write-Host ("run-testapp: $($results.Count - $failures - $skipped) passed, $failures failed, $skipped skipped " +
    "(logs in $(Join-Path $BuildDir 'testapp-logs'))")
exit $failures
