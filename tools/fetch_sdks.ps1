# Downloads the vendor upscaler / frame generation SDKs (pinned releases, official GitHub sources only), verifies
# their SHA-256 and unpacks the parts the game uses into third_party\{streamline,fidelityfx,xess}.
# Usage: powershell -File tools\fetch_sdks.ps1 [-Force]   (-Force re-downloads and overwrites)
# The binaries (third_party\*\bin, ~230 MB) are git-ignored; CMake copies them next to the exe.
param([switch]$Force)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root "third_party"
$cache = Join-Path $root "build\sdk_downloads"
New-Item -ItemType Directory -Force $cache | Out-Null

function Get-Verified([string]$url, [string]$sha256, [string]$file) {
    if ($Force -or -not (Test-Path $file) -or (Get-FileHash -Algorithm SHA256 $file).Hash -ne $sha256.ToUpper()) {
        Write-Host "downloading $url"
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $file
    }
    $h = (Get-FileHash -Algorithm SHA256 $file).Hash
    if ($h -ne $sha256.ToUpper()) { Remove-Item $file; throw "SHA-256 mismatch for $url`n  expected $sha256`n  got      $h" }
    Write-Host "  ok  $([IO.Path]::GetFileName($file))  sha256 $sha256"
}

# Extracts zip entries whose full name starts with one of $prefixes (keeping the path below the prefix) into $dest.
function Expand-Selected([string]$zip, [hashtable]$map) {
    $z = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        foreach ($e in $z.Entries) {
            if ($e.FullName.EndsWith("/")) { continue }
            foreach ($prefix in $map.Keys) {
                if ($e.FullName.StartsWith($prefix)) {
                    $rel = $e.FullName.Substring($prefix.Length)
                    if ($rel.Contains("/")) { continue }  # only files directly inside the prefix folder
                    $out = Join-Path $map[$prefix] $rel
                    New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
                    [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
                }
            }
        }
    } finally { $z.Dispose() }
}

# ---- NVIDIA Streamline 2.14.1 (DLSS 4 SR/DLAA, DLSS Ray Reconstruction, DLSS Frame Generation / Multi Frame
# Generation, Reflex, PCL) ----
# https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1 (digest published by GitHub for the release asset)
$sl = Join-Path $cache "streamline-sdk-v2.14.1.zip"
Get-Verified "https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.1/streamline-sdk-v2.14.1.zip" `
    "92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b" $sl
$slDir = Join-Path $tp "streamline"
Expand-Selected $sl @{ "include/" = "$slDir\include"; "lib/x64/" = "$slDir\lib" }
$z = [IO.Compression.ZipFile]::OpenRead($sl)
try {
    # Production (signed) binaries only: the interposer, the common plugin and the features we use + NGX models.
    $want = "sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "sl.dlss_d.dll", "sl.dlss_g.dll", "sl.reflex.dll",
            "sl.pcl.dll", "nvngx_dlss.dll", "nvngx_dlssd.dll", "nvngx_dlssg.dll"
    foreach ($e in $z.Entries) {
        $n = $e.FullName
        if ($n -like "bin/x64/*" -and $n.Split("/").Count -eq 3 -and $want -contains $e.Name) {
            New-Item -ItemType Directory -Force "$slDir\bin" | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, "$slDir\bin\$($e.Name)", $true)
        }
        if ($n -in "license.txt", "3rd-party-licenses.md", "bin/x64/nvngx_dlss.license.txt", "bin/x64/reflex.license.txt") {
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, "$slDir\$($e.Name)", $true)
        }
    }
} finally { $z.Dispose() }

# ---- AMD FidelityFX / FSR SDK 2.3.0 (FSR Upscaling 4.1.1 w/ FSR 3.1 fallback, FSR Frame Generation, swap chain) ----
# https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.3.0 - prebuilt signed DLLs
$ffx = Join-Path $cache "FidelityFX-Samples-v2.3.0-prebuilt.zip"
Get-Verified "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v2.3.0/FidelityFX-Samples-v2.3.0-prebuilt.zip" `
    "f90890b9323bb2f4f2404ac4cdc9395e8495ecdac6f7aa0bcdf1ad1848422273" $ffx
$ffxDir = Join-Path $tp "fidelityfx"
$z = [IO.Compression.ZipFile]::OpenRead($ffx)
try {
    $want = "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll", "amd_fidelityfx_framegeneration_dx12.dll"
    foreach ($e in $z.Entries) {
        if ($e.FullName -like "*Upscalers/FidelityFX_FSR/dx12/x64/Release/*" -and $want -contains $e.Name) {
            New-Item -ItemType Directory -Force "$ffxDir\bin" | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, "$ffxDir\bin\$($e.Name)", $true)
        }
    }
} finally { $z.Dispose() }
# API headers (not in the prebuilt package): the same release tag's source tree, pinned to its commit.
$commit = "60f4ea81909200d8542eca14dccb2628b763a9a3"  # tag v2.3.0
$headers = @(
    @('Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h', 'c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530'),
    @('Kits/FidelityFX/api/include/dx12/ffx_api_dx12.hpp', '31972deed9c5b94243eb72c97e24c1b4e1a0171ae0d0109f7fe842b6efdeddc7'),
    @('Kits/FidelityFX/api/include/ffx_api.h', 'e1a9d1b559eaf75f4cb401f1c43a66e2f3dc9621abfeb58b9c6fac56443177fd'),
    @('Kits/FidelityFX/api/include/ffx_api.hpp', 'e1fab07fa9ab489db74813e6b754fc91dad6ded68870ab558002e92b6a32566a'),
    @('Kits/FidelityFX/api/include/ffx_api_loader.h', '18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e'),
    @('Kits/FidelityFX/api/include/ffx_api_types.h', 'cc0adc0ffc804dd7cd03c39c15581b575e416383177ffe95060201d52cc0fc49'),
    @('Kits/FidelityFX/upscalers/include/ffx_upscale.h', '65be36d5653eb71c10379e0204f7bb7cd6a8aeec96e6f8cffc5f59ffeb1f89bf'),
    @('Kits/FidelityFX/upscalers/include/ffx_upscale.hpp', '52066379fd629080ed5c9a2821627cd7a3b972e6592565c7f3fa54a95404bac0'),
    @('Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h', 'd533810bbd6fd4f87dfa2c1657c1457c7b1597b66345827d375b081d67127f66'),
    @('Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.hpp', 'c4a4e2d7a97e92216d66d698216dcff690babce58e92671296353f479e27139c'),
    @('Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h', 'e6c42f8a38aab2c7c4fc6931545a8e5463fdf87349902ec9687d9fe4949a0566'),
    @('Kits/FidelityFX/framegeneration/include/ffx_framegeneration.hpp', '826ba0a0385971ff5aa5651b51d4f4315c1ec40cbc391e2c4b48f4fe00a449cb'),
    @('Kits/FidelityFX/framegeneration/include/ffx_framegeneration_api_types.h', '396596af3df63310674e8036abd2ad0fcd061e61334e7a2c76653a2be3c686b4'),
    @('Kits/OpenSource/amd/antilag2/ffx_antilag2_dx12.h', '0554c804cfb02442974b90cf1887031bfd18d41fba2054a2cd23c8ee65e6265f'),
    @('Kits/OpenSource/amd/antilag2/LICENSE.txt', 'b516b8fe7e52d5afe96045f43266c6d2fd4c10963cb624bcf1142b615d3ad4a7'),
    @('Kits/FidelityFX/docs/license.md', '6757f2fec461238da6f84f83536ec195e8c036b1773a06f6f34301b9ed8c0963')
)
foreach ($h in $headers) {
    $rel = $h[0] -replace '^Kits/FidelityFX/', '' -replace '^Kits/OpenSource/amd/antilag2/', 'antilag2/'
    if ($rel -eq 'docs/license.md') { $rel = 'license.md' }
    $out = Join-Path $ffxDir ($rel -replace '/', '\')
    New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
    Get-Verified "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/$commit/$($h[0])" $h[1] $out
}

# ---- Intel XeSS SDK 3.0.2 (XeSS-SR 3, XeSS-FG, XeLL 1.3.2) ----
# https://github.com/intel/xess/releases/tag/v3.0.2
$xs = Join-Path $cache "XeSS_SDK_3.0.2.zip"
Get-Verified "https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip" `
    "88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990" $xs
$xsDir = Join-Path $tp "xess"
Expand-Selected $xs @{ "inc/xess/" = "$xsDir\inc\xess"; "inc/xess_fg/" = "$xsDir\inc\xess_fg"; "inc/xell/" = "$xsDir\inc\xell" }
$z = [IO.Compression.ZipFile]::OpenRead($xs)
try {
    foreach ($e in $z.Entries) {
        if ($e.FullName -in "bin/libxess.dll", "bin/libxess_fg.dll", "bin/libxell.dll") {
            New-Item -ItemType Directory -Force "$xsDir\bin" | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, "$xsDir\bin\$($e.Name)", $true)
        }
        if ($e.FullName -in "lib/libxess.lib", "lib/libxess_fg.lib", "lib/libxell.lib") {
            New-Item -ItemType Directory -Force "$xsDir\lib" | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, "$xsDir\lib\$($e.Name)", $true)
        }
        if ($e.FullName -in "LICENSE.txt", "third-party-programs.txt") {
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, "$xsDir\$($e.Name)", $true)
        }
    }
} finally { $z.Dispose() }

Write-Host "SDKs ready in $tp (re-run CMake / build.ps1 to copy the DLLs next to the exe)"
