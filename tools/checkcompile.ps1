# Compiles the given .cpp files standalone (no linking) with the project's flags.
# Safe to run while another build is in progress. Usage: .\tools\checkcompile.ps1 src\worldgen\density.cpp [...]
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Files)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vs = & "$installer\vswhere.exe" -latest -products * -property installationPath
$env:PATH = "$installer;$env:PATH"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] }
}
$out = Join-Path $env:TEMP ("mcpp-check-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $out | Out-Null
$failed = 0
foreach ($f in $Files) {
    $path = Resolve-Path (Join-Path $root $f) -ErrorAction SilentlyContinue
    if (-not $path) { $path = Resolve-Path $f }
    $fp = if ($path -match 'worldgen') { "/fp:precise" } else { "/fp:fast" }
    & cl /nologo /c /std:c++20 /EHsc /permissive- /Zc:__cplusplus /utf-8 /W3 /O2 $fp `
        /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS `
        /I "$root\src" /I "$root\third_party" /I "$root\third_party\libdeflate" `
        /Fo"$out\\" $path
    if ($LASTEXITCODE -ne 0) { $failed++ }
}
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
if ($failed) { Write-Host "$failed file(s) failed"; exit 1 } else { Write-Host "ok" }
