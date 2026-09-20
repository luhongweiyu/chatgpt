param(
    [ValidateSet("Release","Debug")]
    [string]$Config = "Release",
    [string]$Generator = "Visual Studio 17 2022"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

cmake -S . -B build -A x64 -G $Generator
cmake --build build --config $Config

Write-Host ""
Write-Host "Built:"
Write-Host "  build\$Config\hcbyj64.dll"
Write-Host "  build\$Config\hcbyj64.lib"
Write-Host "  build\$Config\hcbyj64_smoke.exe"
