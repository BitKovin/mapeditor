# Fetches the AMD FidelityFX SDK pieces the editor uses into native\third_party\fidelityfx (git-ignored, about 36 MB): the FidelityFX API headers, the upscaler header,
# the licence, and the two signed Vulkan DLLs (the loader and the upscaler effect). Without them the editor still builds and runs; only FSR (the upscaler for every GPU) is missing.
# Each file is checked against a pinned SHA-256.
#
# Usage:  powershell -ExecutionPolicy Bypass -File native\tools\fetch_fidelityfx.ps1 [-Force]
param([switch]$Force)
$ErrorActionPreference = 'Stop'

$tag    = 'v2.4.0'
$base   = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/$tag"
$target = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\third_party\fidelityfx'))

# local path (under $target) = path in the SDK repository, SHA-256
$files = @(
    @('api/include/ffx_api.h',                  'Kits/FidelityFX/api/include/ffx_api.h',                  '4d9dcf243ef5efbf03340a02f16b51a22053243a22e8a9a0b3ce91c26982b4bf'),
    @('api/include/ffx_api.hpp',                'Kits/FidelityFX/api/include/ffx_api.hpp',                'af441486342abfeb189c5672f3216a28446b582ad314ec4e754b81e20bb8a840'),
    @('api/include/ffx_api_loader.h',           'Kits/FidelityFX/api/include/ffx_api_loader.h',           '18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e'),
    @('api/include/ffx_api_types.h',            'Kits/FidelityFX/api/include/ffx_api_types.h',            '53f07c113a9ba493f297656e4481b6539aed0afc9f3cc8da682aa477d270c46c'),
    @('api/include/ffx_api_matrix_helpers.h',   'Kits/FidelityFX/api/include/ffx_api_matrix_helpers.h',   'f37055b1980562b75295ac0ee3f4b60dd314c8c2f21a41b7d71d9f9070858bd5'),
    @('api/include/ffx_api_matrix_helpers.hpp', 'Kits/FidelityFX/api/include/ffx_api_matrix_helpers.hpp', '8f671dadf9e1267dd8a51d224e11b27656ddb9da51ef9524a177b89114548737'),
    @('api/include/vk/ffx_api_vk.h',            'Kits/FidelityFX/api/include/vk/ffx_api_vk.h',            '09ac1855bc4c2f711243d6982909385072ca3f8d30db44d1a82ee5bd637832ad'),
    @('api/include/vk/ffx_api_vk.hpp',          'Kits/FidelityFX/api/include/vk/ffx_api_vk.hpp',          '7757c3ffa6b1e189bca5eec3b8c93aa8f802386642648a0dfeb8c2c52502e5ba'),
    @('upscalers/include/ffx_upscale.h',        'Kits/FidelityFX/upscalers/include/ffx_upscale.h',        '13091e7fe5fd9c82b1fb7c044b9f08f664d9653f599d2ea500738709236fca4d'),
    @('upscalers/include/ffx_upscale.hpp',      'Kits/FidelityFX/upscalers/include/ffx_upscale.hpp',      '52066379fd629080ed5c9a2821627cd7a3b972e6592565c7f3fa54a95404bac0'),
    @('license.md',                             'docs/license.md',                                        '410f9c138e766e846acf2dc93db768198def0293a017d7e3296815b451688dbe'),
    @('bin/amd_fidelityfx_loader_vk.dll',       'Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll',   '38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9'),
    @('bin/amd_fidelityfx_upscaler_vk.dll',     'Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll', '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7')
)

if ((Test-Path (Join-Path $target 'api\include\ffx_api.h')) -and (Test-Path (Join-Path $target 'bin\amd_fidelityfx_upscaler_vk.dll')) -and -not $Force) {
    Write-Host "FidelityFX is already at $target (use -Force to fetch again)."
    exit 0
}

New-Item -ItemType Directory -Path $target -Force | Out-Null
foreach ($f in $files) {
    $dest = Join-Path $target ($f[0] -replace '/', '\')
    New-Item -ItemType Directory -Path (Split-Path $dest) -Force | Out-Null
    $url = "$base/$($f[1])"
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
    $actual = (Get-FileHash -Algorithm SHA256 $dest).Hash.ToLowerInvariant()
    if ($actual -ne $f[2]) {
        Remove-Item -Force $dest
        throw "SHA-256 mismatch for $($f[0]): expected $($f[2]), got $actual"
    }
}
Write-Host "FidelityFX SDK $tag unpacked to $target"
