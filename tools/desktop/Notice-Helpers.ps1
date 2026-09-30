# Shared by the generator and its focused byte-preservation acceptance check.
function Test-NoticeLockEquivalent([string]$First, [string]$Second) {
    # Git's Windows checkout EOL conversion does not change the lock graph.
    [IO.File]::ReadAllText($First).Replace("`r`n","`n") -ceq [IO.File]::ReadAllText($Second).Replace("`r`n","`n")
}

function Get-NoticeFiles([string]$Directory, [string]$DeclaredPath, [string]$Key, [hashtable]$Supplemental, [string]$Repo) {
    $files = @{}
    if (Test-Path -LiteralPath $Directory -PathType Container) {
        foreach ($file in (Get-ChildItem -LiteralPath $Directory -File)) {
            if ($file.Name -match '^(LICENSE|LICENCE|COPYING|NOTICE)(?:[._-].*|$)') { $files[$file.FullName]=$file.FullName }
        }
    }
    if ($DeclaredPath) {
        $declared = if ([IO.Path]::IsPathRooted($DeclaredPath)) { $DeclaredPath } else { Join-Path $Directory $DeclaredPath }
        if (-not (Test-Path -LiteralPath $declared -PathType Leaf)) { throw "Declared license text missing for ${Key}: $DeclaredPath" }
        $files[$declared]=$declared
    }
    if ($files.Count -eq 0 -and $Supplemental.ContainsKey($Key)) {
        foreach ($source in $Supplemental[$Key]) {
            $path = Join-Path $Repo $source.path
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Supplemental license text missing for ${Key}: $($source.path)" }
            $files[$path]=$path
        }
    }
    if ($files.Count -eq 0) { throw "License text missing for $Key" }
    @($files.Values | Sort-Object)
}

function Copy-NoticeText([string]$SourcePath, [string]$OutputDirectory) {
    $hash = (Get-FileHash -LiteralPath $SourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $relative = "texts/$hash.txt"
    $destination = Join-Path $OutputDirectory $relative
    if (-not (Test-Path -LiteralPath $destination)) { Copy-Item -LiteralPath $SourcePath -Destination $destination }
    $relative
}

function Get-InstalledPackageDirectory([string]$FromDirectory, [string]$Name) {
    $current = $FromDirectory
    while ($current) {
        $candidate = Join-Path $current "node_modules/$Name"
        if (Test-Path -LiteralPath (Join-Path $candidate 'package.json')) {
            $entry = Get-Item -LiteralPath $candidate
            if ($entry.LinkType) {
                $target = [string]@($entry.Target)[0]
                if ([IO.Path]::IsPathRooted($target)) { return [IO.Path]::GetFullPath($target) }
                return [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $candidate) $target))
            }
            return $entry.FullName
        }
        $current = Split-Path -Parent $current
    }
    return $null
}
