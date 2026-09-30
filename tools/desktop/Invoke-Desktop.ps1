[CmdletBinding()]
param(
    [ValidateSet('Build','Test','Dev')][string]$Mode = 'Build',
    [string]$EngineBuildDirectory = 'out/build/windows-ninja-release',
    [string]$CargoPath,
    [string]$PnpmPath = 'pnpm',
    [string]$TestStlPath,
    [switch]$DebugBuild,
    [switch]$SkipFrontend
)
$ErrorActionPreference = 'Stop'
$taskRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Push-Location $taskRepo
try {
    $env:RUSTUP_TOOLCHAIN = '1.98.1'
    if (-not $CargoPath) {
        $taskLocalCargo = Join-Path $taskRepo '.local/t008/cargo/bin/cargo.exe'
        if (Test-Path -LiteralPath $taskLocalCargo) {
            $env:CARGO_HOME = Join-Path $taskRepo '.local/t008/cargo'
            $env:RUSTUP_HOME = Join-Path $taskRepo '.local/t008/rustup'
            $env:Path = (Join-Path $env:CARGO_HOME 'bin')+';'+$env:Path
            $CargoPath = $taskLocalCargo
        } else { $CargoPath = 'cargo' }
    }
    & (Join-Path $PSScriptRoot 'Stage-Desktop.ps1') -EngineBuildDirectory $EngineBuildDirectory
    if ($Mode -eq 'Test') {
        if (-not $TestStlPath) { throw 'TestStlPath must name a real 10mm analytic fixture.' }
        $env:SPECTRAPACK_TEST_ENGINE = Join-Path $taskRepo "$EngineBuildDirectory/bin/spectrapack-engine.exe"
        $env:SPECTRAPACK_TEST_STL = [IO.Path]::GetFullPath($TestStlPath)
        & $CargoPath test --manifest-path desktop/src-tauri/Cargo.toml --locked --no-default-features
        if ($LASTEXITCODE -ne 0) { throw 'Desktop native-core tests failed.' }
    } elseif ($Mode -eq 'Dev') {
        & $PnpmPath desktop:native dev
        if ($LASTEXITCODE -ne 0) { throw 'Desktop development shell failed.' }
    } else {
        if (-not $SkipFrontend) {
            & $PnpmPath desktop:build
            if ($LASTEXITCODE -ne 0) { throw 'Desktop frontend build failed.' }
        }
        $taskBuildArgs = @('build','--manifest-path','desktop/src-tauri/Cargo.toml','--locked','--features','custom-protocol')
        if (-not $DebugBuild) { $taskBuildArgs += '--release' }
        & $CargoPath @taskBuildArgs
        if ($LASTEXITCODE -ne 0) { throw 'Desktop shell build failed.' }
        $taskProfile = if ($DebugBuild) { 'debug' } else { 'release' }
        $taskOutput = Join-Path $taskRepo "desktop/src-tauri/target/$taskProfile"
        Copy-Item -LiteralPath (Join-Path $taskRepo 'desktop/src-tauri/binaries/spectrapack-engine-x86_64-pc-windows-msvc.exe') -Destination (Join-Path $taskOutput 'spectrapack-engine.exe') -Force
        $taskResources = [IO.Path]::GetFullPath((Join-Path $taskOutput 'engine-resources'))
        if (-not $taskResources.StartsWith($taskRepo+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Build resources must remain inside the workspace.' }
        if (Test-Path -LiteralPath $taskResources) { Remove-Item -LiteralPath $taskResources -Recurse -Force }
        Copy-Item -LiteralPath (Join-Path $taskRepo 'desktop/src-tauri/engine-resources') -Destination $taskResources -Recurse -Force
        Write-Output "Native shell: $(Join-Path $taskOutput 'spectrapack-desktop.exe')"
    }
} finally { Pop-Location }
