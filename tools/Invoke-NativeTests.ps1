[CmdletBinding()]
param(
    [ValidateSet('windows-ninja-debug', 'windows-ninja-release')]
    [string]$Preset = 'windows-ninja-debug',
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$VsWherePath = (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
    [string]$CmakePath = 'cmake',
    [string]$CtestPath = 'ctest',
    [string]$PythonPath = 'python',
    [string]$ToolchainLock = (Join-Path $PSScriptRoot '..\cmake\toolchains.lock.json'),
    [ValidateSet('local-windows-2026', 'hosted-windows-2025')]
    [string]$ToolchainProfile,
    [switch]$Fresh
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    throw 'Set VCPKG_ROOT or pass -VcpkgRoot before configuring.'
}
if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot 'scripts/buildsystems/vcpkg.cmake'))) {
    throw "VcpkgRoot does not contain scripts/buildsystems/vcpkg.cmake: $VcpkgRoot"
}

if ([string]::IsNullOrWhiteSpace($ToolchainProfile)) {
    $ToolchainProfile = if ($env:GITHUB_ACTIONS -eq 'true') { 'hosted-windows-2025' } else { 'local-windows-2026' }
}
if (-not (Test-Path -LiteralPath $ToolchainLock)) { throw "Toolchain lock was not found: $ToolchainLock" }
$toolchainProfiles = Get-Content -LiteralPath $ToolchainLock -Raw | ConvertFrom-Json
$profileLock = $toolchainProfiles.profiles.$ToolchainProfile
if ($null -eq $profileLock) { throw "Toolchain profile '$ToolchainProfile' is not locked in $ToolchainLock" }
$dependencyLockPath = Join-Path $PSScriptRoot '..\cmake\dependencies.lock.json'
$dependencyLock = Get-Content -LiteralPath $dependencyLockPath -Raw | ConvertFrom-Json
$vcpkgHead = (& git -C $VcpkgRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $vcpkgHead -ne $dependencyLock.baseline) { throw "vcpkg checkout expected $($dependencyLock.baseline), got $vcpkgHead" }
$vcpkgChanges = & git -C $VcpkgRoot status --porcelain -- ports scripts/cmake
if ($LASTEXITCODE -ne 0) { throw 'could not inspect vcpkg ports/scripts for local overrides.' }
if ($vcpkgChanges) { throw "vcpkg checkout has relevant local port/script overrides: $($vcpkgChanges -join '; ')" }

$installations = & $VsWherePath -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -version "[$($profileLock.vs_version),$($profileLock.vs_version)]" -format json
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installations)) {
    throw "vswhere could not find Visual Studio $($profileLock.vs_version) for locked profile '$ToolchainProfile'."
}
$installationPath = (($installations | ConvertFrom-Json) | Select-Object -First 1).installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installationPath)) {
    throw "vswhere could not find Visual Studio $($profileLock.vs_version) for locked profile '$ToolchainProfile'. Pass -VsWherePath if it is not on PATH."
}
$devCmd = Join-Path $installationPath.Trim() 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $devCmd)) { throw "VsDevCmd.bat was not found: $devCmd" }

# Import the Developer Command Prompt environment into this PowerShell process.
$developerEnvironment = cmd.exe /d /s /c "`"$devCmd`" -no_logo -arch=x64 -host_arch=x64 -vcvars_ver=$($profileLock.toolset) -winsdk=$($profileLock.windows_sdk) >nul && set"
if ($LASTEXITCODE -ne 0) { throw "VsDevCmd.bat failed with exit code $LASTEXITCODE" }
$importedEnvironment = [System.Collections.Generic.Dictionary[string, string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$developerEnvironment | ForEach-Object {
    $pair = $_.Split('=', 2)
    if ($pair.Count -eq 2) {
        if ($pair[0] -ieq 'Path' -and $importedEnvironment.ContainsKey('Path')) {
            $existingPath = $importedEnvironment['Path']
            if ($existingPath -notmatch '(?i)\\VC\\Tools\\' -and $pair[1] -match '(?i)\\VC\\Tools\\') {
                $importedEnvironment['Path'] = $pair[1]
            }
        } else {
            $importedEnvironment[$pair[0]] = $pair[1]
        }
    }
}
foreach ($pair in $importedEnvironment.GetEnumerator()) {
    if ($pair.Key -ieq 'Path') {
        Remove-Item -Path Env:Path -ErrorAction SilentlyContinue
        [Environment]::SetEnvironmentVariable('Path', $pair.Value, 'Process')
    } else {
        Set-Item -Path "Env:$($pair.Key)" -Value $pair.Value
    }
}
$env:VCPKG_ROOT = $VcpkgRoot
& (Join-Path $PSScriptRoot 'Prepare-VcpkgAssets.ps1') -VcpkgRoot $VcpkgRoot
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$metadataPath = Join-Path (Join-Path $PSScriptRoot '..\out\build') "$Preset\build-metadata.json"
$metadataDirectory = Split-Path -Parent $metadataPath
New-Item -ItemType Directory -Force -Path $metadataDirectory | Out-Null
$vsVersion = & $VsWherePath -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -version "[$($profileLock.vs_version),$($profileLock.vs_version)]" -property installationVersion
if ($LASTEXITCODE -ne 0) { throw "vswhere could not measure locked Visual Studio profile '$ToolchainProfile'." }
$vcpkgVersion = & (Join-Path $VcpkgRoot 'vcpkg.exe') version
if ($LASTEXITCODE -ne 0) { throw "vcpkg version failed with exit code $LASTEXITCODE" }
$vcpkgVersionText = ($vcpkgVersion | Select-Object -First 1) -replace '^vcpkg package management program version ', ''
$vcpkgExecutable = Join-Path $VcpkgRoot 'vcpkg.exe'
$vcpkgSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $vcpkgExecutable).Hash.ToLowerInvariant()
& $PythonPath (Join-Path $PSScriptRoot 'Inspect-CpuBuild.py') capture-toolchain --out $metadataPath --vs-version $vsVersion.Trim() --triplet $profileLock.triplet --vcpkg-tool-version $vcpkgVersionText.Trim() --vcpkg-tool-sha256 $vcpkgSha256 --cmake $CmakePath --ninja ninja
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $PythonPath (Join-Path $PSScriptRoot 'Inspect-CpuBuild.py') check-vcpkg-tool --lock (Join-Path $PSScriptRoot '..\cmake\dependencies.lock.json') --actual $metadataPath
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($Fresh) {
    & $CmakePath --fresh --preset $Preset "-DPython3_EXECUTABLE=$((Get-Command $PythonPath).Source)"
} else {
    & $CmakePath --preset $Preset "-DPython3_EXECUTABLE=$((Get-Command $PythonPath).Source)"
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $PythonPath (Join-Path $PSScriptRoot 'Inspect-CpuBuild.py') check-crt --build-type $(if ($Preset -eq 'windows-ninja-debug') { 'Debug' } else { 'Release' }) --cache (Join-Path $metadataDirectory 'CMakeCache.txt') --compile-commands (Join-Path $metadataDirectory 'compile_commands.json')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$compilerRecords = @(Get-ChildItem -LiteralPath (Join-Path $metadataDirectory 'CMakeFiles') -Filter 'CMakeCXXCompiler.cmake' -File -Recurse)
if ($compilerRecords.Count -ne 1) {
    throw "Expected exactly one generated CMake CXX compiler record, found $($compilerRecords.Count)."
}
& $PythonPath (Join-Path $PSScriptRoot 'Inspect-CpuBuild.py') record-build --metadata $metadataPath --cache (Join-Path $metadataDirectory 'CMakeCache.txt') --compiler-record $compilerRecords[0].FullName --profile $ToolchainProfile --build-type $(if ($Preset -eq 'windows-ninja-debug') { 'Debug' } else { 'Release' })
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $PythonPath (Join-Path $PSScriptRoot 'Inspect-CpuBuild.py') check-toolchain --lock $ToolchainLock --profile $ToolchainProfile --actual $metadataPath
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$installedRoot = Join-Path $metadataDirectory 'vcpkg_installed'
& $PythonPath (Join-Path $PSScriptRoot 'Inspect-CpuBuild.py') collect-dependencies --status (Join-Path $installedRoot 'vcpkg\status') --installed-root $installedRoot --out (Join-Path $metadataDirectory 'installed-dependencies.json')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $PythonPath (Join-Path $PSScriptRoot 'Inspect-CpuBuild.py') check-dependencies --lock (Join-Path $PSScriptRoot '..\cmake\dependencies.lock.json') --actual (Join-Path $metadataDirectory 'installed-dependencies.json')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $CmakePath --build --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $CtestPath --preset $Preset --timeout 30
exit $LASTEXITCODE
