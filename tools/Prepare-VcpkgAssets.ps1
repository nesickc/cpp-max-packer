[CmdletBinding()]
param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$DownloadsRoot = $env:VCPKG_DOWNLOADS
)

$ErrorActionPreference = 'Stop'

$patchName = '0034c113477f83c28d4380de1ee189c25b1168e6.patch'
$expectedSha512 = '5c165b50813b0d9937ff0eb4d4a81e2d1e77718ac3b0d02b93931c8eddb4e06e4fae1822c5cc97a5b01c995916a29d0af03fcbcd8f059cb29cfeb0e2371b15e3'
$seedPath = Join-Path $PSScriptRoot "..\third_party\vcpkg\$patchName"

# Validate the repository source before creating or replacing anything in the cache.
if (-not (Test-Path -LiteralPath $seedPath -PathType Leaf)) {
    throw "Required committed vcpkg patch seed is missing: $seedPath"
}
$actualSha512 = (Get-FileHash -Algorithm SHA512 -LiteralPath $seedPath).Hash.ToLowerInvariant()
if ($actualSha512 -ne $expectedSha512) {
    throw "Committed vcpkg patch seed SHA512 mismatch: expected $expectedSha512, got $actualSha512 ($seedPath)"
}

if ([string]::IsNullOrWhiteSpace($DownloadsRoot)) {
    if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
        throw 'Set VCPKG_DOWNLOADS, set VCPKG_ROOT, or pass -DownloadsRoot/-VcpkgRoot before preparing vcpkg assets.'
    }
    $DownloadsRoot = Join-Path $VcpkgRoot 'downloads'
}

New-Item -ItemType Directory -Force -Path $DownloadsRoot | Out-Null
$destination = Join-Path $DownloadsRoot $patchName
Copy-Item -LiteralPath $seedPath -Destination $destination -Force
Write-Output "Prepared locked vcpkg download asset: $destination"
