param(
  [Alias('Host')][Parameter(Mandatory = $true)][string]$Server,
  [int]$Port = 7000,
  [Parameter(Mandatory = $true)][string]$Token,
  [string]$Passphrase = '',
  [int]$Minutes = 10,
  [switch]$NoPrompt
)
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent          # remote/
$bench = Join-Path $root '..\build\release\srt-bench.exe'
if (-not (Test-Path $bench)) {
  throw "build srt-bench first: powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1"
}
$outDir = Join-Path $root 'bench-results'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$outFile = Join-Path $outDir ((Get-Date -Format 'yyyy-MM-dd-HHmm') + '.md')
$seconds = $Minutes * 60
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add("# SRT benchmark $(Get-Date -Format 'yyyy-MM-dd HH:mm')")
$lines.Add('')
$lines.Add("- server: ${Server}:$Port")
$lines.Add("- minutes per run: $Minutes")
if ($Passphrase) { $lines.Add('- passphrase: set') }
$lines.Add('')
Set-Content -Path $outFile -Value $lines

$optionSets = @(
  [pscustomobject]@{ Name = 'default'; Args = @() },
  [pscustomobject]@{ Name = 'tuned'; Args = @('--latency-rtt', '4', '--sndbuf', '64MiB', '--rcvbuf', '64MiB', '--fc', '60000') }
)

function Get-SummaryRow([string[]]$Log) {
  return ($Log | Where-Object { $_ -match '^\|' -and $_ -notmatch 'bitrate' } | Select-Object -Last 1)
}

function Add-Row([string]$Title, [string]$Row) {
  Add-Content -Path $outFile -Value @("## $Title", '', $Row, '')
}

foreach ($mbps in 10, 20, 40, 50) {
  foreach ($opt in $optionSets) {
    # ---- direct: the receiver runs on the VPS, one per run ----
    $recvArgs = @('recv', '--listen', '--port', '7001', '--seconds', ($seconds + 30).ToString())
    if ($Passphrase) { $recvArgs += @('--passphrase', $Passphrase) }
    $vpsCommand = "docker run --rm --network host --entrypoint srt-bench bmb-remote " + ($recvArgs -join ' ')
    Write-Host ''
    Write-Host "== $mbps Mbps, $($opt.Name), direct =="
    Write-Host "On the VPS, start: $vpsCommand"
    if (-not $NoPrompt) { Read-Host 'Press Enter when the VPS receiver is running (or when it is already up)' }

    $sendArgs = @('send', '--host', $Server, '--port', '7001', '--mbps', $mbps, '--seconds', $seconds) + $opt.Args
    if ($Passphrase) { $sendArgs += @('--passphrase', $Passphrase) }
    $log = @(& $bench @sendArgs 2>&1)
    Add-Row "$mbps Mbps, $($opt.Name), direct" (Get-SummaryRow $log)

    # ---- via the relay on the VPS: both ends run from here ----
    $stream = "bmb/bench-$mbps-$($opt.Name)/relay/cam1"
    $pubId = "#!::r=$stream,m=publish,u=$Token"
    $reqId = "#!::r=$stream,m=request,u=$Token"
    $recvArgs = @('recv', '--host', $Server, '--port', $Port, '--streamid', $reqId,
                  '--seconds', ($seconds + 2)) + $opt.Args
    if ($Passphrase) { $recvArgs += @('--passphrase', $Passphrase) }
    $sendArgs = @('send', '--host', $Server, '--port', $Port, '--streamid', $pubId,
                  '--mbps', $mbps, '--seconds', $seconds) + $opt.Args
    if ($Passphrase) { $sendArgs += @('--passphrase', $Passphrase) }

    Write-Host "== $mbps Mbps, $($opt.Name), via relay =="
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("srt-recv-" + [guid]::NewGuid().ToString('N') + '.log')
    $receiver = Start-Process -FilePath $bench -ArgumentList $recvArgs -NoNewWindow -PassThru -RedirectStandardOutput $tmp
    Start-Sleep -Seconds 2
    $sendLog = @(& $bench @sendArgs 2>&1)
    $receiver.WaitForExit()
    $recvRow = Get-SummaryRow (Get-Content -Path $tmp -ErrorAction SilentlyContinue)
    Remove-Item -Path $tmp -ErrorAction SilentlyContinue
    Add-Row "$mbps Mbps, $($opt.Name), via relay — sender" (Get-SummaryRow $sendLog)
    Add-Row "$mbps Mbps, $($opt.Name), via relay — receiver" $recvRow
  }
}

Add-Content -Path $outFile -Value 'Pass bar: zero unrecovered drops at 40 Mbps for 10 minutes, tuned, direct and via the relay.'
Write-Host ''
Write-Host "results: $outFile"