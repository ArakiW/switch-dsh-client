$ErrorActionPreference = 'Stop'
# switch-tools 的 exe 依赖 msys64 运行库
$env:PATH = "C:\msys64\usr\bin;$env:PATH"
Write-Host ("T2: " + $env:PATH.Substring(0,40))
