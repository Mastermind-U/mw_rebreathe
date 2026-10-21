# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $GameDirectory,
    [ValidateSet('ClassicReadable', 'ModernSans')] [string] $FontStyle = 'ClassicReadable',
    [ValidateSet('cp437', 'cp1251')] [string] $FontEncoding = 'cp437',
    [ValidateSet(16, 20)] [int] $FontSize = 16
)
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

function Set-IniFont([string] $Text, [string] $Key, [string] $Value) {
    $newline = [Environment]::NewLine
    $foundNewline = [regex]::Match($Text, '\r\n|\n|\r')
    if ($foundNewline.Success) { $newline = $foundNewline.Value }
    $section = Get-FontsSection $Text
    if ($null -eq $section) {
        if ($Text.Length -gt 0 -and $Text[$Text.Length - 1] -ne [char]10 -and $Text[$Text.Length - 1] -ne [char]13) { $Text += $newline }
        $Text += '[Fonts]' + $newline
        $section = Get-FontsSection $Text
    }
    $entry = Get-FontEntry $Text $Key
    if ($null -ne $entry) {
        return $Text.Remove($entry.Start, $entry.Length).Insert($entry.Start, $entry.Prefix + $Value + $entry.Newline)
    }
    $prefix = ''
    if ($section.End -gt 0 -and $Text[$section.End - 1] -ne [char]10 -and $Text[$section.End - 1] -ne [char]13) { $prefix = $newline }
    return $Text.Insert($section.End, $prefix + $Key + '=' + $Value + $newline)
}

$gameRoot = (Resolve-Path -LiteralPath $GameDirectory).ProviderPath
if (-not (Test-Path -LiteralPath (Join-Path $gameRoot 'Morrowind.exe') -PathType Leaf)) { throw 'Morrowind.exe was not found in GameDirectory.' }
$iniPath = Join-Path $gameRoot 'Morrowind.ini'
if (-not (Test-Path -LiteralPath $iniPath -PathType Leaf)) { throw 'Morrowind.ini was not found.' }
$targetExe = Join-Path $gameRoot 'Morrowind.exe'
foreach ($running in @(Get-Process -Name Morrowind -ErrorAction SilentlyContinue)) {
    try { $runningPath = $running.Path } catch { throw 'Cannot inspect a running Morrowind process. Close it before installing fonts.' }
    if (!$runningPath -or [string]::Equals($runningPath, $targetExe, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'This Morrowind installation is running. Close the game before installing fonts.'
    }
}
$backupDirectory = Join-Path $gameRoot '.modernization-backup'
$manifestPath = Join-Path $backupDirectory 'install.json'
if (Test-Path -LiteralPath $manifestPath) { throw 'A font profile is already installed. Run Uninstall.ps1 before selecting another profile.' }
$fontDirectory = Join-Path $gameRoot 'Data Files\Fonts'
$sourceDirectory = Join-Path $PSScriptRoot ('fonts\' + $FontEncoding)
$files = @()
if ($FontStyle -eq 'ClassicReadable') {
    $fontName = 'century_gothic_font_regular'
    if ($FontSize -eq 20) { $fontName = 'century_gothic_big' }
    $captionFontName = 'century_gothic_font_regular'
    foreach ($name in @((($fontName + '.fnt'), ($fontName + '_0_Lod_A.tex'), ($captionFontName + '.fnt'), ($captionFontName + '_0_Lod_A.tex')) | Select-Object -Unique)) {
        $builtin = Join-Path $fontDirectory $name
        if (-not (Test-Path -LiteralPath $builtin -PathType Leaf)) { throw "Required built-in game font is missing: $builtin" }
    }
}
else {
    $fontName = 'modern_sans_' + $FontSize
    $captionFontName = 'modern_sans_16'
    $names = @('modern_sans_16.fnt', 'modern_sans_16_0_Lod_A.tex', 'modern_sans_20.fnt', 'modern_sans_20_0_Lod_A.tex')
    foreach ($name in $names) {
        $source = Join-Path $sourceDirectory $name
        $destination = Join-Path $fontDirectory $name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing font asset: $source" }
        if (Test-Path -LiteralPath $destination) { throw "A file with the mod's font name already exists: $destination. No existing font files were replaced." }
        $files += [pscustomobject]@{ Name = $name; Sha256 = Get-Sha256 ([IO.File]::ReadAllBytes($source)) }
    }
}
$originalIni = [IO.File]::ReadAllBytes($iniPath)
$originalHash = Get-Sha256 $originalIni
$iniText = Convert-IniBytes $originalIni
# Parse both entries before making any changes, so ambiguous input fails safely.
$null = Get-FontEntry $iniText 'Font 0'
$null = Get-FontEntry $iniText 'Font 1'
$installedText = Set-IniFont (Set-IniFont $iniText 'Font 0' $fontName) 'Font 1' $captionFontName
$installedIni = [Text.Encoding]::GetEncoding(28591).GetBytes($installedText)
$installedHash = Get-Sha256 $installedIni
$installationId = [Guid]::NewGuid().ToString('N')
$backupName = 'Morrowind.ini.' + $installationId + '.original'
$backupPath = Join-Path $backupDirectory $backupName
$copied = @()
$iniWritten = $false
try {
    $null = [IO.Directory]::CreateDirectory($backupDirectory)
    $null = [IO.Directory]::CreateDirectory($fontDirectory)
    [IO.File]::WriteAllBytes($backupPath, $originalIni)
    foreach ($file in $files) {
        [IO.File]::Copy((Join-Path $sourceDirectory $file.Name), (Join-Path $fontDirectory $file.Name), $false)
        $copied += $file
    }
    if ((Get-Sha256 ([IO.File]::ReadAllBytes($iniPath))) -ne $originalHash) { throw 'Morrowind.ini changed during installation; installation was cancelled.' }
    [IO.File]::WriteAllBytes($iniPath, $installedIni)
    $iniWritten = $true
    $manifest = [ordered]@{
        SchemaVersion = 1
        Feature = 'fonts'
        InstallationId = $installationId
        InstalledUtc = [DateTime]::UtcNow.ToString('o')
        FontStyle = $FontStyle
        FontEncoding = $FontEncoding
        FontSize = $FontSize
        FontName = $fontName
        InstalledFonts = [ordered]@{ 'Font 0' = $fontName; 'Font 1' = $captionFontName }
        OriginalIniSha256 = $originalHash
        InstalledIniSha256 = $installedHash
        Files = $files
    }
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
}
catch {
    $failure = $_
    if ($iniWritten -and (Test-Path -LiteralPath $iniPath) -and (Get-Sha256 ([IO.File]::ReadAllBytes($iniPath))) -eq $installedHash) {
        [IO.File]::WriteAllBytes($iniPath, $originalIni)
    }
    foreach ($file in $copied) {
        $destination = Join-Path $fontDirectory $file.Name
        if ((Test-Path -LiteralPath $destination -PathType Leaf) -and (Get-Sha256 ([IO.File]::ReadAllBytes($destination))) -eq $file.Sha256) {
            Remove-Item -LiteralPath $destination
        }
    }
    throw $failure
}
Write-Host "Font profile installed: $FontStyle; body Font0=$fontName; compact captions/console Font1=$captionFontName. Daedric font retained."
Write-Host "Original Morrowind.ini backup: $backupPath"
Write-Host 'Start the game through the modernization launcher to enable engine changes.'
