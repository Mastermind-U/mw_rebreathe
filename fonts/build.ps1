param()
$ErrorActionPreference = 'Stop'
$validation = Join-Path $PSScriptRoot 'font-validation.json'
if (Test-Path -LiteralPath $validation -PathType Leaf) { Remove-Item -LiteralPath $validation }
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $csc)) {
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'
}
if (-not (Test-Path -LiteralPath $csc)) { throw '.NET Framework 4 compiler was not found.' }
$generator = Join-Path $PSScriptRoot 'BuildFonts.exe'
& $csc /nologo /optimize+ /r:System.Drawing.dll ("/out:" + $generator) (Join-Path $PSScriptRoot 'BuildFonts.cs')
if ($LASTEXITCODE -ne 0) { throw 'Font generator compilation failed.' }
& $generator $PSScriptRoot
if ($LASTEXITCODE -ne 0) { throw 'Font generation failed.' }
& (Join-Path $PSScriptRoot 'Validate-Fonts.ps1') -FontDirectory $PSScriptRoot
