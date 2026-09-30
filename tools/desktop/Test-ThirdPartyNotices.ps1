# SYS-01 / SYS-04, ADR 0010: distribution notices use locked production packages
# and preserve distinct complete license/NOTICE bytes in an ignored directory.
[CmdletBinding()]
param([string]$CargoPath, [switch]$SkipStage)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$evidence = Join-Path $repo '.local/t008/notices-refactor'
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
$firstIndex = Join-Path $evidence 'first/index.md'
$arguments = @{ OutputPath='.local/t008/notices-refactor/first/index.md' }
if ($CargoPath) { $arguments.CargoPath=$CargoPath }
& (Join-Path $PSScriptRoot 'Generate-ThirdPartyNotices.ps1') @arguments
$index = [IO.File]::ReadAllText($firstIndex)
foreach ($name in @('react','three','scheduler')) {
    if ($index -notmatch "(?m)^\| npm \| $([regex]::Escape($name)) \|") { throw "Production package missing: $name" }
}
foreach ($name in @('vitest','jsdom','rolldown','@tauri-apps/cli')) {
    if ($index -match "(?m)^\| npm[^|]*\| $([regex]::Escape($name)) \|") { throw "Build/test package leaked into distribution notices: $name" }
}
if ($index -notmatch '(?m)^\| npm-generated \| vite \| 8\.3\.1 \|') { throw 'Vite generated runtime helpers need their pinned core notice.' }
if ($index -match '(?m)^\| npm \| vite \|') { throw 'Vite must be scoped as a generated runtime contribution.' }
if ($index -notmatch 'texts/[a-f0-9]{64}\.txt') { throw 'Distribution notices must reference complete content-addressed texts.' }
if ($index.Contains($repo) -or $index -match '(?<![A-Za-z])[A-Za-z]:[\\/]') { throw 'Distribution index contains a machine-specific path.' }
$firstDirectory=Split-Path -Parent $firstIndex
$references=@([regex]::Matches($index,'texts/([a-f0-9]{64})\.txt') | ForEach-Object { $_.Value } | Sort-Object -Unique)
$hashes=@{}
foreach ($relative in $references) {
    $path=Join-Path $firstDirectory $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Unresolvable text reference: $relative" }
    $hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($relative -ne "texts/$hash.txt") { throw "Changed source bytes or hash mismatch: $relative" }
    $hashes[$hash]=$true
}
if (@(Get-ChildItem (Join-Path $firstDirectory 'texts') -File).Count -ne $references.Count) { throw 'Unreferenced or duplicated text artifact.' }
# Independently check every included source license against its package row.
$metadata=Get-Content (Join-Path $repo '.local/t008/notices/cargo-metadata.json') -Raw | ConvertFrom-Json
$sourceCount=0
foreach ($line in ($index -split "`n" | Where-Object { $_ -match '^\| (npm|cargo) \|' })) {
    $columns=$line -split '\s*\|\s*'
    $ecosystem=$columns[1]; $name=$columns[2]; $version=$columns[3]
    if ($ecosystem -eq 'npm') {
        $sourceDirectory=(Get-Item (Join-Path $repo "node_modules/$name") -ErrorAction SilentlyContinue)
        if ($sourceDirectory) { $sourceDirectory=$sourceDirectory.FullName }
        else {
            $matches=@(Get-ChildItem (Join-Path $repo 'node_modules/.pnpm') -Directory | Where-Object { $_.Name -like "${name}@${version}*" })
            if ($matches.Count -ne 1) { throw "Cannot independently locate source: $name $version" }
            $sourceDirectory=Join-Path $matches[0].FullName "node_modules/$name"
        }
        $declared=(Get-Content (Join-Path $sourceDirectory 'package.json') -Raw | ConvertFrom-Json).license_file
    } else {
        $package=@($metadata.packages | Where-Object { $_.name -eq $name -and $_.version -eq $version })
        if ($package.Count -ne 1) { throw "Cannot independently locate crate: $name $version" }
        $sourceDirectory=Split-Path -Parent $package[0].manifest_path
        $declared=$package[0].license_file
    }
    $sourceFiles=@(Get-ChildItem -LiteralPath $sourceDirectory -File | Where-Object { $_.Name -match '^(LICENSE|LICENCE|COPYING|NOTICE)(?:[._-].*|$)' } | ForEach-Object FullName)
    if ($declared) { $sourceFiles+=Join-Path $sourceDirectory $declared }
    if (-not $sourceFiles) {
        $supplement=Get-Content (Join-Path $repo 'desktop/licenses/supplemental.json') -Raw | ConvertFrom-Json
        $sourceFiles=@($supplement.packages | Where-Object { $_.name -eq $name -and $_.version -eq $version } | ForEach-Object { $_.files } | ForEach-Object { Join-Path $repo $_.path })
    }
    foreach ($source in ($sourceFiles | Sort-Object -Unique)) {
        $hash=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant()
        if (-not $line.Contains("texts/$hash.txt")) { throw "Full license/NOTICE bytes absent from package row: $name $source" }
        $sourceCount++
    }
}
$core=Join-Path $repo 'desktop/licenses/vite-8.3.1-LICENSE-core.txt'
$coreHash=(Get-FileHash -LiteralPath $core).Hash.ToLowerInvariant()
if (-not $hashes.ContainsKey($coreHash)) { throw 'Generated runtime contribution bytes were lost.' }
if ($sourceCount -le $references.Count) { throw 'Real locked sources did not exercise cross-package deduplication.' }
$arguments.OutputPath='.local/t008/notices-refactor/second/index.md'
& (Join-Path $PSScriptRoot 'Generate-ThirdPartyNotices.ps1') @arguments
$secondDirectory=Join-Path $evidence 'second'
foreach ($relative in @('index.md')+$references) {
    if ((Get-FileHash (Join-Path $firstDirectory $relative)).Hash -ne (Get-FileHash (Join-Path $secondDirectory $relative)).Hash) {
        throw "Two generations differ: $relative"
    }
}
# Boundary fixture retains copyright, CRLF, trailing spaces, and a final blank line.
. (Join-Path $PSScriptRoot 'Notice-Helpers.ps1')
$boundary=Join-Path $evidence 'boundary'
New-Item -ItemType Directory -Force -Path $boundary,(Join-Path $boundary 'output/texts'),(Join-Path $boundary 'empty') | Out-Null
$notice=Join-Path $boundary 'NOTICE'
$license=Join-Path $boundary 'LICENSE'
[IO.File]::WriteAllBytes($notice,[Text.UTF8Encoding]::new($false).GetBytes("Copyright (c) 2026 Packaging boundary fixture`r`nFull NOTICE with trailing spaces  `r`n`r`n"))
Copy-Item -LiteralPath $notice -Destination $license -Force
$lfLock=Join-Path $boundary 'lock-lf.yaml'
$crlfLock=Join-Path $boundary 'lock-crlf.yaml'
[IO.File]::WriteAllText($lfLock,"lockfileVersion: '9.0'`npackages:`n")
[IO.File]::WriteAllText($crlfLock,"lockfileVersion: '9.0'`r`npackages:`r`n")
if (-not (Test-NoticeLockEquivalent $lfLock $crlfLock)) { throw 'LF/CRLF lockfiles must resolve to the same notice graph.' }
[IO.File]::AppendAllText($crlfLock,"  changed@1:`r`n")
if (Test-NoticeLockEquivalent $lfLock $crlfLock) { throw 'Substantively different installed lockfile must reject.' }
$files=@(Get-NoticeFiles $boundary '' 'boundary@1' @{} $repo)
foreach ($file in $files) { $null=Copy-NoticeText $file (Join-Path $boundary 'output') }
$outputs=@(Get-ChildItem (Join-Path $boundary 'output/texts') -File)
if ($outputs.Count -ne 1 -or (Get-FileHash $notice).Hash -ne (Get-FileHash $outputs[0].FullName).Hash) { throw 'NOTICE byte retention or exact-file deduplication failed.' }
$missing=$false
try { Get-NoticeFiles (Join-Path $boundary 'empty') '' 'missing@1' @{} $repo | Out-Null }
catch { if ($_.Exception.Message -eq 'License text missing for missing@1') { $missing=$true } else { throw } }
if (-not $missing) { throw 'An included package without license text must fail.' }
$missing=$false
try { Get-NoticeFiles $boundary 'absent-license' 'boundary@1' @{} $repo | Out-Null }
catch { if ($_.Exception.Message -like 'Declared license text missing*') { $missing=$true } else { throw } }
if (-not $missing) { throw 'An absent declared license text must fail even if another license file exists.' }
if (-not $SkipStage) {
    if (Test-Path (Join-Path $repo 'desktop/THIRD_PARTY_NOTICES.md')) { throw 'Remove the committed monolith before staging acceptance.' }
    $obsolete=Join-Path $repo 'desktop/src-tauri/engine-resources/desktop-THIRD_PARTY_NOTICES.md'
    [IO.File]::WriteAllText($obsolete,'Obsolete staging fixture')
    & (Join-Path $PSScriptRoot 'Stage-Desktop.ps1') -CargoPath $CargoPath
    if (Test-Path -LiteralPath $obsolete) { throw 'Obsolete monolithic resource survived staging.' }
    $staged=Join-Path $repo 'desktop/src-tauri/engine-resources/third-party'
    foreach ($relative in @('index.md')+$references) {
        if ((Get-FileHash (Join-Path $firstDirectory $relative)).Hash -ne (Get-FileHash (Join-Path $staged $relative)).Hash) { throw "Staged notices differ: $relative" }
    }
}
$size=(Get-ChildItem -LiteralPath $firstDirectory -Recurse -File | Measure-Object Length -Sum).Sum
Write-Output "PASS: locked scope, $sourceCount full source files, $($references.Count) distinct texts, exact NOTICE bytes, missing-text failures, deterministic generation, staging=$(-not $SkipStage); $size distribution bytes."
