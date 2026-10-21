param([Parameter(Mandatory = $true)] [string]$GameDirectory, [switch]$Rebuild)
$ErrorActionPreference = 'Stop'
if ($Rebuild) { & (Join-Path $PSScriptRoot 'Build.ps1') }
$gameRoot = (Resolve-Path -LiteralPath $GameDirectory).ProviderPath
$expected = 'EAC72E1B3524EE0012D080D3C2E2E00E401464E41EE54F9AFD6AAAADF2C221F5'
$before = (Get-FileHash -LiteralPath (Join-Path $gameRoot 'Morrowind.exe') -Algorithm SHA256).Hash
if ($before -ne $expected) { throw 'Unsupported executable SHA256.' }
& (Join-Path $PSScriptRoot 'bin\MorrowindModern.exe') --game-dir $gameRoot --probe
if ($LASTEXITCODE -ne 0) { throw "Native RAM-cache probe failed: $LASTEXITCODE" }
$result = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'logs\probe-results.json') -Raw | ConvertFrom-Json
if ($result.failures -ne 0 -or $result.checks -lt 400 -or !$result.legacy_crt_cache_verified -or !$result.mmio_wav_cache_verified) { throw 'Incomplete probe verification.' }
& (Join-Path $PSScriptRoot 'fonts\Test-Install.ps1')
$after = (Get-FileHash -LiteralPath (Join-Path $gameRoot 'Morrowind.exe') -Algorithm SHA256).Hash
if ($before -ne $after) { throw 'Executable changed during verification.' }
Write-Output ($result | ConvertTo-Json)
Write-Output 'Probe and reversible font installation passed; Morrowind.exe unchanged.'
Write-Output 'Interactive game/menu/font/cell-crossing verification is separate.'

$binaryHashes = [ordered]@{}
foreach ($name in @('MorrowindModern.exe', 'runner.exe', 'modernizer.dll', 'cache_probe.exe')) {
    $binaryHashes[$name] = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot ('bin/' + $name)) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$verification = [ordered]@{
    profile = 'ram-cache-standard-loading'
    verifiedUtc = [DateTime]::UtcNow.ToString('o')
    scope = 'Injected x86 RAM-cache probe and reversible font installation fixtures; not interactive gameplay validation.'
    originalExecutableUnchanged = ($before -eq $after)
    checks = $result.checks
    failures = $result.failures
    legacyCrtCacheVerified = $result.legacy_crt_cache_verified
    mmioWavCacheVerified = $result.mmio_wav_cache_verified
    binarySha256 = $binaryHashes
    probeSha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'logs/probe-results.json') -Algorithm SHA256).Hash.ToLowerInvariant()
}
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'logs/verification.json'), ($verification | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding($false)))
