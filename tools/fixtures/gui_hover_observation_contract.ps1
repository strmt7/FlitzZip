param([Parameter(Mandatory = $true)][string]$SourcePath,
    [string]$SmokeSourcePath = (Join-Path (Split-Path -Parent $SourcePath) 'gui_smoke.ps1'))

# Real production observers run against independent RAM-created pixels; no product coverage is inferred.
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$source = (Resolve-Path -LiteralPath $SourcePath).Path
$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$null,[ref]$errors)
if($errors.Count){throw 'Hover source did not parse.'}
foreach($name in @('Get-GuiColumnSeparator','Get-GuiTooltipPaintCount')){
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
 foreach($designTop in @(136,194)){
 foreach($kind in @('Single','Flat','Ambiguous','Short','Below')){
  $bitmap=[Drawing.Bitmap]::new(2000,1500)
  try{
   $xs=if($kind -eq 'Flat'){@()}elseif($kind -eq 'Ambiguous'){@(340,350)}else{@(340)}
   foreach($designX in $xs){
    $x=7+[int][Math]::Round($designX*$scale)
    $height=if($kind -eq 'Short'){8}else{20}
    $paintTop=if($kind -eq 'Below'){$designTop+40}else{$designTop}
    for($y=31+[int][Math]::Round($paintTop*$scale);$y -lt 31+[int][Math]::Round(($paintTop+$height)*$scale);++$y){$bitmap.SetPixel($x,$y,[Drawing.Color]::FromArgb(54,72,78))}
   }
   $path=Join-Path $root "$dpi-$designTop-$kind.png"; $bitmap.Save($path,[Drawing.Imaging.ImageFormat]::Png)
  }finally{$bitmap.Dispose()}
  $observed=$null;$rejected=$false
  try{$observed=Get-GuiColumnSeparator -Path $path -Dpi $dpi -OffsetX 7 -OffsetY 31 -Left 320 -Right 370 -Top $designTop}
  catch{if($_.Exception.Message -ne 'A unique painted table separator was not observed.'){throw};$rejected=$true}
  if($kind -eq 'Single'){if($rejected -or [Math]::Abs($observed-340) -gt 0.5){throw "Wrong measured separator: $dpi"}}
  elseif(-not $rejected){throw "False separator admission: $dpi/$kind"}
  ++$checks
 }
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

# Execute the actual focused branch with recorded scenario boundaries; no product path is replaced in a real run.
$errors=$null
$smokeAst=[Management.Automation.Language.Parser]::ParseFile((Resolve-Path -LiteralPath $SmokeSourcePath).Path,[ref]$null,[ref]$errors)
if($errors.Count){throw 'GUI smoke source did not parse.'}
$branches=@($smokeAst.FindAll({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Clauses[0].Item1.Extent.Text -eq '$HoverOnly'},$true))
if($branches.Count -ne 1){throw 'Focused hover routing is missing or ambiguous.'}
$focusedCalls=[Collections.Generic.List[string]]::new()
# Purpose: Record exact focused-controller arguments. Inputs: Scalar smoke fixture values and SkipHistory.
# Outputs: Rejects accidental History work and records one returned capture without accessing a GUI.
function Assert-GuiTextTooltip {
 param($Handle,$Dpi,$FixtureArchive,$FixtureRoot,$BasePath,$Extension,[switch]$SkipHistory)
 if($Handle -ne [IntPtr]42 -or $Dpi -ne 144 -or $FixtureArchive -cne 'fixture.flzip' -or $FixtureRoot -cne 'owned-root' -or
    $BasePath -cne 'owned-capture' -or $Extension -cne '.png' -or -not $SkipHistory){throw 'Focused hover lost its exact fixture arguments or requested History work.'}
 $focusedCalls.Add('Queue-Extract-only')
 [pscustomobject]@{Path='owned-capture.png'}
}
# Purpose: Reject a broad scenario from a focused controller. Inputs: Any bound/unbound arguments.
# Outputs: Throws before external work can start.
function Assert-GuiQueueColumnResize { throw 'Focused hover unexpectedly invoked the full Queue resize scenario.' }
$focusedInputs=@{
 HoverOnly=$true;windowHandle=[IntPtr]42;windowDpi=144;smokeArchive='fixture.flzip';smokeRoot='owned-root'
 basePath='owned-capture';extension='.png';captures=@()
}
# The parsed production branch resolves these exact names dynamically in its parent scope.
foreach($entry in $focusedInputs.GetEnumerator()){Set-Variable -Name $entry.Key -Value $entry.Value}
[void](& ([scriptblock]::Create($branches[0].Extent.Text)))
if($focusedCalls.Count -ne 1 -or $focusedCalls[0] -cne 'Queue-Extract-only'){throw 'Focused hover did not execute exactly its admitted scenario.'}
'Actual focused HoverOnly branch preserves the Queue/Extract scope and exact arguments.'
