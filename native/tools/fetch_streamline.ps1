# Fetches the NVIDIA Streamline SDK into native\third_party\streamline (git-ignored, 276 MB download, 486 MB unpacked).
# The SDK is not committed: it is large and NVIDIA's licence (native\third_party\streamline\license.txt) is kept next to the
# binaries it covers. Without it the editor still builds and runs; only the NVIDIA features (Reflex, DLSS) are missing.
#
# Usage:  powershell -ExecutionPolicy Bypass -File native\tools\fetch_streamline.ps1 [-Force]
param([switch]$Force)
$ErrorActionPreference = 'Stop'

$version = '2.14.16'
$url     = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$version/streamline-sdk-v$version.zip"
$sha256  = '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b'
$target  = Join-Path $PSScriptRoot '..\third_party\streamline'
$target  = [System.IO.Path]::GetFullPath($target)

if ((Test-Path (Join-Path $target 'include\sl.h')) -and -not $Force) {
    Write-Host "Streamline is already at $target (use -Force to fetch again)."
    exit 0
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("streamline_fetch_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $zip = Join-Path $work "streamline-sdk-v$version.zip"
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    $actual = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
    if ($actual -ne $sha256) { throw "SHA-256 mismatch: expected $sha256, got $actual" }
    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    New-Item -ItemType Directory -Path $target | Out-Null
    Expand-Archive -Path $zip -DestinationPath $target
    Write-Host "Streamline $version unpacked to $target"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
