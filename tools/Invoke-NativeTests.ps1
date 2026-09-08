[CmdletBinding()]
param(
    [ValidateSet('windows-ninja-debug', 'windows-ninja-release')]
    [string]$Preset = 'windows-ninja-debug',
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$VsWherePath = (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
    [string]$CmakePath = 'cmake',
    [string]$CtestPath = 'ctest',
    [switch]$Fresh
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    throw 'Set VCPKG_ROOT or pass -VcpkgRoot before configuring.'
}
if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot 'scripts/buildsystems/vcpkg.cmake'))) {
    throw "VcpkgRoot does not contain scripts/buildsystems/vcpkg.cmake: $VcpkgRoot"
}

$installationPath = & $VsWherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installationPath)) {
    throw 'vswhere could not find a Visual Studio C++ installation. Pass -VsWherePath if it is not on PATH.'
}
$devCmd = Join-Path $installationPath.Trim() 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $devCmd)) { throw "VsDevCmd.bat was not found: $devCmd" }

# Import the Developer Command Prompt environment into this PowerShell process.
$developerEnvironment = cmd.exe /d /s /c "`"$devCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
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
if ($Fresh) {
    & $CmakePath --fresh --preset $Preset
} else {
    & $CmakePath --preset $Preset
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $CmakePath --build --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $CtestPath --preset $Preset
exit $LASTEXITCODE
