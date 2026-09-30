[CmdletBinding()]
param(
    [string]$EngineBuildDirectory = 'out/build/windows-ninja-release',
    [string]$StageDirectory = '.local/t008/desktop-stage',
    [string]$CmakePath = 'cmake',
    [string]$CargoPath
)
$ErrorActionPreference = 'Stop'
$taskRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$taskBuild = [IO.Path]::GetFullPath((Join-Path $taskRepo $EngineBuildDirectory))
$taskStage = [IO.Path]::GetFullPath((Join-Path $taskRepo $StageDirectory))
if (-not (Test-Path -LiteralPath (Join-Path $taskBuild 'bin/spectrapack-engine.exe'))) { throw 'Build the qualified native engine first.' }
New-Item -ItemType Directory -Force -Path $taskStage | Out-Null
& $CmakePath --install $taskBuild --prefix $taskStage --component engine
if ($LASTEXITCODE -ne 0) { throw 'Engine staging failed.' }
$taskNative = Join-Path $taskRepo 'desktop/src-tauri'
New-Item -ItemType Directory -Force (Join-Path $taskNative 'binaries'),(Join-Path $taskNative 'engine-resources') | Out-Null
$taskEngine = Join-Path $taskStage 'bin/spectrapack-engine.exe'
Copy-Item -LiteralPath $taskEngine -Destination (Join-Path $taskNative 'binaries/spectrapack-engine-x86_64-pc-windows-msvc.exe') -Force
$taskShare = [IO.Path]::GetFullPath((Join-Path $taskNative 'engine-resources/share'))
if (-not $taskShare.StartsWith($taskRepo+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Staged resources must remain inside the workspace.' }
if (Test-Path -LiteralPath $taskShare) { Remove-Item -LiteralPath $taskShare -Recurse -Force }
Copy-Item -LiteralPath (Join-Path $taskStage 'share') -Destination $taskShare -Recurse -Force
& (Join-Path $PSScriptRoot 'Generate-ThirdPartyNotices.ps1') -CargoPath $CargoPath
$taskObsoleteNotices = [IO.Path]::GetFullPath((Join-Path $taskNative 'engine-resources/desktop-THIRD_PARTY_NOTICES.md'))
if (-not $taskObsoleteNotices.StartsWith($taskRepo+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Obsolete notice resource must remain inside the workspace.' }
if (Test-Path -LiteralPath $taskObsoleteNotices) { Remove-Item -LiteralPath $taskObsoleteNotices -Force }
$taskHash = (Get-FileHash -LiteralPath $taskEngine -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $taskNative 'engine-resources/engine.sha256'),$taskHash+"`n",[Text.UTF8Encoding]::new($false))
$taskCapabilities = & $taskEngine capabilities --json
if ($LASTEXITCODE -ne 0) { throw 'Staged engine capabilities failed.' }
$taskCapabilities | Set-Content -LiteralPath (Join-Path $taskNative 'engine-resources/capabilities.json') -Encoding utf8
Write-Output "Staged fixed engine $taskHash with runtime notices and measured native build metadata."
