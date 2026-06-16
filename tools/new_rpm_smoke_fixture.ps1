param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [Parameter(Mandatory = $true)]
    [string]$PayloadPath,

    [ValidateSet("none")]
    [string]$PayloadCompressor = "none"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $PayloadPath -PathType Leaf)) {
    throw "RPM smoke fixture payload does not exist: $PayloadPath"
}

# Purpose: Append a 32-bit big-endian RPM field.
# Inputs: Bytes receives encoded output and Value is the decoded integer.
# Outputs: Appends four bytes.
function Add-RpmBigEndianUInt32 {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.List[byte]]$Bytes,
        [Parameter(Mandatory = $true)]
        [uint32]$Value
    )
    [void]$Bytes.Add([byte](($Value -shr 24) -band 0xFF))
    [void]$Bytes.Add([byte](($Value -shr 16) -band 0xFF))
    [void]$Bytes.Add([byte](($Value -shr 8) -band 0xFF))
    [void]$Bytes.Add([byte]($Value -band 0xFF))
}

# Purpose: Append one RPM header index entry.
# Inputs: Header receives bytes; Tag, Type, Offset, and Count are RPM index fields.
# Outputs: Appends a 16-byte index record.
function Add-RpmIndexEntry {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.List[byte]]$Header,
        [Parameter(Mandatory = $true)]
        [uint32]$Tag,
        [Parameter(Mandatory = $true)]
        [uint32]$Type,
        [Parameter(Mandatory = $true)]
        [uint32]$Offset,
        [Parameter(Mandatory = $true)]
        [uint32]$Count
    )
    Add-RpmBigEndianUInt32 -Bytes $Header -Value $Tag
    Add-RpmBigEndianUInt32 -Bytes $Header -Value $Type
    Add-RpmBigEndianUInt32 -Bytes $Header -Value $Offset
    Add-RpmBigEndianUInt32 -Bytes $Header -Value $Count
}

# Purpose: Create a minimal RPM header containing string tags.
# Inputs: Strings is an array of objects with Tag and Value properties.
# Outputs: Returns a complete RPM header byte array.
function New-RpmStringHeader {
    param(
        [AllowEmptyCollection()]
        [object[]]$Strings = @()
    )

    $store = [System.Collections.Generic.List[byte]]::new()
    $entries = @()
    foreach ($entry in $Strings) {
        $tag = [uint32]$entry.Tag
        $value = [string]$entry.Value
        $entries += [pscustomobject]@{
            Tag = $tag
            Offset = [uint32]$store.Count
        }
        foreach ($byte in [Text.Encoding]::ASCII.GetBytes($value)) {
            [void]$store.Add($byte)
        }
        [void]$store.Add([byte]0)
    }

    $header = [System.Collections.Generic.List[byte]]::new()
    foreach ($byte in [byte[]](0x8E, 0xAD, 0xE8, 0x01, 0, 0, 0, 0)) {
        [void]$header.Add($byte)
    }
    Add-RpmBigEndianUInt32 -Bytes $header -Value ([uint32]$entries.Count)
    Add-RpmBigEndianUInt32 -Bytes $header -Value ([uint32]$store.Count)
    foreach ($entry in $entries) {
        Add-RpmIndexEntry -Header $header -Tag $entry.Tag -Type 6 -Offset $entry.Offset -Count 1
    }
    foreach ($byte in $store) {
        [void]$header.Add($byte)
    }
    return $header.ToArray()
}

# Purpose: Append zero padding until a byte list is eight-byte aligned.
# Inputs: Bytes receives zero padding.
# Outputs: Adds zero to seven bytes.
function Add-RpmEightBytePadding {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.List[byte]]$Bytes
    )
    while (($Bytes.Count % 8) -ne 0) {
        [void]$Bytes.Add([byte]0)
    }
}

$payloadBytes = [IO.File]::ReadAllBytes($PayloadPath)
$rpm = [System.Collections.Generic.List[byte]]::new()
for ($index = 0; $index -lt 96; ++$index) {
    [void]$rpm.Add([byte]0)
}
$rpm[0] = 0xED
$rpm[1] = 0xAB
$rpm[2] = 0xEE
$rpm[3] = 0xDB
$rpm[4] = 3
$rpm[5] = 0
$rpm[6] = 0
$rpm[7] = 0
$nameBytes = [Text.Encoding]::ASCII.GetBytes("superzip-release-smoke")
for ($index = 0; $index -lt $nameBytes.Length -and $index -lt 66; ++$index) {
    $rpm[10 + $index] = $nameBytes[$index]
}

foreach ($byte in (New-RpmStringHeader -Strings @())) {
    [void]$rpm.Add($byte)
}
Add-RpmEightBytePadding -Bytes $rpm

$mainHeader = New-RpmStringHeader -Strings @(
    [pscustomobject]@{ Tag = 1124; Value = "cpio" },
    [pscustomobject]@{ Tag = 1125; Value = $PayloadCompressor.ToLowerInvariant() }
)
foreach ($byte in $mainHeader) {
    [void]$rpm.Add($byte)
}
foreach ($byte in $payloadBytes) {
    [void]$rpm.Add($byte)
}

$parentDirectory = Split-Path -Parent $OutputPath
if ($parentDirectory) {
    New-Item -ItemType Directory -Force -Path $parentDirectory | Out-Null
}
[IO.File]::WriteAllBytes($OutputPath, $rpm.ToArray())
