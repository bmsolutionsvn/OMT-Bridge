param(
  [ValidateSet('release', 'debug')][string]$Preset = 'release',
  [string]$Filter = ''
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -Preset $Preset
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Set-Location (Join-Path $PSScriptRoot "..\build\$Preset")
$ok = $true
if ($Filter) { & .\ob_tests.exe "--test-case=$Filter" } else { & .\ob_tests.exe }
if ($LASTEXITCODE -ne 0) { $ok = $false }
if ($Filter) { & .\remote_tests.exe "--test-case=$Filter" } else { & .\remote_tests.exe }
if ($LASTEXITCODE -ne 0) { $ok = $false }
if ($ok) { exit 0 } else { exit 1 }