<#
.SYNOPSIS
Read SpectraPack spec sections or requirement/acceptance rows without the full file.
.EXAMPLE
./tools/Read-Spec.ps1 -Section 5.2,5.3 -Id GEO-06,AT-09
#>
[CmdletBinding()]
param(
    [string[]] $Section = @(),
    [string[]] $Id = @()
)

$ErrorActionPreference = 'Stop'
$specPath = Join-Path $PSScriptRoot '../doc/spectrapack-spec.md'
$lines = @(Get-Content -LiteralPath $specPath -Encoding UTF8)
$headings = @(
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^(#{2,6})\s+(\d+(?:\.\d+)*)(?:\.)?\s') {
            [pscustomobject]@{ Index = $i; Level = $Matches[1].Length; Number = $Matches[2] }
        }
    }
)

if (-not $Section.Count -and -not $Id.Count) {
    $headings | ForEach-Object { $lines[$_.Index] }
    return
}

# Accept both PowerShell array arguments and comma-separated native-shell strings.
$selected = [System.Collections.Generic.SortedSet[int]]::new()
foreach ($number in ($Section -split ',')) {
    $number = $number.Trim()
    if (-not $number) { continue }
    $heading = @($headings | Where-Object Number -EQ $number)
    if ($heading.Count -ne 1) { throw "Unknown spec section: $number" }
    $start = $heading[0]
    $next = $headings | Where-Object { $_.Index -gt $start.Index -and $_.Level -le $start.Level } | Select-Object -First 1
    $end = if ($null -eq $next) { $lines.Count } else { $next.Index }
    for ($i = $start.Index; $i -lt $end; $i++) { [void] $selected.Add($i) }
}

foreach ($requirement in ($Id -split ',')) {
    $requirement = $requirement.Trim().ToUpperInvariant()
    if (-not $requirement) { continue }
    $pattern = '^\|\s*' + [regex]::Escape($requirement) + '\s*\|'
    $found = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match $pattern) {
            [void] $selected.Add($i)
            $found = $true
        }
    }
    if (-not $found) { throw "Unknown requirement/acceptance ID: $requirement" }
}

$previous = -2
foreach ($i in $selected) {
    if ($i -gt ($previous + 1)) { Write-Output "`n[spec line $($i + 1)]" }
    Write-Output $lines[$i]
    $previous = $i
}
