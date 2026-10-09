param(
    [Parameter(Mandatory = $true)][string]$Target,
    [Parameter(Mandatory = $true)][ValidateSet('rename', 'delete', 'substitute')][string]$Mode
)

# Infrastructure for an actual competing process at the checked STL/JSON boundary.
# Its exit code is the underlying Win32 error, or zero if pathname mutation succeeds.
try {
    if ($Mode -eq 'delete') {
        [System.IO.File]::Delete($Target)
    } else {
        [System.IO.File]::Move($Target, $Target + '.displaced')
        if ($Mode -eq 'substitute') {
            [System.IO.File]::WriteAllText($Target, 'uncertified-substitute', [System.Text.Encoding]::ASCII)
        }
    }
    exit 0
} catch {
    $cause = $_.Exception
    while ($null -ne $cause.InnerException) { $cause = $cause.InnerException }
    [Console]::Error.WriteLine($cause.Message)
    exit ($cause.HResult -band 0xffff)
}
