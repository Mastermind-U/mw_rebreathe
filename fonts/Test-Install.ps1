# SPDX-License-Identifier: GPL-3.0-only
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$package = Split-Path -Parent $PSScriptRoot
$testRoot = Join-Path $PSScriptRoot ('_install-test-' + [Guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($testRoot)
function Assert([bool] $Condition, [string] $Message) { if (-not $Condition) { throw $Message } }
function EqualBytes([byte[]] $First, [byte[]] $Second) {
    return [Convert]::ToBase64String($First) -ceq [Convert]::ToBase64String($Second)
}
function New-TestGame([string] $Name) {
    $folder = Join-Path $testRoot $Name
    $null = [IO.Directory]::CreateDirectory((Join-Path $folder 'Data Files\Fonts'))
    [IO.File]::WriteAllBytes((Join-Path $folder 'Morrowind.exe'), [byte[]]@(77,90))
    return $folder
}
$encoding = [Text.Encoding]::GetEncoding(1251)
$originalText = "[General]" + [Environment]::NewLine + "Max FPS=240" + [Environment]::NewLine +
    'Localized=' + [char]0x042F + [Environment]::NewLine +
    "[Fonts]" + [Environment]::NewLine + "Font 0=magic_cards_regular" + [Environment]::NewLine +
    "Font 1=century_gothic_font_regular" + [Environment]::NewLine + "Font 2=daedric_font" + [Environment]::NewLine +
    "[Archives]" + [Environment]::NewLine + "Archive 0=Tribunal.bsa" + [Environment]::NewLine
$original = $encoding.GetBytes($originalText)
$checks = @()
try {
    $game = New-TestGame 'roundtrip'
    $ini = Join-Path $game 'Morrowind.ini'
    [IO.File]::WriteAllBytes($ini, $original)
    & (Join-Path $package 'Install.ps1') -FontStyle ModernSans -GameDirectory $game
    Assert (([IO.File]::ReadAllText($ini)).Contains('Font 0=modern_sans_16')) 'Font 0 was not installed.'
    & (Join-Path $package 'Uninstall.ps1') -GameDirectory $game
    Assert (EqualBytes $original ([IO.File]::ReadAllBytes($ini))) 'Unchanged ini was not restored byte for byte.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $game 'Data Files\Fonts\modern_sans_16.fnt'))) 'Installed font was not removed.'
    $checks += 'Byte-exact roundtrip, including original CP1251 non-ASCII data.'

    & (Join-Path $package 'Install.ps1') -FontStyle ModernSans -GameDirectory $game -FontEncoding cp1251 -FontSize 20
    $changed = $encoding.GetString([IO.File]::ReadAllBytes($ini)).Replace('Max FPS=240','Max FPS=144')
    [IO.File]::WriteAllBytes($ini, $encoding.GetBytes($changed))
    & (Join-Path $package 'Uninstall.ps1') -GameDirectory $game
    Assert (EqualBytes ($encoding.GetBytes($originalText.Replace('Max FPS=240','Max FPS=144'))) ([IO.File]::ReadAllBytes($ini))) 'Uninstall did not preserve unrelated ini edits.'
    $checks += 'Reinstall succeeds; larger Cyrillic selection and unrelated edits preserved.'

    $collisionGame = New-TestGame 'collision'
    $collisionIni = Join-Path $collisionGame 'Morrowind.ini'
    [IO.File]::WriteAllBytes($collisionIni, $original)
    $collisionFile = Join-Path $collisionGame 'Data Files\Fonts\modern_sans_16.fnt'
    [IO.File]::WriteAllBytes($collisionFile, [byte[]]@(1,2,3))
    $rejected = $false
    try { & (Join-Path $package 'Install.ps1') -FontStyle ModernSans -GameDirectory $collisionGame }
    catch { $rejected = $true }
    Assert $rejected 'A pre-existing font collision was not rejected.'
    Assert (EqualBytes $original ([IO.File]::ReadAllBytes($collisionIni))) 'Collision modified ini.'
    Assert (EqualBytes ([byte[]]@(1,2,3)) ([IO.File]::ReadAllBytes($collisionFile))) 'Collision modified the existing font.'
    $checks += 'Existing font collisions rejected before mutation.'

    $changedGame = New-TestGame 'changed-font'
    $changedIni = Join-Path $changedGame 'Morrowind.ini'
    [IO.File]::WriteAllBytes($changedIni, $original)
    & (Join-Path $package 'Install.ps1') -FontStyle ModernSans -GameDirectory $changedGame
    $changedFont = Join-Path $changedGame 'Data Files\Fonts\modern_sans_16.fnt'
    [IO.File]::WriteAllBytes($changedFont, [byte[]]@(7,8,9))
    & (Join-Path $package 'Uninstall.ps1') -GameDirectory $changedGame
    Assert (EqualBytes ([byte[]]@(7,8,9)) ([IO.File]::ReadAllBytes($changedFont))) 'Modified font was deleted.'
    Assert (Test-Path -LiteralPath (Join-Path $changedGame '.modernization-backup\install.json')) 'Partial uninstall did not retain its manifest.'
    Assert (EqualBytes $original ([IO.File]::ReadAllBytes($changedIni))) 'Original ini was not restored for a modified-font uninstall.'
    $checks += 'User-modified font retained with recovery manifest.'

    $classicGame = New-TestGame 'classic-readable'
    $classicIni = Join-Path $classicGame 'Morrowind.ini'
    $classicDirectory = Join-Path $classicGame 'Data Files\Fonts'
    [IO.File]::WriteAllBytes($classicIni, $original)
    $builtinNames = @('century_gothic_font_regular.fnt', 'century_gothic_font_regular_0_Lod_A.tex',
        'century_gothic_big.fnt', 'century_gothic_big_0_Lod_A.tex')
    foreach ($name in $builtinNames) { [IO.File]::WriteAllBytes((Join-Path $classicDirectory $name), [byte[]]@(11,22,33)) }
    # Default profile must use built-in assets and never copy or own them.
    & (Join-Path $package 'Install.ps1') -GameDirectory $classicGame
    Assert (([IO.File]::ReadAllText($classicIni)).Contains('Font 0=century_gothic_font_regular')) 'Default classic Font 0 was not selected.'
    $classicManifest = [IO.File]::ReadAllText((Join-Path $classicGame '.modernization-backup\install.json')) | ConvertFrom-Json
    Assert ($classicManifest.FontStyle -eq 'ClassicReadable' -and @($classicManifest.Files).Count -eq 0) 'Classic profile owns game assets.'
    Assert (@(Get-ChildItem -LiteralPath $classicDirectory -File).Count -eq 4) 'Classic profile copied extra assets.'
    & (Join-Path $package 'Uninstall.ps1') -GameDirectory $classicGame
    Assert (EqualBytes $original ([IO.File]::ReadAllBytes($classicIni))) 'Classic profile did not restore original ini.'
    foreach ($name in $builtinNames) { Assert (EqualBytes ([byte[]]@(11,22,33)) ([IO.File]::ReadAllBytes((Join-Path $classicDirectory $name)))) 'A built-in asset was altered or removed.' }
    $checks += 'Default ClassicReadable changes only ini; no copied assets, empty ownership list, built-in assets preserved.'
    $classicLargeOriginalText = $originalText.Replace('Font 1=century_gothic_font_regular','Font 1=magic_cards_regular')
    [IO.File]::WriteAllBytes($classicIni, $encoding.GetBytes($classicLargeOriginalText))
    & (Join-Path $package 'Install.ps1') -GameDirectory $classicGame -FontSize 20
    Assert (([IO.File]::ReadAllText($classicIni)).Contains('Font 0=century_gothic_big')) 'Classic 20px font was not selected.'
    Assert (([IO.File]::ReadAllText($classicIni)).Contains('Font 1=century_gothic_font_regular')) 'Classic large body did not retain compact caption role.'
    $roleManifest = [IO.File]::ReadAllText((Join-Path $classicGame '.modernization-backup\install.json')) | ConvertFrom-Json
    Assert ($roleManifest.InstalledFonts.'Font 0' -eq 'century_gothic_big' -and $roleManifest.InstalledFonts.'Font 1' -eq 'century_gothic_font_regular') 'Mixed font roles were not recorded.'
    $classicChanged = $encoding.GetString([IO.File]::ReadAllBytes($classicIni)).Replace('Max FPS=240','Max FPS=165')
    [IO.File]::WriteAllBytes($classicIni, $encoding.GetBytes($classicChanged))
    & (Join-Path $package 'Uninstall.ps1') -GameDirectory $classicGame
    Assert (EqualBytes ($encoding.GetBytes($classicLargeOriginalText.Replace('Max FPS=240','Max FPS=165'))) ([IO.File]::ReadAllBytes($classicIni))) 'Classic uninstall did not preserve unrelated ini edits.'
    foreach ($name in $builtinNames) { Assert (EqualBytes ([byte[]]@(11,22,33)) ([IO.File]::ReadAllBytes((Join-Path $classicDirectory $name)))) 'A large built-in asset was altered or removed.' }
    $checks += 'ClassicReadable 20px assigns big body and regular compact captions, records separate roles and restores both around unrelated user edits.'

    $result = [ordered]@{ Passed = $true; RuntimeEngineTested = $false; Checks = $checks }
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'installer-validation.json'), ($result | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
    Write-Output ($result | ConvertTo-Json -Depth 4)
}
finally {
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $allowedPrefix = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd([char]92, [char]47) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($allowedPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Test cleanup target escaped the font package.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
