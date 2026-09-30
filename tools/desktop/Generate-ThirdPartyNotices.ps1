[CmdletBinding()]
param(
    [string]$OutputPath = 'desktop/src-tauri/engine-resources/third-party/index.md',
    [string]$CargoPath,
    [string]$CargoMetadataPath = '.local/t008/notices/cargo-metadata.json'
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $PSScriptRoot 'Notice-Helpers.ps1')
Push-Location $repo
try {
    $output = if ([IO.Path]::IsPathRooted($OutputPath)) { [IO.Path]::GetFullPath($OutputPath) } else { [IO.Path]::GetFullPath((Join-Path $repo $OutputPath)) }
    $directory = Split-Path -Parent $output
    if (-not $directory.StartsWith($repo+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Generated notices must remain inside the workspace.' }
    $metadata = if ([IO.Path]::IsPathRooted($CargoMetadataPath)) { $CargoMetadataPath } else { Join-Path $repo $CargoMetadataPath }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $metadata) | Out-Null
    $env:RUSTUP_TOOLCHAIN = '1.98.1'
    if (-not $CargoPath) {
        $localCargo = Join-Path $repo '.local/t008/cargo/bin/cargo.exe'
        if (Test-Path -LiteralPath $localCargo) {
            $env:CARGO_HOME = Join-Path $repo '.local/t008/cargo'
            $env:RUSTUP_HOME = Join-Path $repo '.local/t008/rustup'
            $CargoPath=$localCargo
        } else { $CargoPath='cargo' }
    }
    $metadataArgs = @('metadata','--locked','--offline','--manifest-path','desktop/src-tauri/Cargo.toml','--features','custom-protocol','--format-version','1','--filter-platform','x86_64-pc-windows-msvc')
    $cargoJson = & $CargoPath @metadataArgs 2> (Join-Path (Split-Path -Parent $metadata) 'cargo-metadata.stderr.log')
    if ($LASTEXITCODE -ne 0) {
        # Cold build registries need locked sources before the offline inventory.
        & $CargoPath fetch --locked --manifest-path desktop/src-tauri/Cargo.toml
        if ($LASTEXITCODE -ne 0) { throw 'Locked Cargo source fetch failed.' }
        $cargoJson = & $CargoPath @metadataArgs
        if ($LASTEXITCODE -ne 0) { throw 'Offline locked Cargo metadata failed after fetch.' }
    }
    $cargoTree = & $CargoPath tree --locked --offline --manifest-path desktop/src-tauri/Cargo.toml --features custom-protocol --target x86_64-pc-windows-msvc --prefix none -e normal,build
    if ($LASTEXITCODE -ne 0) { throw 'Offline locked Cargo dependency tree failed.' }
    [IO.File]::WriteAllText($metadata,($cargoJson -join "`n")+"`n",[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path (Split-Path -Parent $metadata) 'cargo-tree.txt'),($cargoTree -join "`n")+"`n",[Text.UTF8Encoding]::new($false))
    $cargoData = ($cargoJson -join "`n") | ConvertFrom-Json
    $cargoIds = @{}
    foreach ($line in $cargoTree) {
        if ($line -match '^(.+?) v([0-9][^ ]*)(?: \(.*\))?$') { $cargoIds["$($Matches[1])@$($Matches[2])"]=$true }
    }
    if (-not (Test-Path 'node_modules/.pnpm/lock.yaml') -or
        -not (Test-NoticeLockEquivalent (Join-Path $repo 'pnpm-lock.yaml') (Join-Path $repo 'node_modules/.pnpm/lock.yaml'))) {
        throw 'Install frontend packages with the frozen pnpm lockfile before generating notices.'
    }
    $allowedPnpm = @{}
    $inPackages = $false
    foreach ($line in (Get-Content pnpm-lock.yaml)) {
        if ($line -eq 'packages:') { $inPackages=$true; continue }
        if ($inPackages -and $line -eq 'snapshots:') { break }
        if ($inPackages -and $line -match '^  (.+):$') {
            $key=$Matches[1].Trim().Trim([char[]]@([char]39,[char]34))
            if ($key -match '^(.+?)@(\d[^(@]*)(?:\(.*\))?$') { $allowedPnpm["$($Matches[1])@$($Matches[2])"]=$true }
        }
    }
    $rootPackage = Get-Content package.json -Raw | ConvertFrom-Json
    $queue = [Collections.Generic.Queue[object]]::new()
    foreach ($property in $rootPackage.dependencies.PSObject.Properties) {
        $queue.Enqueue([pscustomobject]@{ From=$repo; Name=$property.Name; Optional=$false })
    }
    foreach ($property in $rootPackage.optionalDependencies.PSObject.Properties) {
        $queue.Enqueue([pscustomobject]@{ From=$repo; Name=$property.Name; Optional=$true })
    }
    $frontend = @{}
    $visited = @{}
    while ($queue.Count) {
        $dependency=$queue.Dequeue()
        $packageDirectory=Get-InstalledPackageDirectory $dependency.From $dependency.Name
        if (-not $packageDirectory) {
            if ($dependency.Optional) { continue }
            throw "Installed production dependency missing: $($dependency.Name)"
        }
        if ($visited.ContainsKey($packageDirectory)) { continue }
        $visited[$packageDirectory]=$true
        $package=Get-Content (Join-Path $packageDirectory 'package.json') -Raw | ConvertFrom-Json
        $key="$($package.name)@$($package.version)"
        if (-not $allowedPnpm.ContainsKey($key)) { throw "Production package absent from pnpm lockfile: $key" }
        $frontend[$key]=[pscustomobject]@{ Name=$package.name; Version=$package.version; License=$package.license; Directory=$packageDirectory; Declared=$package.license_file; Source="https://www.npmjs.com/package/$($package.name)/v/$($package.version)"; Ecosystem='npm'; Reason='' }
        $optionalNames=@($package.optionalDependencies.PSObject.Properties.Name)
        foreach ($property in $package.dependencies.PSObject.Properties) {
            $queue.Enqueue([pscustomobject]@{ From=$packageDirectory; Name=$property.Name; Optional=($property.Name -in $optionalNames) })
        }
        foreach ($property in $package.optionalDependencies.PSObject.Properties) {
            $queue.Enqueue([pscustomobject]@{ From=$packageDirectory; Name=$property.Name; Optional=$true })
        }
        foreach ($property in $package.peerDependencies.PSObject.Properties) {
            $optionalPeer=$package.peerDependenciesMeta.($property.Name).optional -eq $true
            $queue.Enqueue([pscustomobject]@{ From=$packageDirectory; Name=$property.Name; Optional=$optionalPeer })
        }
    }
    $packages = [Collections.Generic.List[object]]::new()
    foreach ($package in ($frontend.Values | Sort-Object Name,Version)) { $packages.Add($package) }
    $supplementData=Get-Content desktop/licenses/supplemental.json -Raw | ConvertFrom-Json
    foreach ($contribution in ($supplementData.runtimeContributions | Sort-Object name,version)) {
        $key="$($contribution.name)@$($contribution.version)"
        if ($rootPackage.devDependencies.($contribution.name) -ne $contribution.version -or -not $allowedPnpm.ContainsKey($key)) {
            throw "Generated runtime contribution must match a pinned devDependency and lock key: $key"
        }
        $installed=Get-InstalledPackageDirectory $repo $contribution.name
        if (-not $installed) { throw "Generated runtime contribution package missing: $key" }
        $manifest=Get-Content (Join-Path $installed 'package.json') -Raw | ConvertFrom-Json
        if ($manifest.name -ne $contribution.name -or $manifest.version -ne $contribution.version) { throw "Generated runtime contribution installed version mismatch: $key" }
        if (-not $contribution.reason -or @($contribution.files).Count -eq 0) { throw "Generated runtime contribution needs files and reason: $key" }
        $files=@(foreach ($source in $contribution.files) {
            $path=Join-Path $repo $source.path
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Runtime contribution license text missing for ${key}: $($source.path)" }
            $path
        })
        $packages.Add([pscustomobject]@{ Name=$manifest.name; Version=$manifest.version; License=$manifest.license; Source="https://www.npmjs.com/package/$($manifest.name)/v/$($manifest.version)"; Ecosystem='npm-generated'; Reason=$contribution.reason; Files=$files })
    }
    foreach ($package in ($cargoData.packages | Where-Object { $_.source -like 'registry+*' -and $cargoIds.ContainsKey("$($_.name)@$($_.version)") } | Sort-Object name,version)) {
        $packages.Add([pscustomobject]@{ Name=$package.name; Version=$package.version; License=$package.license; Directory=(Split-Path -Parent $package.manifest_path); Declared=$package.license_file; Source="https://crates.io/crates/$($package.name)/$($package.version)"; Ecosystem='cargo'; Reason='' })
    }
    $supplemental=@{}
    $sources=@{}
    foreach ($package in $supplementData.packages) { $supplemental["$($package.name)@$($package.version)"]=@($package.files) }
    foreach ($package in (@($supplementData.packages)+@($supplementData.runtimeContributions))) {
        foreach ($source in $package.files) { $sources[(Join-Path $repo $source.path)]=$source }
    }
    # Validate the entire inventory before replacing previous successful output.
    foreach ($package in $packages) {
        if ($package.Ecosystem -eq 'npm-generated') { continue }
        $files=@(Get-NoticeFiles $package.Directory $package.Declared "$($package.Name)@$($package.Version)" $supplemental $repo)
        $package | Add-Member -NotePropertyName Files -NotePropertyValue $files
    }
    $texts=[IO.Path]::GetFullPath((Join-Path $directory 'texts'))
    if (-not $texts.StartsWith($repo+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Generated text directory must remain inside the workspace.' }
    if (Test-Path -LiteralPath $texts) { Remove-Item -LiteralPath $texts -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $texts | Out-Null
    $lines=[Collections.Generic.List[string]]::new()
    $lines.Add('# Third-party notices')
    $lines.Add('')
    $lines.Add('Frontend: installed production dependencies, including required peers and installed optional dependencies, checked against pnpm-lock.yaml; explicit generated runtime contributions are recorded separately. Rust: conservative Windows MSVC normal/build registry dependency closure from Cargo.lock, including build-time code generation; this is not an exact linked-binary inventory. Full source license/NOTICE files are preserved byte-for-byte and deduplicated only when their SHA-256 hashes match. No legal completeness claim is made.')
    $lines.Add('')
    $lines.Add('| Ecosystem | Package | Version | Declared license | Source | Full texts |')
    $lines.Add('| --- | --- | --- | --- | --- | --- |')
    foreach ($package in $packages) {
        $links=[Collections.Generic.List[string]]::new()
        foreach ($file in ($package.Files | Sort-Object)) {
            $relative=Copy-NoticeText $file $directory
            $link="[$([IO.Path]::GetFileName($file))]($relative)"
            if ($sources.ContainsKey($file)) {
                $source=$sources[$file]
                $details=[Collections.Generic.List[string]]::new()
                if ($source.sourceUrl) { $details.Add("[upstream]($($source.sourceUrl))") }
                foreach ($field in @('sourceRef','packageSourceCommit','sourceFile','sourceFileSha256','note')) {
                    if ($source.$field) { $details.Add([string]$source.$field) }
                }
                if ($details.Count) { $link+=' ('+($details -join '; ')+')' }
            }
            $links.Add($link)
        }
        $license=([string]$package.License).Replace('|','\|')
        $source="[package]($($package.Source))"
        if ($package.Reason) { $source+="; $($package.Reason)" }
        $lines.Add("| $($package.Ecosystem) | $($package.Name) | $($package.Version) | $license | $source | $($links -join '; ') |")
    }
    [IO.File]::WriteAllText($output,($lines -join "`n")+"`n",[Text.UTF8Encoding]::new($false))
    $nativeCount=@($packages | Where-Object Ecosystem -eq 'cargo').Count
    $distinct=@(Get-ChildItem -LiteralPath $texts -File).Count
    Write-Output "Generated $($frontend.Count) production frontend and $nativeCount Rust records, plus $(@($supplementData.runtimeContributions).Count) generated runtime contributions, with $distinct distinct full texts: $output"
} finally { Pop-Location }
