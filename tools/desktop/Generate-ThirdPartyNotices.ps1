[CmdletBinding()]
param(
    [string]$OutputPath = 'desktop/THIRD_PARTY_NOTICES.md',
    [string]$CargoHome = '.local/t008/cargo',
    [string]$RustupHome = '.local/t008/rustup',
    [string]$CargoMetadataPath = '.local/t008/notices/cargo-metadata.json'
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Set-Location $repo
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputPath))
$cargo = [IO.Path]::GetFullPath((Join-Path $repo $CargoHome))
$rustup = [IO.Path]::GetFullPath((Join-Path $repo $RustupHome))
$metadata = [IO.Path]::GetFullPath((Join-Path $repo $CargoMetadataPath))
$env:CARGO_HOME = $cargo
$env:RUSTUP_HOME = $rustup
$cargoExe = Join-Path $cargo 'bin/cargo.exe'
if (-not (Test-Path -LiteralPath $cargoExe)) { throw "Task-local Cargo not found: $cargoExe" }
$metadataArgs = @('metadata','--locked','--offline','--manifest-path','desktop/src-tauri/Cargo.toml','--format-version','1','--filter-platform','x86_64-pc-windows-msvc')
$cargoJson = & $cargoExe @metadataArgs
if ($LASTEXITCODE -ne 0) { throw 'Offline locked Cargo metadata failed.' }
$treeArgs = @('tree','--locked','--offline','--manifest-path','desktop/src-tauri/Cargo.toml','--target','x86_64-pc-windows-msvc','--prefix','none','-e','normal,build')
$cargoTree = & $cargoExe @treeArgs
if ($LASTEXITCODE -ne 0) { throw 'Offline locked Cargo dependency tree failed.' }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $metadata) | Out-Null
[IO.File]::WriteAllText($metadata,($cargoJson -join "`n") + "`n",[Text.UTF8Encoding]::new($false))
$cargoData = $cargoJson -join "`n" | ConvertFrom-Json
$treeFile = Join-Path (Split-Path -Parent $metadata) 'cargo-tree.txt'
[IO.File]::WriteAllText($treeFile,($cargoTree -join "`n") + "`n",[Text.UTF8Encoding]::new($false))
$cargoIds = @{}
foreach ($line in $cargoTree) {
    if ($line -match '^(.+?) v([0-9][^ ]*)(?: \(.*\))?$') { $cargoIds["$($Matches[1])@$($Matches[2])"]=$true }
}
$rootPkg = Get-Content 'package.json' -Raw | ConvertFrom-Json
$allowedPnpm = @{}
$inPackages = $false
foreach ($line in (Get-Content 'pnpm-lock.yaml')) {
    if ($line -eq 'packages:') { $inPackages=$true; continue }
    if ($inPackages -and $line -match '^  (.+):$') {
        $lockKey = $Matches[1].Trim().Trim([char[]]@([char]39,[char]34))
        if ($lockKey -match '^(.+?)@(\d[^(@]*)(?:\(.*\))?$') { $allowedPnpm["$($Matches[1])@$($Matches[2])"]=$true }
    }
    elseif ($inPackages -and $line -match '^snapshots:') { break }
}
$licenseNamePattern = '^(LICENSE|LICENCE|COPYING|NOTICE)(?:[._-].*|$)'
function Get-LicenseFiles([string]$directory, [string]$declaredPath) {
    $found = @{}
    if (Test-Path -LiteralPath $directory) {
        Get-ChildItem -LiteralPath $directory -File | Where-Object { $_.Name -match $licenseNamePattern } | ForEach-Object { $found[$_.FullName]=$_.FullName }
    }
    if ($declaredPath) {
        $declared = if ([IO.Path]::IsPathRooted($declaredPath)) { $declaredPath } else { Join-Path $directory $declaredPath }
        if (Test-Path -LiteralPath $declared -PathType Leaf) { $found[[IO.Path]::GetFullPath($declared)] = [IO.Path]::GetFullPath($declared) }
    }
    @($found.Values | Sort-Object)
}
function Add-LicenseText([Collections.Generic.List[string]]$lines, [string]$path) {
    $content = [IO.File]::ReadAllText($path).TrimEnd()
    foreach ($line in ($content -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) { $lines.Add('') }
        else { $lines.Add('    '+$line.TrimEnd()) }
    }
}
$supplementFile = Join-Path $repo 'desktop/licenses/supplemental.json'
$supplemental = @{}
$supplementalSources = @{}
if (Test-Path -LiteralPath $supplementFile) {
    $supplementData = Get-Content -LiteralPath $supplementFile -Raw | ConvertFrom-Json
    foreach ($package in $supplementData.packages) { $supplemental["$($package.name)@$($package.version)"] = @($package.files) }
}
function Get-PackageLicenseFiles([string]$key, [string[]]$localFiles) {
    if ($localFiles.Count -gt 0) { return $localFiles }
    if (-not $supplemental.ContainsKey($key)) { return @() }
    $files = [Collections.Generic.List[string]]::new()
    foreach ($source in $supplemental[$key]) {
        $path = [IO.Path]::GetFullPath((Join-Path $repo $source.path))
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $files.Add($path)
            $supplementalSources[$path] = $source
        }
    }
    @($files)
}
$root = Join-Path $repo 'node_modules/.pnpm'
$frontend = @{}
foreach ($dir in Get-ChildItem $root -Directory) {
    $modules = Join-Path $dir.FullName 'node_modules'
    if (-not (Test-Path $modules)) { continue }
    $pkgDirs = [Collections.Generic.List[IO.DirectoryInfo]]::new()
    foreach ($moduleEntry in Get-ChildItem $modules -Directory -ErrorAction SilentlyContinue) {
        if ($moduleEntry.Name.StartsWith('@')) {
            foreach ($scopedPackage in Get-ChildItem $moduleEntry.FullName -Directory -ErrorAction SilentlyContinue) { $pkgDirs.Add($scopedPackage) }
        } else { $pkgDirs.Add($moduleEntry) }
    }
    foreach ($pkgDir in $pkgDirs) {
        $manifest = Join-Path $pkgDir.FullName 'package.json'
        if (-not (Test-Path $manifest)) { continue }
        try { $pkg = Get-Content $manifest -Raw | ConvertFrom-Json } catch { continue }
        if (-not $pkg.name -or -not $pkg.version) { continue }
        $key = "$($pkg.name)@$($pkg.version)"
        if (-not $allowedPnpm.ContainsKey($key)) { continue }
        if ($frontend.ContainsKey($key)) { continue }
        $textFiles = @(Get-PackageLicenseFiles $key @(Get-LicenseFiles $pkgDir.FullName ([string]$pkg.license_file)))
        $frontend[$key] = [pscustomobject]@{ Name=$pkg.name; Version=$pkg.version; License=[string]$pkg.license; Repository=$pkg.repository; Dir=$pkgDir.FullName; Files=$textFiles; Runtime=([string]$pkg.name -in @('@tauri-apps/api','react','react-dom','three')); RuntimeTransitive=([string]$pkg.name -eq 'scheduler') }
    }
}
$direct = @($rootPkg.dependencies.PSObject.Properties.Name) + @($rootPkg.devDependencies.PSObject.Properties.Name)
$directSet = @{}; foreach ($name in $direct) { $directSet[$name]=$true }
$lines = [Collections.Generic.List[string]]::new()
$lines.Add('# Third-party notices')
$lines.Add('')
$lines.Add('Generated from installed pnpm packages matched to package keys in `pnpm-lock.yaml` and the Windows MSVC-target crates.io normal/build dependency closure. The frontend section reflects packages present in this installation; it does not claim coverage of every lockfile entry. Source metadata and upstream license files are retained where available.')
$lines.Add('')
$lines.Add('## Frontend and JavaScript packages')
$lines.Add('')
$lines.Add('The direct runtime packages are `@tauri-apps/api`, `react`, `react-dom`, and `three`. Installed locked packages found in the pnpm store are listed below, including available frontend transitive dependencies and build/test tooling. Package source: npm registry; exact versions are pinned by `pnpm-lock.yaml`.')
$lines.Add('')
foreach ($entry in ($frontend.Values | Sort-Object Name,Version)) {
    $kind = if ($entry.Runtime) { 'direct runtime' } elseif ($entry.RuntimeTransitive) { 'transitive runtime' } elseif ($directSet.ContainsKey($entry.Name)) { 'direct tooling' } else { 'installed locked package (runtime/tooling)' }
    $sourceLine = "- **$($entry.Name) $($entry.Version)** ($kind; SPDX: $($entry.License)) — npm package."
    $lines.Add($sourceLine)
    $repository = if ($entry.Repository -is [string]) { $entry.Repository } elseif ($entry.Repository.url) { [string]$entry.Repository.url } else { '' }
    if ($repository) { $lines.Add("  - Upstream repository: $repository") }
    if ($entry.Files.Count -eq 0) { $lines.Add('  - License text unavailable in the installed package; identifier above is from package.json.') }
    foreach ($file in $entry.Files) {
        $lines.Add('  - `'+[IO.Path]::GetFileName($file)+'`:')
        if ($supplementalSources.ContainsKey($file)) {
            $source = $supplementalSources[$file]
            $lines.Add("    Source: $($source.sourceUrl) ($($source.sourceRef)).")
            if ($source.packageSourceCommit) { $lines.Add("    Pinned package source commit: $($source.packageSourceCommit).") }
            if ($source.note) { $lines.Add("    Note: $($source.note)") }
        }
        $lines.Add('')
        Add-LicenseText $lines $file
        $lines.Add('')
    }
}
$frontendMissing = @($frontend.Values | Where-Object { $_.Files.Count -eq 0 } | Sort-Object Name,Version)
$lines.Add('## Missing upstream license texts')
$lines.Add('')
$lines.Add("$($frontendMissing.Count) installed locked frontend packages have no recognized or declared license text in the local package. Their package license identifiers are retained above where supplied; no text is inferred.")
foreach ($entry in $frontendMissing) { $lines.Add("- $($entry.Name) $($entry.Version)") }
$lines.Add('')
$lines.Add('## Native Rust packages')
$lines.Add('')
$lines.Add('The list below is the Windows MSVC-target normal and build dependency closure from `Cargo.lock`, resolved using `cargo tree --locked --offline -e normal,build`. It covers crates.io packages used by the application and its build scripts. Checksums are in `desktop/src-tauri/Cargo.lock`.')
$lines.Add('')
$nativeMissing = [Collections.Generic.List[string]]::new()
foreach ($pkg in ($cargoData.packages | Where-Object { $_.source -like 'registry+*' -and $cargoIds.ContainsKey("$($_.name)@$($_.version)") } | Sort-Object name,version)) {
    $class = 'Windows MSVC normal/build dependency'
    $lic = if ($pkg.license) { $pkg.license } else { 'license metadata unspecified' }
    $lines.Add("- **$($pkg.name) $($pkg.version)** ($class; SPDX: $lic) — crates.io.")
    if ($pkg.repository) { $lines.Add("  - Upstream repository: $($pkg.repository)") }
    if ($pkg.homepage) { $lines.Add("  - Homepage: $($pkg.homepage)") }
    $manifestDir = $pkg.manifest_path | Split-Path -Parent
    $packageKey = "$($pkg.name)@$($pkg.version)"
    $files = @(Get-PackageLicenseFiles $packageKey @(Get-LicenseFiles $manifestDir ([string]$pkg.license_file)))
    if (-not $files) { $lines.Add('  - License text unavailable in the local registry source cache; no text has been inferred.'); $nativeMissing.Add("$($pkg.name) $($pkg.version)") }
    foreach ($file in $files) {
        $lines.Add('  - `'+[IO.Path]::GetFileName($file)+'`:')
        if ($supplementalSources.ContainsKey($file)) {
            $source = $supplementalSources[$file]
            $lines.Add("    Source: $($source.sourceUrl) ($($source.sourceRef)).")
            if ($source.packageSourceCommit) { $lines.Add("    Pinned package source commit: $($source.packageSourceCommit).") }
            if ($source.note) { $lines.Add("    Note: $($source.note)") }
        }
        $lines.Add('')
        Add-LicenseText $lines $file
        $lines.Add('')
    }
}
$lines.Add('## Native packages missing upstream license texts')
$lines.Add('')
$lines.Add("$($nativeMissing.Count) packages in the selected crates.io dependency closure have no recognized or declared license text in the local registry cache.")
foreach ($entry in $nativeMissing) { $lines.Add("- $entry") }
$lines.Add('')
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $output) | Out-Null
[IO.File]::WriteAllText($output,($lines -join "`n") + "`n",[Text.UTF8Encoding]::new($false))
Write-Output "Generated $output from pnpm-lock.yaml and Cargo.lock using offline locked metadata."
