$ErrorActionPreference = 'Stop'
# switch-tools deps
$env:PATH = "C:\msys64\usr\bin;$env:PATH"
Write-Host ("T1: " + $env:PATH.Substring(0,40))
