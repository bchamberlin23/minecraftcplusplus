# Builds minecraftCplusplus with MSVC + Ninja. Usage: .\build.ps1 [-Config Release|Debug] [-Dir <name>]
# -Dir builds into build\<name> (default: the config name), e.g. a private directory per concurrent builder.
param([string]$Config = "Release", [string]$Dir = "")
$ErrorActionPreference = "Stop"

$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vs = & "$installer\vswhere.exe" -latest -products * -property installationPath
$env:PATH = "$installer;$env:PATH"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] }
}
$env:PATH = "C:\Program Files\CMake\bin;$env:LOCALAPPDATA\Microsoft\WinGet\Links;$env:PATH"

$build = Join-Path $PSScriptRoot ("build\" + $(if ($Dir) { $Dir } else { $Config }))
cmake -S $PSScriptRoot -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $build
exit $LASTEXITCODE
