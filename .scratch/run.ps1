# Runs the game once with a script under the machine-wide game lock; keeps options.txt byte-identical.
param([string]$Script, [string]$Shaders = "off", [int]$Timeout = 170, [string]$World = "fxtest", [string]$Extra = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\fx\minecraftCplusplus.exe"
$lock = "C:\Users\Admin\AppData\Local\Temp\mcpp-game.lock"
$saves = "C:\Users\Admin\AppData\Local\Temp\mcpp-fx\saves"
New-Item -ItemType Directory -Force $saves | Out-Null
$opts = Join-Path $env:LOCALAPPDATA "minecraftCplusplus\options.txt"
$backup = Join-Path $PSScriptRoot "options.backup"

# Acquire the lock (stale after 10 minutes).
while ($true) {
    try {
        New-Item -ItemType Directory $lock -ErrorAction Stop | Out-Null
        Set-Content -Path (Join-Path $lock "owner.txt") -Value "fx-agent (view effects) $(Get-Date -Format o)"
        break
    } catch {
        $age = (Get-Date) - (Get-Item $lock).CreationTime
        if ($age.TotalMinutes -gt 10) { Remove-Item -Recurse -Force $lock; continue }
        Write-Host "lock busy ($([int]$age.TotalSeconds)s), waiting"
        Start-Sleep -Seconds 15
    }
}
try {
    $hadOpts = Test-Path $opts
    if ($hadOpts) { Copy-Item $opts $backup -Force }
    $gargs = @("--play", "--world", $World, "--saves-dir", $saves, "--script", $Script, "--size", "1280x720", "--shaders", $Shaders)
    if (-not (Test-Path (Join-Path $saves "$World\level.dat"))) {
        $gargs += @("--create-name", $World, "--create-mode", "creative", "--create-cheats", "1", "--create-seed", "424242")
    }
    if ($Extra) { $gargs += $Extra.Split(" ") }
    $log = Join-Path $PSScriptRoot "game.log"
    $p = Start-Process -FilePath $exe -ArgumentList $gargs -WorkingDirectory (Split-Path $exe) -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    if (-not $p.WaitForExit($Timeout * 1000)) { Write-Host "timeout: killing"; $p.Kill(); $p.WaitForExit() }
    Write-Host "exit $($p.ExitCode)"
    Start-Sleep -Seconds 2
    Get-CimInstance Win32_Process -Filter "Name='java.exe' or Name='javaw.exe'" | Where-Object { $_.CommandLine -like "*mcpp-fx*" } | ForEach-Object { Write-Host "killing orphan server $($_.ProcessId)"; Stop-Process -Id $_.ProcessId -Force }
} finally {
    if ($hadOpts) {
        $a = (Get-FileHash $opts).Hash; $b = (Get-FileHash $backup).Hash
        if ($a -ne $b) { Copy-Item $backup $opts -Force; Write-Host "options.txt restored" } else { Write-Host "options.txt unchanged" }
    } elseif (Test-Path $opts) { Remove-Item $opts; Write-Host "options.txt removed (created by the run)" }
    Remove-Item -Recurse -Force $lock
}
