# Purpose: Preserve one exact argument through the Windows CRT command-line parser.
# Inputs: Value is a possibly empty argument, including spaces, quotes, or trailing backslashes.
# Outputs: Returns a correctly quoted ProcessStartInfo.Arguments component.
function ConvertTo-FlitzZipWindowsArgument {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)

    if ($Value.IndexOf([char]0) -ge 0) { throw 'A Windows process argument cannot contain a null character.' }

    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($char in $Value.ToCharArray()) {
        if ($char -eq '\') { ++$slashes; continue }
        if ($char -eq '"') {
            [void]$builder.Append(('\' * ($slashes * 2 + 1)))
        } elseif ($slashes -gt 0) {
            [void]$builder.Append(('\' * $slashes))
        }
        [void]$builder.Append($char)
        $slashes = 0
    }
    if ($slashes -gt 0) { [void]$builder.Append(('\' * ($slashes * 2))) }
    [void]$builder.Append('"')
    return $builder.ToString()
}

