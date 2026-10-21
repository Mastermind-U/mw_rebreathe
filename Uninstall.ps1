# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([Parameter(Mandatory = $true)] [string] $GameDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-Sha256([byte[]] $Bytes) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}

function Convert-IniBytes([byte[]] $Bytes) {
    if ([Array]::IndexOf($Bytes, [byte]0) -ge 0) { throw 'Morrowind.ini must use an 8-bit encoding; UTF-16 is unsupported.' }
    return [Text.Encoding]::GetEncoding(28591).GetString($Bytes)
}

function Get-FontsSection([string] $Text) {
    $headers = [regex]::Matches($Text, '(?im)^\[Fonts\][^\r\n]*(?:\r\n|\n|\r|$)')
    if ($headers.Count -gt 1) { throw 'Multiple [Fonts] sections are ambiguous; keep one section before installing or uninstalling.' }
    if ($headers.Count -eq 0) { return $null }
    $header = $headers[0]
    $start = $header.Index + $header.Length
    $next = [regex]::Match($Text.Substring($start), '(?m)^\[[^\]\r\n]+\][^\r\n]*(?:\r\n|\n|\r|$)')
    $end = $Text.Length
    if ($next.Success) { $end = $start + $next.Index }
    return [pscustomobject]@{ HeaderStart = $header.Index; Start = $start; End = $end }
}

function Get-FontEntry([string] $Text, [string] $Key) {
    $section = Get-FontsSection $Text
    if ($null -eq $section) { return $null }
    $pattern = '(?im)^([ \t]*' + [regex]::Escape($Key) + '[ \t]*=)([^\r\n]*)(\r\n|\n|\r|$)'
    $entries = [regex]::Matches($Text.Substring($section.Start, $section.End - $section.Start), $pattern)
    if ($entries.Count -gt 1) { throw "Duplicate $Key entries are ambiguous." }
    if ($entries.Count -eq 0) { return $null }
    $entry = $entries[0]
    return [pscustomobject]@{
        Start = $section.Start + $entry.Index
        Length = $entry.Length
        Prefix = $entry.Groups[1].Value
        Value = $entry.Groups[2].Value
        Newline = $entry.Groups[3].Value
        Raw = $entry.Value
    }
}

$gameRoot = (Resolve-Path -LiteralPath $GameDirectory).ProviderPath
$targetExe = Join-Path $gameRoot 'Morrowind.exe'
foreach ($running in @(Get-Process -Name Morrowind -ErrorAction SilentlyContinue)) {
    try { $runningPath = $running.Path } catch { throw 'Cannot inspect a running Morrowind process. Close it before removing fonts.' }
    if (!$runningPath -or [string]::Equals($runningPath, $targetExe, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'This Morrowind installation is running. Close the game before removing fonts.'
    }
}
$iniPath = Join-Path $gameRoot 'Morrowind.ini'
$backupDirectory = Join-Path $gameRoot '.modernization-backup'
$manifestPath = Join-Path $backupDirectory 'install.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'No active modernization font installation was found.' }
$manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
if ($manifest.SchemaVersion -ne 1 -or $manifest.Feature -ne 'fonts' -or $manifest.InstallationId -notmatch '^[0-9a-f]{32}$') { throw 'Invalid installation manifest.' }
if ($manifest.FontName -notmatch '^(modern_sans_(16|20)|century_gothic_font_regular|century_gothic_big)$' -or $manifest.OriginalIniSha256 -notmatch '^[0-9a-f]{64}$' -or $manifest.InstalledIniSha256 -notmatch '^[0-9a-f]{64}$') { throw 'Invalid installation manifest values.' }
$isClassic = $manifest.FontName -in @('century_gothic_font_regular', 'century_gothic_big')
$installedFonts = @{ 'Font 0' = $manifest.FontName; 'Font 1' = $manifest.FontName }
# Older manifests used one name for both roles; keep those installations reversible.
if ($manifest.PSObject.Properties.Name -contains 'InstalledFonts') {
    $roleNames = @($manifest.InstalledFonts.PSObject.Properties.Name)
    if ($roleNames.Count -ne 2 -or $roleNames -notcontains 'Font 0' -or $roleNames -notcontains 'Font 1') { throw 'Invalid installed font roles.' }
    $bodyFont = [string]$manifest.InstalledFonts.'Font 0'
    $captionFont = [string]$manifest.InstalledFonts.'Font 1'
    $expectedCaption = 'modern_sans_16'
    if ($isClassic) { $expectedCaption = 'century_gothic_font_regular' }
    if ($bodyFont -ne $manifest.FontName -or $captionFont -ne $expectedCaption) { throw 'Invalid installed font role values.' }
    $installedFonts = @{ 'Font 0' = $bodyFont; 'Font 1' = $captionFont }
}
if ($isClassic) {
    if (@($manifest.Files).Count -ne 0) { throw 'A built-in font profile must not own any game font files.' }
}
else {
    $names = @('modern_sans_16.fnt', 'modern_sans_16_0_Lod_A.tex', 'modern_sans_20.fnt', 'modern_sans_20_0_Lod_A.tex')
    if (@($manifest.Files).Count -ne $names.Count) { throw 'Invalid font file list in manifest.' }
    foreach ($name in $names) {
        $matching = @($manifest.Files | Where-Object { $_.Name -eq $name })
        if ($matching.Count -ne 1 -or $matching[0].Sha256 -notmatch '^[0-9a-f]{64}$') { throw 'Invalid font file entry in manifest.' }
    }
}
$backupName = 'Morrowind.ini.' + $manifest.InstallationId + '.original'
$backupPath = Join-Path $backupDirectory $backupName
$originalIni = [IO.File]::ReadAllBytes($backupPath)
if ((Get-Sha256 $originalIni) -ne $manifest.OriginalIniSha256) { throw 'The original Morrowind.ini backup checksum does not match the manifest.' }
$currentIni = [IO.File]::ReadAllBytes($iniPath)
$currentText = Convert-IniBytes $currentIni
if ((Get-Sha256 $currentIni) -eq $manifest.InstalledIniSha256) {
    $restoredIni = $originalIni
    $restoredText = Convert-IniBytes $originalIni
}
else {
    $originalText = Convert-IniBytes $originalIni
    $restoredText = $currentText
    foreach ($key in @('Font 0', 'Font 1')) {
        $current = Get-FontEntry $restoredText $key
        $original = Get-FontEntry $originalText $key
        if ($null -eq $current) { continue }
        if ($current.Value.Trim() -ne $installedFonts[$key]) {
            Write-Host "$key was changed after installation; that setting was retained."
            continue
        }
        $replacement = ''
        if ($null -ne $original) { $replacement = $original.Raw }
        $restoredText = $restoredText.Remove($current.Start, $current.Length).Insert($current.Start, $replacement)
    }
    if ($null -eq (Get-FontsSection $originalText)) {
        $section = Get-FontsSection $restoredText
        if ($null -ne $section -and $restoredText.Substring($section.Start, $section.End - $section.Start).Trim().Length -eq 0) {
            $restoredText = $restoredText.Remove($section.HeaderStart, $section.End - $section.HeaderStart)
        }
    }
    $restoredIni = [Text.Encoding]::GetEncoding(28591).GetBytes($restoredText)
    Write-Host 'Other edits to Morrowind.ini were retained; only unchanged mod font settings were restored.'
}
if ((Get-Sha256 ([IO.File]::ReadAllBytes($iniPath))) -ne (Get-Sha256 $currentIni)) { throw 'Morrowind.ini changed during uninstall; no files were removed.' }
[IO.File]::WriteAllBytes($iniPath, $restoredIni)
$fontDirectory = Join-Path $gameRoot 'Data Files\Fonts'
$retainFonts = $restoredText -match '(?im)^[ \t]*Font[ \t]+\d+[ \t]*=[ \t]*modern_sans_(16|20)[ \t]*(?:\r?$)'
$partial = $false
foreach ($file in $manifest.Files) {
    $destination = Join-Path $fontDirectory $file.Name
    if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) { continue }
    if ($retainFonts) {
        $partial = $true
        continue
    }
    if ((Get-Sha256 ([IO.File]::ReadAllBytes($destination))) -ne $file.Sha256) {
        Write-Host "Changed font asset retained: $destination"
        $partial = $true
        continue
    }
    Remove-Item -LiteralPath $destination
}
if ($partial) {
    Write-Host 'Some mod font assets were retained because they were edited or are still referenced in [Fonts].'
    Write-Host 'The installation manifest and original ini backup were retained. Run uninstall again after resolving those files or settings.'
}
else {
    $archivePath = Join-Path $backupDirectory ('uninstalled.' + $manifest.InstallationId + '.json')
    Move-Item -LiteralPath $manifestPath -Destination $archivePath
    Write-Host 'Font profile uninstalled. Original ini backup and archived manifest retained for reference.'
}
