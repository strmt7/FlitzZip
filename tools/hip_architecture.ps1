# Purpose: Resolve a bounded HIP architecture selection without allowing compiler or shell option injection.
# Inputs: A lower-case gfx target, comma-separated targets, or the repository's release preset.
# Outputs: Returns a canonical comma-separated list; rejects empty, duplicate, malformed, or excessive targets.
function Resolve-HipArchitecture {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Architecture)

    if ($Architecture -ceq "release") {
        $Architecture = "gfx1100,gfx1101,gfx1102,gfx1151,gfx1200,gfx1201"
    }
    if ($Architecture.Length -gt 256 -or $Architecture -cnotmatch '\Agfx[0-9a-z]+(,gfx[0-9a-z]+)*\z') {
        throw "HIP architecture must be a lower-case gfx target, comma-separated gfx targets, or release."
    }
    $targets = $Architecture.Split(',')
    if ($targets.Count -gt 16) {
        throw "HIP architecture selection exceeds the 16-target build limit."
    }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($target in $targets) {
        if (-not $seen.Add($target)) {
            throw "HIP architecture selection contains a duplicate target: $target"
        }
    }
    return $Architecture
}

# Purpose: Produce one explicit HIP compiler option for every validated target.
# Inputs: The same bounded architecture selection accepted by Resolve-HipArchitecture.
# Outputs: Returns a space-delimited argument fragment containing only validated --offload-arch options.
function Get-HipOffloadArgument {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Architecture)

    $resolved = Resolve-HipArchitecture -Architecture $Architecture
    return (($resolved.Split(',') | ForEach-Object { "--offload-arch=$_" }) -join ' ')
}
