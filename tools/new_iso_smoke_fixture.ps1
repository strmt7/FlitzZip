param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [Parameter(Mandatory = $true)]
    [string]$Payload
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# Purpose: Write an ISO 9660 both-endian 32-bit field.
# Inputs: Buffer receives bytes, Offset is the first field byte, and Value is the decoded number.
# Outputs: Writes little-endian and big-endian copies.
function Set-IsoBothEndianUInt32 {
    param([byte[]]$Buffer, [int]$Offset, [uint32]$Value)
    $Buffer[$Offset + 0] = [byte]($Value -band 0xFF)
    $Buffer[$Offset + 1] = [byte](($Value -shr 8) -band 0xFF)
    $Buffer[$Offset + 2] = [byte](($Value -shr 16) -band 0xFF)
    $Buffer[$Offset + 3] = [byte](($Value -shr 24) -band 0xFF)
    $Buffer[$Offset + 4] = [byte](($Value -shr 24) -band 0xFF)
    $Buffer[$Offset + 5] = [byte](($Value -shr 16) -band 0xFF)
    $Buffer[$Offset + 6] = [byte](($Value -shr 8) -band 0xFF)
    $Buffer[$Offset + 7] = [byte]($Value -band 0xFF)
}

# Purpose: Write an ISO 9660 both-endian 16-bit field.
# Inputs: Buffer receives bytes, Offset is the first field byte, and Value is the decoded number.
# Outputs: Writes little-endian and big-endian copies.
function Set-IsoBothEndianUInt16 {
    param([byte[]]$Buffer, [int]$Offset, [uint16]$Value)
    $Buffer[$Offset + 0] = [byte]($Value -band 0xFF)
    $Buffer[$Offset + 1] = [byte](($Value -shr 8) -band 0xFF)
    $Buffer[$Offset + 2] = [byte](($Value -shr 8) -band 0xFF)
    $Buffer[$Offset + 3] = [byte]($Value -band 0xFF)
}

# Purpose: Build one minimal ISO 9660 directory record.
# Inputs: ExtentSector/DataLength describe payload location, Flags controls file/directory bits, and Identifier is the raw ISO file identifier.
# Outputs: Returns a padded directory record byte array.
function New-IsoRecord {
    param([uint32]$ExtentSector, [uint32]$DataLength, [byte]$Flags, [byte[]]$Identifier)
    $rawLength = 33 + $Identifier.Length
    $recordLength = $rawLength + ($rawLength % 2)
    $record = [byte[]]::new($recordLength)
    $record[0] = [byte]$recordLength
    Set-IsoBothEndianUInt32 -Buffer $record -Offset 2 -Value $ExtentSector
    Set-IsoBothEndianUInt32 -Buffer $record -Offset 10 -Value $DataLength
    $record[25] = $Flags
    Set-IsoBothEndianUInt16 -Buffer $record -Offset 28 -Value 1
    $record[32] = [byte]$Identifier.Length
    [Array]::Copy($Identifier, 0, $record, 33, $Identifier.Length)
    return $record
}

# Purpose: Append one directory record to a directory sector in the image.
# Inputs: Image receives bytes, Sector selects the directory extent, Cursor is advanced, and Record is copied into place.
# Outputs: Mutates Image and Cursor.
function Add-IsoDirectoryRecord {
    param([byte[]]$Image, [uint32]$Sector, [ref]$Cursor, [byte[]]$Record)
    $offset = ([int]$Sector * 2048) + [int]$Cursor.Value
    if (($offset + $Record.Length) -gt $Image.Length) {
        throw "ISO smoke fixture directory record exceeds image bounds."
    }
    [Array]::Copy($Record, 0, $Image, $offset, $Record.Length)
    $Cursor.Value = [int]$Cursor.Value + $Record.Length
}

$image = [byte[]]::new(32 * 2048)
$payloadBytes = [Text.Encoding]::UTF8.GetBytes($Payload)
if ($payloadBytes.Length -gt 2048) {
    throw "ISO smoke fixture payload must fit in one sector."
}

$current = [byte[]](0)
$parent = [byte[]](1)
$rootRecord = New-IsoRecord -ExtentSector 20 -DataLength 2048 -Flags 2 -Identifier $current

$pvd = 16 * 2048
$image[$pvd] = 1
[Array]::Copy([Text.Encoding]::ASCII.GetBytes("CD001"), 0, $image, $pvd + 1, 5)
$image[$pvd + 6] = 1
Set-IsoBothEndianUInt32 -Buffer $image -Offset ($pvd + 80) -Value 32
Set-IsoBothEndianUInt16 -Buffer $image -Offset ($pvd + 128) -Value 2048
[Array]::Copy($rootRecord, 0, $image, $pvd + 156, $rootRecord.Length)

$terminator = 17 * 2048
$image[$terminator] = 255
[Array]::Copy([Text.Encoding]::ASCII.GetBytes("CD001"), 0, $image, $terminator + 1, 5)
$image[$terminator + 6] = 1

$cursor = 0
Add-IsoDirectoryRecord -Image $image -Sector 20 -Cursor ([ref]$cursor) -Record $rootRecord
Add-IsoDirectoryRecord -Image $image -Sector 20 -Cursor ([ref]$cursor) -Record (New-IsoRecord -ExtentSector 20 -DataLength 2048 -Flags 2 -Identifier $parent)
Add-IsoDirectoryRecord -Image $image -Sector 20 -Cursor ([ref]$cursor) -Record (New-IsoRecord -ExtentSector 21 -DataLength ([uint32]$payloadBytes.Length) -Flags 0 -Identifier ([Text.Encoding]::ASCII.GetBytes("ALPHA.TXT;1")))
[Array]::Copy($payloadBytes, 0, $image, 21 * 2048, $payloadBytes.Length)

$parentDirectory = Split-Path -Parent $OutputPath
if ($parentDirectory) {
    New-Item -ItemType Directory -Force -Path $parentDirectory | Out-Null
}
[IO.File]::WriteAllBytes($OutputPath, $image)
