param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [Parameter(Mandatory = $true)]
    [string]$EntryName,

    [Parameter(Mandatory = $true)]
    [string]$Payload
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($EntryName) -or $EntryName.Contains([char]0)) {
    throw "CAB smoke fixture entry name must be a non-empty NUL-free path."
}

$nameBytes = [Text.Encoding]::ASCII.GetBytes($EntryName)
$payloadBytes = [Text.Encoding]::UTF8.GetBytes($Payload)
if ($payloadBytes.Length -gt [uint16]::MaxValue) {
    throw "CAB smoke fixture payload is too large for the single-block fixture."
}

# Purpose: Append a 16-bit little-endian CAB field.
# Inputs: Bytes receives encoded output and Value is the decoded integer.
# Outputs: Appends two bytes.
function Add-CabLittleEndianUInt16 {
    param(
        [AllowEmptyCollection()]
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.List[byte]]$Bytes,
        [Parameter(Mandatory = $true)]
        [uint16]$Value
    )
    [void]$Bytes.Add([byte]($Value -band 0xFF))
    [void]$Bytes.Add([byte](($Value -shr 8) -band 0xFF))
}

# Purpose: Append a 32-bit little-endian CAB field.
# Inputs: Bytes receives encoded output and Value is the decoded integer.
# Outputs: Appends four bytes.
function Add-CabLittleEndianUInt32 {
    param(
        [AllowEmptyCollection()]
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.List[byte]]$Bytes,
        [Parameter(Mandatory = $true)]
        [uint32]$Value
    )
    [void]$Bytes.Add([byte]($Value -band 0xFF))
    [void]$Bytes.Add([byte](($Value -shr 8) -band 0xFF))
    [void]$Bytes.Add([byte](($Value -shr 16) -band 0xFF))
    [void]$Bytes.Add([byte](($Value -shr 24) -band 0xFF))
}

# Purpose: Append raw bytes to a byte list.
# Inputs: Bytes receives output and Data provides the bytes to copy.
# Outputs: Appends every byte in order.
function Add-CabBytes {
    param(
        [AllowEmptyCollection()]
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.List[byte]]$Bytes,
        [Parameter(Mandatory = $true)]
        [byte[]]$Data
    )
    foreach ($byte in $Data) {
        [void]$Bytes.Add($byte)
    }
}

$headerBytes = 36
$folderBytes = 8
$fileRecordBytes = 16 + $nameBytes.Length + 1
$dataHeaderBytes = 8
$fileTableOffset = [uint32]($headerBytes + $folderBytes)
$dataOffset = [uint32]($fileTableOffset + $fileRecordBytes)
$cabinetSize = [uint32]($dataOffset + $dataHeaderBytes + $payloadBytes.Length)

$cab = [System.Collections.Generic.List[byte]]::new([int]$cabinetSize)
Add-CabBytes -Bytes $cab -Data ([byte[]][char[]]"MSCF")
Add-CabLittleEndianUInt32 -Bytes $cab -Value 0
Add-CabLittleEndianUInt32 -Bytes $cab -Value $cabinetSize
Add-CabLittleEndianUInt32 -Bytes $cab -Value 0
Add-CabLittleEndianUInt32 -Bytes $cab -Value $fileTableOffset
Add-CabLittleEndianUInt32 -Bytes $cab -Value 0
[void]$cab.Add([byte]3)
[void]$cab.Add([byte]1)
Add-CabLittleEndianUInt16 -Bytes $cab -Value 1
Add-CabLittleEndianUInt16 -Bytes $cab -Value 1
Add-CabLittleEndianUInt16 -Bytes $cab -Value 0
Add-CabLittleEndianUInt16 -Bytes $cab -Value 0x1234
Add-CabLittleEndianUInt16 -Bytes $cab -Value 0

Add-CabLittleEndianUInt32 -Bytes $cab -Value $dataOffset
Add-CabLittleEndianUInt16 -Bytes $cab -Value 1
Add-CabLittleEndianUInt16 -Bytes $cab -Value 0

Add-CabLittleEndianUInt32 -Bytes $cab -Value ([uint32]$payloadBytes.Length)
Add-CabLittleEndianUInt32 -Bytes $cab -Value 0
Add-CabLittleEndianUInt16 -Bytes $cab -Value 0
Add-CabLittleEndianUInt16 -Bytes $cab -Value 0
Add-CabLittleEndianUInt16 -Bytes $cab -Value 0
Add-CabLittleEndianUInt16 -Bytes $cab -Value 0x20
Add-CabBytes -Bytes $cab -Data $nameBytes
[void]$cab.Add([byte]0)

Add-CabLittleEndianUInt32 -Bytes $cab -Value 0
Add-CabLittleEndianUInt16 -Bytes $cab -Value ([uint16]$payloadBytes.Length)
Add-CabLittleEndianUInt16 -Bytes $cab -Value ([uint16]$payloadBytes.Length)
Add-CabBytes -Bytes $cab -Data $payloadBytes

if ($cab.Count -ne $cabinetSize) {
    throw "CAB smoke fixture size mismatch."
}

$parentDirectory = Split-Path -Parent $OutputPath
if ($parentDirectory) {
    New-Item -ItemType Directory -Force -Path $parentDirectory | Out-Null
}
[IO.File]::WriteAllBytes($OutputPath, $cab.ToArray())
