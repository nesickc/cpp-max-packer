[CmdletBinding()]
param(
    [ValidateSet('Build','Installer','Test','Dev')][string]$Mode = 'Build',
    [string]$EngineBuildDirectory = 'out/build/windows-ninja-release',
    [string]$CargoPath,
    [string]$PnpmPath = 'pnpm',
    [string]$TestStlPath,
    [switch]$DebugBuild,
    [switch]$SkipFrontend
)
# Build leaves a runnable desktop in bin/release (bin/debug with -DebugBuild):
# spectrapack-desktop.exe beside spectrapack-engine.exe and engine-resources, the
# same layout the setup installs. Installer also packs that release build into a
# versioned NSIS setup with the offline WebView2 installer, in bin/installer.
# Both shells take the Release engine unless -EngineBuildDirectory names another
# build: a Debug engine packs far slower.
$ErrorActionPreference = 'Stop'
$taskRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Push-Location $taskRepo
try {
    if ($Mode -eq 'Installer' -and $DebugBuild) { throw 'The installer packs the release shell; omit -DebugBuild.' }
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
    & (Join-Path $PSScriptRoot 'Stage-Desktop.ps1') -EngineBuildDirectory $EngineBuildDirectory -CargoPath $CargoPath
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
        # The product version is the CMake project version the staged engine reports;
        # the shell and the setup take it from there, so the three cannot drift.
        $taskVersion = (Get-Content -Raw -LiteralPath 'desktop/src-tauri/engine-resources/capabilities.json' | ConvertFrom-Json).engine.version
        if ($taskVersion -notmatch '^\d+\.\d+\.\d+$') { throw "Staged engine reports no release version: '$taskVersion'." }
        $taskVersionConfig = '{"version":"'+$taskVersion+'"}'
        $env:TAURI_CONFIG = $taskVersionConfig
        $taskBuildArgs = @('build','--manifest-path','desktop/src-tauri/Cargo.toml','--locked','--features','custom-protocol')
        if (-not $DebugBuild) { $taskBuildArgs += '--release' }
        & $CargoPath @taskBuildArgs
        if ($LASTEXITCODE -ne 0) { throw 'Desktop shell build failed.' }
        $taskProfile = if ($DebugBuild) { 'debug' } else { 'release' }
        $taskBin = Join-Path $taskRepo "bin/$taskProfile"
        if (Test-Path -LiteralPath $taskBin) { Remove-Item -LiteralPath $taskBin -Recurse -Force }
        New-Item -ItemType Directory -Force $taskBin | Out-Null
        Copy-Item -LiteralPath "desktop/src-tauri/target/$taskProfile/spectrapack-desktop.exe" -Destination $taskBin
        Copy-Item -LiteralPath 'desktop/src-tauri/binaries/spectrapack-engine-x86_64-pc-windows-msvc.exe' -Destination (Join-Path $taskBin 'spectrapack-engine.exe')
        Copy-Item -LiteralPath 'desktop/src-tauri/engine-resources' -Destination (Join-Path $taskBin 'engine-resources') -Recurse
        Write-Output "SpectraPack ${taskVersion}: $(Join-Path $taskBin 'spectrapack-desktop.exe')"
        if ($Mode -eq 'Installer') {
            # The bundler packs the shell built above with the staged sidecar and
            # resources and downloads the offline WebView2 installer. Without binary
            # patching the installed shell is byte-identical to bin/release.
            $taskConfigFile = Join-Path $taskRepo 'desktop/src-tauri/target/version.conf.json'
            [IO.File]::WriteAllText($taskConfigFile,$taskVersionConfig,[Text.UTF8Encoding]::new($false))
            Push-Location 'desktop/src-tauri'
            try {
                & (Join-Path $taskRepo 'node_modules/.bin/tauri.cmd') bundle --bundles nsis --config $taskConfigFile --no-binary-patching
                $taskBundleResult = $LASTEXITCODE
            } finally { Pop-Location }
            if ($taskBundleResult -ne 0) { throw 'Desktop installer bundling failed.' }
            $taskSetup = Join-Path $taskRepo "desktop/src-tauri/target/release/bundle/nsis/SpectraPack_${taskVersion}_x64-setup.exe"
            if (-not (Test-Path -LiteralPath $taskSetup)) { throw "The bundler did not write $taskSetup." }
            $taskInstaller = Join-Path $taskRepo 'bin/installer'
            if (Test-Path -LiteralPath $taskInstaller) { Remove-Item -LiteralPath $taskInstaller -Recurse -Force }
            New-Item -ItemType Directory -Force $taskInstaller | Out-Null
            Copy-Item -LiteralPath $taskSetup -Destination $taskInstaller
            Write-Output "Setup: $(Join-Path $taskInstaller (Split-Path -Leaf $taskSetup))"
        }
    }
} finally {
    Remove-Item -LiteralPath Env:TAURI_CONFIG -ErrorAction SilentlyContinue
    Pop-Location
}
