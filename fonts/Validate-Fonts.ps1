# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([string] $FontDirectory = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = (Resolve-Path -LiteralPath $FontDirectory).ProviderPath
$reportPath = Join-Path $root 'font-validation.json'
# A failed standalone validation must not leave an earlier successful report.
if (Test-Path -LiteralPath $reportPath -PathType Leaf) { Remove-Item -LiteralPath $reportPath }

function Assert-Font([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw ('Font structural validation failed: ' + $Message) }
}
function Get-Digest([string] $Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Test-GlyphInk([byte[]] $Texture, [int] $AtlasWidth, [int] $Left, [int] $Top, [int] $Right, [int] $Bottom) {
    for ($y = $Top; $y -lt $Bottom; ++$y) {
        for ($x = $Left; $x -lt $Right; ++$x) {
            if ($Texture[8 + 4 * ($y * $AtlasWidth + $x) + 3] -ne 0) { return $true }
        }
    }
    return $false
}

$fonts = @()
foreach ($codePage in @(437, 1251)) {
    $directory = 'cp' + $codePage
    foreach ($size in @(16, 20)) {
        $name = 'modern_sans_' + $size
        $textureName = $name + '_0_Lod_A'
        $fntPath = Join-Path $root ($directory + '/' + $name + '.fnt')
        $texPath = Join-Path $root ($directory + '/' + $textureName + '.tex')
        $fnt = [IO.File]::ReadAllBytes($fntPath)
        $texture = [IO.File]::ReadAllBytes($texPath)
        $label = $directory + '/' + $name
        Assert-Font ($fnt.Length -eq 14632) "$label FNT must contain a 296-byte header and 256 56-byte glyphs."
        Assert-Font ([BitConverter]::ToSingle($fnt, 0) -eq $size) "$label line height differs from its profile."
        Assert-Font (([BitConverter]::ToInt32($fnt, 4) -eq 1) -and ([BitConverter]::ToInt32($fnt, 8) -eq 1)) "$label unsupported FNT header."
        $nameEnd = 12
        while ($nameEnd -lt 296 -and $fnt[$nameEnd] -ne 0) { ++$nameEnd }
        Assert-Font ($nameEnd -lt 296) "$label texture name is not terminated."
        Assert-Font ([Text.Encoding]::ASCII.GetString($fnt, 12, $nameEnd - 12) -ceq $textureName) "$label texture filename differs from FNT."
        for ($padding = $nameEnd; $padding -lt 296; ++$padding) { Assert-Font ($fnt[$padding] -eq 0) "$label nonzero header name padding." }
        Assert-Font ($texture.Length -ge 8) "$label missing texture header."
        $width = [BitConverter]::ToInt32($texture, 0)
        $height = [BitConverter]::ToInt32($texture, 4)
        Assert-Font (($width -eq 512) -and ($height -gt 0) -and ($height -le 2048) -and (($height -band ($height - 1)) -eq 0)) "$label invalid atlas dimensions."
        Assert-Font ($texture.LongLength -eq (8L + 4L * $width * $height)) "$label texture is not exact tightly packed RGBA."
        $glyphs = @()
        $drawable = 0
        for ($code = 0; $code -lt 256; ++$code) {
            $values = [float[]]::new(14)
            for ($field = 0; $field -lt 14; ++$field) {
                $value = [BitConverter]::ToSingle($fnt, 296 + 56 * $code + 4 * $field)
                Assert-Font (-not [Single]::IsNaN($value) -and -not [Single]::IsInfinity($value)) "$label byte $code contains a nonfinite metric."
                $values[$field] = $value
            }
            $left = [double]$values[1]; $top = [double]$values[2]
            $right = [double]$values[3]; $bottom = [double]$values[6]
            Assert-Font ($values[0] -eq 0) "$label byte $code has an unsupported leading field."
            Assert-Font ($left -ge 0 -and $top -ge 0 -and $right -le 1 -and $bottom -le 1 -and $right -ge $left -and $bottom -ge $top) "$label byte $code UV outside atlas."
            Assert-Font ($values[4] -eq $values[2] -and $values[5] -eq $values[1] -and $values[7] -eq $values[3] -and $values[8] -eq $values[6]) "$label byte $code nonrectangular UV quad."
            Assert-Font ($values[9] -ge 0 -and $values[10] -ge 0) "$label byte $code negative glyph dimensions."
            Assert-Font ([Math]::Abs(($right - $left) * $width - 2 * $values[9]) -le 0.0002 -and [Math]::Abs(($bottom - $top) * $height - 2 * $values[10]) -le 0.0002) "$label byte $code violates atlas scale 2."
            $advance = [double]$values[9] + [double]$values[12]
            Assert-Font ($advance -ge 0) "$label byte $code negative advance."
            if ($values[9] -gt 0) {
                Assert-Font ($values[10] -gt 0 -and $advance -gt 0) "$label byte $code drawable glyph has no height/advance."
                ++$drawable
            } else {
                Assert-Font ($values[10] -eq 0 -and $left -eq 0 -and $top -eq 0 -and $right -eq 0 -and $bottom -eq 0) "$label byte $code empty glyph has atlas coordinates."
            }
            if ($code -lt 32 -or $code -eq 127) { Assert-Font ($values[9] -eq 0 -and $advance -eq 0) "$label control byte $code has printable metrics." }
            $glyphs += [pscustomobject]@{ byte = $code; width = [double]$values[9]; height = [double]$values[10]; advance = $advance; left = $left; top = $top; right = $right; bottom = $bottom }
        }
        Assert-Font ($drawable -ge 200) "$label unexpectedly few drawable glyphs."
        # All ASCII letters/digits, extended CP437 samples and all Russian letters
        # must have nonempty metrics. Ink checks use representative glyphs only.
        $required = @(48..57) + @(65..90) + @(97..122)
        $sampleCodes = if ($codePage -eq 1251) { @(65, 192, 224, 168, 184) } else { @(65, 130, 137, 154) }
        if ($codePage -eq 1251) { $required += @(192..255) + @(168, 184) } else { $required += @(130, 137, 154) }
        foreach ($code in $required) { Assert-Font ($glyphs[$code].width -gt 0 -and $glyphs[$code].height -gt 0 -and $glyphs[$code].advance -gt 0) "$label required byte $code is empty." }
        $samples = @()
        foreach ($code in $sampleCodes) {
            $glyph = $glyphs[$code]
            $ink = Test-GlyphInk $texture $width ([int][Math]::Round($glyph.left * $width)) ([int][Math]::Round($glyph.top * $height)) ([int][Math]::Round($glyph.right * $width)) ([int][Math]::Round($glyph.bottom * $height))
            Assert-Font $ink "$label representative byte $code has no alpha ink."
            $samples += [ordered]@{ byte = $code; width = $glyph.width; height = $glyph.height; advance = $glyph.advance; alphaInkPresent = $true }
        }
        $fntHash = Get-Digest $fntPath
        $fonts += [ordered]@{
            directory = $directory; font = $name; fntBytes = $fnt.Length
            textureWidth = $width; textureHeight = $height; textureBytes = $texture.Length
            drawableGlyphCount = $drawable; sampleGlyphs = $samples
            sha256 = $fntHash; fntSha256 = $fntHash; textureSha256 = Get-Digest $texPath
        }
    }
}
$result = [ordered]@{
    schemaVersion = 1; format = 'Morrowind bitmap fonts'
    structuralValidationOnly = $true; runtimeEngineTested = $false
    validatedUtc = [DateTime]::UtcNow.ToString('o')
    sourceFontSha256 = Get-Digest (Join-Path $root 'source/NotoSans-Regular.ttf')
    fonts = $fonts
}
$temporary = $reportPath + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
try {
    [IO.File]::WriteAllText($temporary, ($result | ConvertTo-Json -Depth 7) + [Environment]::NewLine, (New-Object Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $temporary -Destination $reportPath -Force
} finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
Write-Output ('Font structural validation passed: ' + $fonts.Count + ' FNT/TEX pairs; report ' + $reportPath)
