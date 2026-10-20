param(
  [Parameter(Mandatory = $true)][string]$Source,
  [ValidateSet('OMT', 'OMT-HX')][string]$Target = 'OMT',
  [int]$Seconds = 20,
  [ValidateSet('release', 'debug')][string]$Configuration = 'release'
)

# M5 cross-family proof: an NDI source (a test pattern, OBS, Studio Monitor, ...) through the
# bridge to an OMT (Full) or OMT-HX output, verified by omthx-recv --dump.
#
#   .\scripts\m5-ndi-to-omt.ps1 -Source "MACHINE (Test Pattern)" -Target OMT
#
# The source name is exactly what an NDI receiver shows, e.g. from Studio Monitor's source list.
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "build\$Configuration"
$engineExe = Join-Path $bin 'omt-bridge-engine.exe'
$recvExe = Join-Path $bin 'omthx-build\omthx-recv.exe'

$missing = @($engineExe, $recvExe) | Where-Object { -not (Test-Path -LiteralPath $_) }
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
    & cmake --build --preset $Configuration --target omthx-recv omt-bridge-engine
    if ($LASTEXITCODE) { throw "cmake build failed ($LASTEXITCODE)" }
  } finally {
    Pop-Location
  }
  foreach ($path in $missing) {
    if (-not (Test-Path -LiteralPath $path)) { throw "build did not produce $path" }
  }
}

# A free loopback port chosen by the OS.
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
$listener.Stop()

$tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
$work = Join-Path ([System.IO.Path]::GetTempPath()) "omt-bridge-m5-$tag"
New-Item -ItemType Directory -Path $work | Out-Null

$bridgeName = "M5 NDI Bridge $tag"
$outputName = 'M5 NDI Out'
$outputAddress = "$env:COMPUTERNAME ($bridgeName ($outputName))"
$bridgePath = Join-Path $work 'bridge.json'
$dumpPath = Join-Path $work 'frame.bmp'
$engineOut = Join-Path $work 'engine.out'; $engineErr = Join-Path $work 'engine.err'
$recvOut = Join-Path $work 'recv.out'; $recvErr = Join-Path $work 'recv.err'

$bridge = [ordered]@{
  name = $bridgeName
  settings = [ordered]@{
    defaultTarget = $Target
    defaultQuality = 'Medium'
    thumbnailIntervalMs = 2000
    startEngineWithApp = $false
  }
  sources = @([ordered]@{ id = 's1'; kind = 'NDI'; address = $Source })
  outputs = @([ordered]@{
    id = 'o1'
    sourceId = 's1'
    target = $Target
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
  return Start-Process -FilePath $Exe -ArgumentList (Format-ArgumentList $Arguments) `
    -RedirectStandardOutput $Out -RedirectStandardError $Err -NoNewWindow -PassThru
}

function Assert-Dump([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path)) { throw "receiver wrote no dump at $Path" }
  if ((Get-Item -LiteralPath $Path).Length -lt 58) { throw "dump at $Path is too short to be a BMP" }
  $stream = [System.IO.File]::OpenRead($Path)
  try {
    $header = New-Object byte[] 54
    [void]$stream.Read($header, 0, 54)
  } finally {
    $stream.Dispose()
  }
  if ($header[0] -ne 0x42 -or $header[1] -ne 0x4D) { throw "dump at $Path is not a BMP" }
  $w = [BitConverter]::ToInt32($header, 18)
  $h = [math]::Abs([BitConverter]::ToInt32($header, 22))
  if ($w -lt 2 -or $h -lt 2) { throw "dump at $Path has no picture (${w}x${h})" }
  return [pscustomobject]@{ width = $w; height = $h }
}

$engineArgs = @('--port', "$port", '--token', "m5-$tag", '--bridge', $bridgePath)
$recvArgs = @('--source', $outputAddress, '--seconds', "$Seconds", '--dump', $dumpPath)
$engineProcess = $null
$recvProcess = $null
$failure = $null

try {
  Write-Host "source: $Source (NDI)  ->  $Target"
  Write-Host "engine: $engineExe $(Format-ArgumentList $engineArgs)"
  Write-Host "receiver: $recvExe $(Format-ArgumentList $recvArgs)"

  $engineProcess = Start-Logged $engineExe $engineArgs $engineOut $engineErr
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

  Start-Sleep -Milliseconds 1500  # the output sender needs discovery time
  $recvProcess = Start-Logged $recvExe $recvArgs $recvOut $recvErr
  if (-not $recvProcess.WaitForExit(($Seconds + 30) * 1000)) {
    throw "omthx-recv did not finish within $($Seconds + 30) s"
  }
  if ($null -ne $recvProcess.ExitCode -and $recvProcess.ExitCode -ne 0) {
    throw "omthx-recv exited with code $($recvProcess.ExitCode)"
  }

  $dump = Assert-Dump $dumpPath

  $fps = New-Object System.Collections.Generic.List[int]
  $mbps = New-Object System.Collections.Generic.List[double]
  foreach ($line in (Get-Content -LiteralPath $recvOut)) {
    if ($line -match '^\s*(?:HX|Full)\s+.*?\s(\d+)\s+fps\s+([0-9.]+)\s+Mbps') {
      $fps.Add([int]$Matches[1]); $mbps.Add([double]$Matches[2])
    }
  }
  if ($fps.Count -eq 0) { throw 'the receiver never reported a picture; check the NDI source name' }
  $avgFps = ($fps | Measure-Object -Average).Average
  $avgMbps = if ($mbps.Count) { ($mbps | Measure-Object -Average).Average } else { 0 }
  if ($engineProcess.HasExited) {
    throw "the engine exited during the run (code $($engineProcess.ExitCode))"
  }

  Write-Host "M5 NDI -> $Target OK"
  Write-Host ("  dump: {0} ({1} bytes, {2}x{3})" -f
    $dumpPath, (Get-Item $dumpPath).Length, $dump.width, $dump.height)
  Write-Host ("  receiver: {0:N1} fps avg, {1:N2} Mbps" -f $avgFps, $avgMbps)
} catch {
  $failure = $_
} finally {
  foreach ($process in @($recvProcess, $engineProcess)) {
    if ($process -and -not $process.HasExited) {
      Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
      [void]$process.WaitForExit(3000)
    }
  }
}

if ($failure) {
  Write-Host "M5 NDI TEST FAILED: $($failure.Exception.Message)" -ForegroundColor Red
  foreach ($log in @($engineOut, $engineErr, $recvOut, $recvErr)) {
    if (Test-Path -LiteralPath $log) {
      Write-Host "--- $log ---"
      Get-Content -LiteralPath $log -Tail 40
    }
  }
  Write-Host "artifacts: $work"
  exit 1
}

Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
