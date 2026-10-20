param(
  [ValidateSet('release', 'debug')][string]$Configuration = 'release',
  [int]$Seconds = 12,
  [int]$Width = 1920,
  [int]$Height = 1080,
  [int]$Fps = 60,
  [switch]$NoAlpha,
  [string]$Send,
  [string]$Recv,
  [string]$Engine
)

# M2 loopback proof: omthx-send -> OMT-Bridge engine -> omthx-recv --dump.
#
# OMT-HX's tools are NOT part of the normal build: OMTHX_BUILD_TESTS defaults to
# PROJECT_IS_TOP_LEVEL, which is false when OMT-HX is a submodule. This script builds
# them on demand with the project's own preset and the OMT-HX test/tool switch.
#
# The address is "<MACHINE> (<advertised name>)", the same string Sender::address()
# returns (libomt uses the machine name). The bridge advertises
# "<Bridge.name> (<Output.name>)", so the receiver's address is built from those.
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "build\$Configuration"

if (-not $Send) { $Send = Join-Path $bin 'omthx-build\omthx-send.exe' }
if (-not $Recv) { $Recv = Join-Path $bin 'omthx-build\omthx-recv.exe' }
if (-not $Engine) { $Engine = Join-Path $bin 'omt-bridge-engine.exe' }

$missing = @($Send, $Recv, $Engine) | Where-Object { -not (Test-Path -LiteralPath $_) }
if ($missing) {
  Write-Host "building missing binaries: $($missing -join ', ')"
  $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
  $vswhere = Join-Path $installer 'vswhere.exe'
  if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "missing binaries and no vswhere.exe at $vswhere"
  }
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

# A free loopback port, chosen by the OS: fixed ports can fall inside Windows' excluded ranges.
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
$listener.Stop()

$tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
$work = Join-Path ([System.IO.Path]::GetTempPath()) "omt-bridge-m2-$tag"
New-Item -ItemType Directory -Path $work | Out-Null

$sourceName = "M2 Source $tag"
$bridgeName = "M2 Bridge $tag"
$outputName = 'M2 Out'
$sourceAddress = "$env:COMPUTERNAME ($sourceName)"
$outputAddress = "$env:COMPUTERNAME ($bridgeName ($outputName))"

$bridgePath = Join-Path $work 'bridge.json'
$dumpPath = Join-Path $work 'frame.bmp'
$sendOut = Join-Path $work 'send.out'; $sendErr = Join-Path $work 'send.err'
$engineOut = Join-Path $work 'engine.out'; $engineErr = Join-Path $work 'engine.err'
$recvOut = Join-Path $work 'recv.out'; $recvErr = Join-Path $work 'recv.err'

$bridge = [ordered]@{
  name = $bridgeName
  settings = [ordered]@{
    defaultTarget = 'OMT-HX'
    defaultQuality = 'Medium'
    thumbnailIntervalMs = 2000
    startEngineWithApp = $false
  }
  sources = @([ordered]@{
    id = 's1'
    kind = 'OMT-HX'
    address = $sourceAddress
  })
  outputs = @([ordered]@{
    id = 'o1'
    sourceId = 's1'
    target = 'OMT-HX'
    codec = 'HEVC'
    quality = 'Medium'
    alpha = 'Auto'
    name = $outputName
  })
}
[System.IO.File]::WriteAllText($bridgePath, ($bridge | ConvertTo-Json -Depth 8),
  (New-Object System.Text.UTF8Encoding($false)))

function Format-ArgumentList([string[]]$Arguments) {
  return (($Arguments | ForEach-Object {
    if ($_ -match '[\s()]') { '"' + $_ + '"' } else { $_ }
  }) -join ' ')
}

function Start-Logged([string]$Exe, [string[]]$Arguments, [string]$Out, [string]$Err) {
  $line = Format-ArgumentList $Arguments
  return Start-Process -FilePath $Exe -ArgumentList $line -RedirectStandardOutput $Out `
    -RedirectStandardError $Err -NoNewWindow -PassThru
}

function Assert-Dump([string]$Path, [int]$W, [int]$H) {
  if (-not (Test-Path -LiteralPath $Path)) { throw "receiver wrote no dump at $Path" }
  $length = (Get-Item -LiteralPath $Path).Length
  $expected = 54 + $W * $H * 4
  if ($length -ne $expected) {
    throw "dump is $length bytes, expected $expected for ${W}x${H} (missing or truncated)"
  }
  $stream = [System.IO.File]::OpenRead($Path)
  try {
    $header = New-Object byte[] 54
    [void]$stream.Read($header, 0, 54)
  } finally {
    $stream.Dispose()
  }
  if ($header[0] -ne 0x42 -or $header[1] -ne 0x4D) { throw "dump at $Path is not a BMP" }
  $bmpWidth = [BitConverter]::ToInt32($header, 18)
  $bmpHeight = [math]::Abs([BitConverter]::ToInt32($header, 22))
  if ($bmpWidth -ne $W -or $bmpHeight -ne $H) {
    throw "dump is ${bmpWidth}x${bmpHeight}, expected ${W}x${H}"
  }
}

$sendArgs = @('--name', $sourceName, '--mode', 'hx', '--codec', 'hevc',
  '--size', "${Width}x${Height}", '--fps', "$Fps", '--seconds', "$($Seconds + 4)")
if (-not $NoAlpha) { $sendArgs += '--alpha' }
$engineArgs = @('--port', "$port", '--token', "m2-$tag", '--bridge', $bridgePath)
$recvArgs = @('--source', $outputAddress, '--seconds', "$Seconds", '--dump', $dumpPath)

$sendProcess = $null
$engineProcess = $null
$recvProcess = $null
$failure = $null
$metrics = $null

try {
  Write-Host "sender: $Send $(Format-ArgumentList $sendArgs)"
  Write-Host "engine: $Engine $(Format-ArgumentList $engineArgs)"
  Write-Host "receiver: $Recv $(Format-ArgumentList $recvArgs)"

  $sendProcess = Start-Logged $Send $sendArgs $sendOut $sendErr
  Start-Sleep -Milliseconds 1500

  $engineProcess = Start-Logged $Engine $engineArgs $engineOut $engineErr
  $ready = $false
  $deadline = (Get-Date).AddSeconds(10)
  while ((Get-Date) -lt $deadline) {
    if ($engineProcess.HasExited) {
      throw "engine exited with code $($engineProcess.ExitCode) before READY"
    }
    if ((Test-Path -LiteralPath $engineOut) -and
        (Select-String -LiteralPath $engineOut -Pattern '^READY ' -Quiet)) {
      $ready = $true
      break
    }
    Start-Sleep -Milliseconds 100
  }
  if (-not $ready) { throw 'engine did not report READY within 10 s' }

  # The output sender exists as soon as the bridge is applied; give discovery a moment
  # before a receiver starts looking for its advertised name.
  Start-Sleep -Milliseconds 2000

  $engineProcess.Refresh()
  $cpuStart = $engineProcess.TotalProcessorTime
  $wall = [System.Diagnostics.Stopwatch]::StartNew()
  $recvProcess = Start-Logged $Recv $recvArgs $recvOut $recvErr
  if (-not $recvProcess.WaitForExit(($Seconds + 20) * 1000)) {
    throw "omthx-recv did not finish within $($Seconds + 20) s"
  }
  $wall.Stop()
  $engineProcess.Refresh()
  $cpuEnd = $engineProcess.TotalProcessorTime
  # PowerShell 5.1 can leave ExitCode null on a timed WaitForExit; a real non-zero exit
  # is still reported, and the dump assertion below is the substantive check.
  $recvCode = $recvProcess.ExitCode
  if ($null -ne $recvCode -and $recvCode -ne 0) {
    throw "omthx-recv exited with code $recvCode"
  }

  Assert-Dump $dumpPath $Width $Height

  # Let the sender leave cleanly so its buffered stats reach the log; if it is still
  # running, the finally block stops it and the receiver figures are unaffected.
  if (-not $sendProcess.HasExited) { [void]$sendProcess.WaitForExit(8000) }

  $rates = New-Object System.Collections.Generic.List[int]
  $mbps = New-Object System.Collections.Generic.List[double]
  $decodes = New-Object System.Collections.Generic.List[double]
  $alphaSeen = $false
  $delayMax = 0.0
  foreach ($line in (Get-Content -LiteralPath $recvOut)) {
    if ($line -match '^\s*(?:HX|Full)\s+.*?\s(\d+)\s+fps\s+([0-9.]+)\s+Mbps\s+decode\s+([0-9.]+)\s+ms.*?delay avg\s+([0-9.]+)\s+max\s+([0-9.]+)\s+ms') {
      $rates.Add([int]$Matches[1])
      $mbps.Add([double]$Matches[2])
      $decodes.Add([double]$Matches[3])
      if ([double]$Matches[5] -gt $delayMax) { $delayMax = [double]$Matches[5] }
      if ($line -match '\+alpha') { $alphaSeen = $true }
    }
  }
  $frames = ($rates | Measure-Object -Sum).Sum
  $cpuSeconds = ($cpuEnd - $cpuStart).TotalSeconds
  $nominalFrames = $wall.Elapsed.TotalSeconds * $Fps
  $metrics = [ordered]@{
    dumpBytes = (Get-Item -LiteralPath $dumpPath).Length
    dumpPath = $dumpPath
    frames = $frames
    avgFps = if ($rates.Count) { $frames / $rates.Count } else { 0 }
    avgMbps = if ($mbps.Count) { ($mbps | Measure-Object -Average).Average } else { 0 }
    avgDecodeMs = if ($decodes.Count) { ($decodes | Measure-Object -Average).Average } else { 0 }
    delayMaxMs = $delayMax
    alpha = $alphaSeen
    wallSeconds = $wall.Elapsed.TotalSeconds
    cpuSeconds = $cpuSeconds
    cores = if ($wall.Elapsed.TotalSeconds) { $cpuSeconds / $wall.Elapsed.TotalSeconds } else { 0 }
    msPerOutputFrame = if ($nominalFrames) { $cpuSeconds * 1000 / $nominalFrames } else { 0 }
  }

  # A correctly sized BMP alone does not certify the run: require enough samples, a real
  # frame rate, the requested alpha, and a live engine.
  $minSamples = [Math]::Max(3, $Seconds - 3)
  if ($rates.Count -lt $minSamples) {
    throw "receiver reported only $($rates.Count) seconds; need at least $minSamples"
  }
  $minFps = [Math]::Max(1, [int]($Fps * 0.9))
  if ($metrics.avgFps -lt $minFps) {
    throw ("receiver averaged {0:N1} fps, below 90% of {1}" -f $metrics.avgFps, $Fps)
  }
  if (-not $NoAlpha -and -not $alphaSeen) {
    throw "alpha was requested but the receiver never reported +alpha"
  }
  if ($engineProcess.HasExited) {
    throw "the engine exited during the run (code $($engineProcess.ExitCode))"
  }
} catch {
  $failure = $_
} finally {
  foreach ($process in @($recvProcess, $engineProcess, $sendProcess)) {
    if ($process -and -not $process.HasExited) {
      Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
      [void]$process.WaitForExit(3000)  # release log/dump handles before the dir is removed
    }
  }
}

if ($failure) {
  Write-Host "M2 E2E FAILED: $($failure.Exception.Message)" -ForegroundColor Red
  foreach ($log in @($engineOut, $engineErr, $recvOut, $recvErr, $sendOut, $sendErr)) {
    if (Test-Path -LiteralPath $log) {
      Write-Host "--- $log ---"
      Get-Content -LiteralPath $log -Tail 40
    }
  }
  Write-Host "artifacts: $work"
  exit 1
}

Write-Host 'M2 E2E OK'
Write-Host ("  dump: {0} ({1} bytes, {2}x{3}{4})" -f $metrics.dumpPath, $metrics.dumpBytes,
  $Width, $Height, $(if ($metrics.alpha) { ', alpha' } else { '' }))
Write-Host ("  receiver: {0:N1} fps avg over {1} s, {2:N2} Mbps, decode {3:N2} ms, delay max {4:N2} ms, {5} frames" -f
  $metrics.avgFps, $Seconds, $metrics.avgMbps, $metrics.avgDecodeMs, $metrics.delayMaxMs, $metrics.frames)
Write-Host ("  engine CPU: {0:N2} s over {1:N2} s wall ({2:N2} cores); {3:N1} ms CPU per output frame at {4} fps" -f
  $metrics.cpuSeconds, $metrics.wallSeconds, $metrics.cores, $metrics.msPerOutputFrame, $Fps)
Write-Host "  artifacts: $work"
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue  # keep logs only on failure
