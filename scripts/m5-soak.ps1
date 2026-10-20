param(
  [int]$Minutes = 10,
  [int]$Width = 1920,
  [int]$Height = 1080,
  [int]$Fps = 60,
  [int]$SampleSeconds = 10,
  [switch]$NoAlpha,
  [ValidateSet('release', 'debug')][string]$Configuration = 'release'
)

# M5 soak: omthx-send -> OMT-HX output -> omthx-recv for N minutes, sampling the engine's
# working set and CPU. Fails on a sustained frame-rate loss or a large memory increase.
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "build\$Configuration"
$sendExe = Join-Path $bin 'omthx-build\omthx-send.exe'
$recvExe = Join-Path $bin 'omthx-build\omthx-recv.exe'
$engineExe = Join-Path $bin 'omt-bridge-engine.exe'
$missing = @($sendExe, $recvExe, $engineExe) | Where-Object { -not (Test-Path -LiteralPath $_) }
if ($missing) {
  Write-Host "building missing binaries: $($missing -join ', ')"
  $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
  $vswhere = Join-Path $installer 'vswhere.exe'
  if (-not (Test-Path -LiteralPath $vswhere)) { throw "missing binaries and no vswhere.exe at $vswhere" }
  $vs = & $vswhere -latest -products * -property installationPath
  if (-not $vs) { throw 'Visual Studio installation not found' }
  & (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
  if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = Join-Path $vs 'VC\vcpkg' }
  Push-Location $root
  try {
    & cmake --preset $Configuration -DOMTHX_BUILD_TESTS=ON
    if ($LASTEXITCODE) { throw "cmake configure failed ($LASTEXITCODE)" }
    & cmake --build --preset $Configuration --target omthx-send omthx-recv omt-bridge-engine
    if ($LASTEXITCODE) { throw "cmake build failed ($LASTEXITCODE)" }
  } finally {
    Pop-Location
  }
  foreach ($path in $missing) {
    if (-not (Test-Path -LiteralPath $path)) { throw "build did not produce $path" }
  }
}

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
$listener.Stop()

$tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
$work = Join-Path ([System.IO.Path]::GetTempPath()) "omt-bridge-soak-$tag"
New-Item -ItemType Directory -Path $work | Out-Null

$sourceName = "Soak Source $tag"
$bridgeName = "Soak Bridge $tag"
$outputName = 'Soak Out'
$sourceAddress = "$env:COMPUTERNAME ($sourceName)"
$outputAddress = "$env:COMPUTERNAME ($bridgeName ($outputName))"
$bridgePath = Join-Path $work 'bridge.json'
$dumpPath = Join-Path $work 'frame.bmp'
$sendOut = Join-Path $work 'send.out'; $sendErr = Join-Path $work 'send.err'
$engineOut = Join-Path $work 'engine.out'; $engineErr = Join-Path $work 'engine.err'
$recvOut = Join-Path $work 'recv.out'; $recvErr = Join-Path $work 'recv.err'

$bridge = [ordered]@{
  name = $bridgeName
  settings = [ordered]@{ defaultTarget = 'OMT-HX'; defaultQuality = 'Medium'
    thumbnailIntervalMs = 2000; startEngineWithApp = $false }
  sources = @([ordered]@{ id = 's1'; kind = 'OMT-HX'; address = $sourceAddress })
  outputs = @([ordered]@{ id = 'o1'; sourceId = 's1'; target = 'OMT-HX'; codec = 'HEVC'
    quality = 'Medium'; alpha = 'Auto'; name = $outputName })
}
[System.IO.File]::WriteAllText($bridgePath, ($bridge | ConvertTo-Json -Depth 8),
  (New-Object System.Text.UTF8Encoding($false)))

function Format-ArgumentList([string[]]$Arguments) {
  return (($Arguments | ForEach-Object {
    if ($_ -match '[\s()]') { '"' + $_ + '"' } else { $_ }
  }) -join ' ')
}
function Start-Logged([string]$Exe, [string[]]$Arguments, [string]$Out, [string]$Err) {
  return Start-Process -FilePath $Exe -ArgumentList (Format-ArgumentList $Arguments) `
    -RedirectStandardOutput $Out -RedirectStandardError $Err -NoNewWindow -PassThru
}

$seconds = $Minutes * 60
$sendArgs = @('--name', $sourceName, '--mode', 'hx', '--codec', 'hevc', '--size',
  "${Width}x${Height}", '--fps', "$Fps", '--seconds', "$($seconds + 15)")
if (-not $NoAlpha) { $sendArgs += '--alpha' }
$engineArgs = @('--port', "$port", '--token', "soak-$tag", '--bridge', $bridgePath)
$recvArgs = @('--source', $outputAddress, '--seconds', "$seconds", '--dump', $dumpPath)

$sendProcess = $null; $engineProcess = $null; $recvProcess = $null
$samples = New-Object System.Collections.Generic.List[object]
$failure = $null

try {
  $sendProcess = Start-Logged $sendExe $sendArgs $sendOut $sendErr
  Start-Sleep -Milliseconds 1500
  $engineProcess = Start-Logged $engineExe $engineArgs $engineOut $engineErr
  $deadline = (Get-Date).AddSeconds(10)
  while ((Get-Date) -lt $deadline) {
    if ($engineProcess.HasExited) { throw "engine exited before READY" }
    if ((Test-Path $engineOut) -and (Select-String -LiteralPath $engineOut -Pattern '^READY ' -Quiet)) { break }
    Start-Sleep -Milliseconds 100
  }
  Start-Sleep -Milliseconds 1500

  $engineProcess.Refresh()
  $cpuStart = $engineProcess.TotalProcessorTime
  $wall = [System.Diagnostics.Stopwatch]::StartNew()
  $recvProcess = Start-Logged $recvExe $recvArgs $recvOut $recvErr

  while (-not $recvProcess.HasExited) {
    if ($engineProcess.HasExited) { throw "engine exited during the soak" }
    $engineProcess.Refresh()
    $samples.Add([pscustomobject]@{
      seconds = [math]::Round($wall.Elapsed.TotalSeconds, 1)
      rssMB = [math]::Round($engineProcess.WorkingSet64 / 1MB, 1)
    })
    Start-Sleep -Seconds $SampleSeconds
  }
  $wall.Stop()
  $engineProcess.Refresh()
  $cpu = ($engineProcess.TotalProcessorTime - $cpuStart).TotalSeconds

  if ($null -ne $recvProcess.ExitCode -and $recvProcess.ExitCode -ne 0) {
    throw "omthx-recv exited with code $($recvProcess.ExitCode)"
  }
  $rates = @()
  foreach ($line in (Get-Content $recvOut)) {
    if ($line -match '^\s*(?:HX|Full)\s+.*?\s(\d+)\s+fps\s+([0-9.]+)\s+Mbps') { $rates += [int]$Matches[1] }
  }
  if ($rates.Count -lt 3) { throw "receiver reported only $($rates.Count) samples" }
  $avgFps = ($rates | Measure-Object -Average).Average
  if ($avgFps -lt $Fps * 0.8) { throw ("average {0:N1} fps below 80% of {1}" -f $avgFps, $Fps) }

  # Memory: skip the first 20 s of allocator warm-up, then compare the later peak with the
  # early baseline. 300 MB leaves room for codec reference frames without hiding a leak.
  $warm = $samples | Where-Object { $_.seconds -ge 20 }
  if ($warm.Count -ge 3) {
    $baseline = ($warm | Select-Object -First 3 | Measure-Object -Property rssMB -Average).Average
    $peak = ($warm | Measure-Object -Property rssMB -Maximum).Maximum
    if ($peak -gt $baseline + 300) {
      throw ("engine working set grew from {0:N1} MB to {1:N1} MB" -f $baseline, $peak)
    }
    Write-Host ("  RSS: {0:N1} MB baseline -> {1:N1} MB peak" -f $baseline, $peak)
  }

  Write-Host "M5 SOAK OK ($Minutes min)"
  Write-Host ("  receiver: {0:N1} fps avg, {1} samples" -f $avgFps, $rates.Count)
  Write-Host ("  engine CPU: {0:N1} s over {1:N1} s wall ({2:N2} cores)" -f
    $cpu, $wall.Elapsed.TotalSeconds, ($cpu / [math]::Max(1, $wall.Elapsed.TotalSeconds)))
} catch {
  $failure = $_
} finally {
  foreach ($process in @($recvProcess, $engineProcess, $sendProcess)) {
    if ($process -and -not $process.HasExited) {
      Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
      [void]$process.WaitForExit(3000)
    }
  }
}

if ($failure) {
  Write-Host "M5 SOAK FAILED: $($failure.Exception.Message)" -ForegroundColor Red
  if ($samples.Count) { $samples | Select-Object -Last 10 | Format-Table | Out-String | Write-Host }
  foreach ($log in @($engineOut, $engineErr, $recvOut, $recvErr, $sendOut, $sendErr)) {
    if (Test-Path $log) { Write-Host "--- $log ---"; Get-Content $log -Tail 30 }
  }
  Write-Host "artifacts: $work"
  exit 1
}

Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
