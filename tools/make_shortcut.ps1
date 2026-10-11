# Creates a Start Menu shortcut (and, with -Desktop, a desktop one) that starts the game without a console window:
# the title screen, from which worlds and servers are opened. Nothing is created unless you run this.
#   powershell -ExecutionPolicy Bypass -File tools\make_shortcut.ps1 [-Desktop] [-Build Release] [-Name "Minecraft C++"]
param(
    [switch]$Desktop,
    [string]$Build = "Release",  # build\<Build>\ holds the executables
    [string]$Name = "minecraftCplusplus"
)
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root "build\$Build"
$launcher = Join-Path $dir "minecraftCplusplusW.exe"
if (-not (Test-Path $launcher)) {
    throw "$launcher not found: build first (.\build.ps1$(if ($Build -ne 'Release') { " -Dir $Build" }))"
}

# The icon: the game launcher's, if installed; otherwise the default.
$icon = $null
foreach ($candidate in @(
        "${env:ProgramFiles(x86)}\Minecraft Launcher\MinecraftLauncher.exe",
        "$env:ProgramFiles\Minecraft Launcher\MinecraftLauncher.exe")) {
    if (Test-Path $candidate) { $icon = "$candidate,0"; break }
}

$shell = New-Object -ComObject WScript.Shell
$targets = @([Environment]::GetFolderPath("Programs"))
if ($Desktop) { $targets += [Environment]::GetFolderPath("Desktop") }
foreach ($folder in $targets) {
    $path = Join-Path $folder "$Name.lnk"
    $lnk = $shell.CreateShortcut($path)
    $lnk.TargetPath = $launcher
    $lnk.WorkingDirectory = $dir
    $lnk.Description = "minecraftCplusplus: title screen, worlds and servers"
    if ($icon) { $lnk.IconLocation = $icon }
    $lnk.Save()
    Write-Host "created $path"
}
Write-Host "Logs: $env:LOCALAPPDATA\minecraftCplusplus\logs\latest.log"
