# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string] $Version = '0.2.0',
    [switch] $WithoutRuntimeEvidence
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = (Resolve-Path -LiteralPath $PSScriptRoot).ProviderPath
$utf8 = New-Object Text.UTF8Encoding($false)
function Get-Sha256([byte[]] $Bytes) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}
$entries = [Collections.Generic.SortedDictionary[string,byte[]]]::new([StringComparer]::Ordinal)
$manifestFiles = @()
foreach ($name in Get-Content -LiteralPath (Join-Path $root 'release-files.txt')) {
    if ([string]::IsNullOrWhiteSpace($name)) { continue }
    if ($name -match '(^/|\\|(^|/)\.\.?(/|$)|^(backup|\.misc|logs|dist|evidence|\.git)/)' -or
        [IO.Path]::IsPathRooted($name) -or $entries.ContainsKey($name)) { throw "Unsafe/duplicate release path: $name" }
    $path = [IO.Path]::GetFullPath((Join-Path $root $name))
    if (!$path.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "Release path escaped project: $name" }
    $item = Get-Item -LiteralPath $path
    if ($item -is [IO.DirectoryInfo]) { throw "Release entry is not a file: $name" }
    $current = $item
    while ($null -ne $current -and $current.FullName -ne $root) {
        if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Reparse point in release path: $name" }
        $current = if ($current -is [IO.DirectoryInfo]) { $current.Parent } else { $current.Directory }
    }
    $bytes = [IO.File]::ReadAllBytes($path)
    $entries.Add($name, $bytes)
    $manifestFiles += [ordered]@{ path = $name; bytes = $bytes.Length; sha256 = Get-Sha256 $bytes }
}
if (!$WithoutRuntimeEvidence) {
    $verifiedPath = Join-Path $root 'logs/verification.json'
    $verified = [IO.File]::ReadAllText($verifiedPath) | ConvertFrom-Json
    if ($verified.profile -ne 'ram-cache-standard-loading' -or !$verified.originalExecutableUnchanged -or
        $verified.failures -ne 0 -or $verified.checks -lt 400 -or !$verified.legacyCrtCacheVerified -or !$verified.mmioWavCacheVerified) {
        throw 'Run Verify.ps1 successfully after the final stable build.'
    }
    foreach ($name in @('MorrowindModern.exe', 'runner.exe', 'modernizer.dll', 'cache_probe.exe')) {
        $hash = Get-Sha256 $entries['bin/' + $name]
        if ($hash -ne $verified.binarySha256.$name) { throw "Binary changed since verification: $name" }
    }
    $probeHash = (Get-FileHash -LiteralPath (Join-Path $root 'logs/probe-results.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($probeHash -ne $verified.probeSha256) { throw 'Probe results changed since verification.' }
    $entries.Add('evidence/verification.json', $utf8.GetBytes(($verified | ConvertTo-Json -Depth 6)))
}
$manifest = [ordered]@{
    version = $Version
    profile = 'ram-cache-standard-loading'
    loadingBehavior = 'Original cell loading, map generation and worker waits; predictive streaming is not linked.'
    runtimeEvidenceIncluded = !$WithoutRuntimeEvidence
    files = $manifestFiles
}
$entries.Add('release-manifest.json', $utf8.GetBytes(($manifest | ConvertTo-Json -Depth 6)))
$dist = Join-Path $root 'dist'
$null = [IO.Directory]::CreateDirectory($dist)
$archiveName = 'mw_rebreathe-' + $Version + '.zip'
$archivePath = Join-Path $dist $archiveName
$temporaryPath = Join-Path $dist ('.package-' + [Guid]::NewGuid().ToString('N') + '.tmp')
$stream = $null
$archive = $null
try {
    $stream = [IO.File]::Open($temporaryPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $archive = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
    $fixedTimestamp = New-Object DateTimeOffset(2026, 1, 1, 0, 0, 0, ([TimeSpan]::Zero))
    foreach ($pair in $entries.GetEnumerator()) {
        $entry = $archive.CreateEntry($pair.Key, [IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = $fixedTimestamp
        $entryStream = $entry.Open()
        try { $entryStream.Write($pair.Value, 0, $pair.Value.Length) }
        finally { $entryStream.Dispose() }
    }
    $archive.Dispose(); $archive = $null
    $stream.Dispose(); $stream = $null
    [IO.File]::Copy($temporaryPath, $archivePath, $true)
}
finally {
    if ($null -ne $archive) { $archive.Dispose() }
    if ($null -ne $stream) { $stream.Dispose() }
    if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) { Remove-Item -LiteralPath $temporaryPath }
}
$checksum = Get-Sha256 ([IO.File]::ReadAllBytes($archivePath))
[IO.File]::WriteAllText(($archivePath + '.sha256'), ($checksum + '  ' + $archiveName + [Environment]::NewLine), [Text.Encoding]::ASCII)
Write-Output ('Archive: ' + $archivePath)
Write-Output ('Entries: ' + $entries.Count)
Write-Output ('SHA256: ' + $checksum)
