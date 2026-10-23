param([Parameter(Mandatory = $true)][string]$SourcePath)

# Real production observers run against independent RAM-created pixels; no product coverage is inferred.
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$source = (Resolve-Path -LiteralPath $SourcePath).Path
$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$null,[ref]$errors)
if($errors.Count){throw 'Hover source did not parse.'}
foreach($name in @('Get-GuiHistorySeparator','Get-GuiTooltipPaintCount')){
 $definition=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
 if(-not $definition){throw "Missing actual helper $name"}
 . ([ScriptBlock]::Create($definition.Extent.Text))
}
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$root = Join-Path $repo ('out/gui-hover-paint-contract-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force|Out-Null
$checks=0
foreach($dpi in @(96,144,192)){
 $scale=$dpi/96.0
 foreach($kind in @('Single','Flat','Ambiguous','Short')){
  $bitmap=[Drawing.Bitmap]::new(2000,1500)
  try{
   $xs=if($kind -eq 'Flat'){@()}elseif($kind -eq 'Ambiguous'){@(340,350)}else{@(340)}
   foreach($designX in $xs){
    $x=7+[int][Math]::Round($designX*$scale)
    $height=if($kind -eq 'Short'){8}else{20}
    for($y=31+[int][Math]::Round(194*$scale);$y -lt 31+[int][Math]::Round((194+$height)*$scale);++$y){$bitmap.SetPixel($x,$y,[Drawing.Color]::FromArgb(54,72,78))}
   }
   $path=Join-Path $root "$dpi-$kind.png"; $bitmap.Save($path,[Drawing.Imaging.ImageFormat]::Png)
  }finally{$bitmap.Dispose()}
  $observed=$null;$rejected=$false
  try{$observed=Get-GuiHistorySeparator -Path $path -Dpi $dpi -OffsetX 7 -OffsetY 31 -Left 320 -Right 370}
  catch{if($_.Exception.Message -ne 'A unique painted History separator was not observed.'){throw};$rejected=$true}
  if($kind -eq 'Single'){if($rejected -or [Math]::Abs($observed-340) -gt 0.5){throw "Wrong measured separator: $dpi"}}
  elseif(-not $rejected){throw "False separator admission: $dpi/$kind"}
  ++$checks
 }
 foreach($inside in @($false,$true)){
  $bitmap=[Drawing.Bitmap]::new(2600,1700)
  try{
   $top=if($inside){150}else{735}
   for($x=7+[int][Math]::Round(200*$scale);$x -lt 7+[int][Math]::Round(300*$scale);++$x){
    for($y=31+[int][Math]::Round($top*$scale);$y -lt 31+[int][Math]::Round(($top+40)*$scale);++$y){$bitmap.SetPixel($x,$y,[Drawing.Color]::FromArgb(33,45,50))}
   }
   $path=Join-Path $root "$dpi-Tooltip-$inside.png";$bitmap.Save($path,[Drawing.Imaging.ImageFormat]::Png)
  }finally{$bitmap.Dispose()}
  $count=Get-GuiTooltipPaintCount -Path $path -Dpi $dpi -OffsetX 7 -OffsetY 31
  if(($inside -and $count -lt 300) -or (-not $inside -and $count -ne 0)){throw 'Tooltip sampler included the status strip or missed owned content.'}
  ++$checks
 }
}
"Actual hover paint helpers passed $checks independent DPI, offset, flat, sparse, ambiguity and status-strip controls."
