# scripts/m3-ndi-smoke.ps1 — NDI in -> NDI out through the engine's own adapters.
#
# The proof lives in ob_tests' NDI loopback cases (an NdiSink advertises, an NdiSource finds it,
# and video, the three alpha bands and 44.1 kHz stereo audio must survive the round trip). This
# script runs just those cases so a release check needs one command.
#
# Still manual after this passes: the NDI Studio Monitor alpha smoke test (design 15.3/18.1) and
# the downstream-tally test with a real receiver.
param([ValidateSet('release', 'debug')][string]$Configuration = 'release')
$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\$Configuration\ob_tests.exe"
if (-not (Test-Path -LiteralPath $exe)) {
  & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$out = & $exe '--test-case=*NDI*' 2>&1
$out | Write-Output
if ($LASTEXITCODE -ne 0) { throw "NDI smoke failed: $LASTEXITCODE" }

# A binary built with OB_WITH_NDI=OFF runs zero NDI cases and still exits 0; refuse that.
$cases = ($out | Select-String -Pattern 'test cases:\s+(\d+)' | Select-Object -First 1)
if (-not $cases -or [int]$cases.Matches[0].Groups[1].Value -lt 3) {
  throw "fewer than three NDI cases ran; was this built with OB_WITH_NDI=ON?"
}

Write-Output "M3 NDI OK (video, alpha, straight conversion, 44.1 kHz audio)"
Write-Output "manual still pending: Studio Monitor alpha smoke, downstream tally"
