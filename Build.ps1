param(
    [string]$VisualStudio = 'C:\Program Files\Microsoft Visual Studio\2022\Community',
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$tools = Get-ChildItem -LiteralPath "$VisualStudio\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (!$tools) { throw 'MSVC C++ tools not found. Install Visual Studio 2022 Desktop development with C++.' }
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdk = Get-ChildItem -LiteralPath "$sdkRoot\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$bin = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $project 'bin' }
$buildPath = $env:PATH
New-Item -ItemType Directory -Path $bin -Force | Out-Null
$env:INCLUDE = "$($tools.FullName)\include;$sdkRoot\Include\$($sdk.Name)\ucrt;$sdkRoot\Include\$($sdk.Name)\shared;$sdkRoot\Include\$($sdk.Name)\um"
function Compile([string]$architecture, [string[]]$arguments) {
    $env:PATH = "$($tools.FullName)\bin\Hostx64\$architecture;$sdkRoot\bin\$($sdk.Name)\x64;$buildPath"
    $env:LIB = "$($tools.FullName)\lib\$architecture;$sdkRoot\Lib\$($sdk.Name)\ucrt\$architecture;$sdkRoot\Lib\$($sdk.Name)\um\$architecture"
    $compiler = "$($tools.FullName)\bin\Hostx64\$architecture\cl.exe"
    & $compiler /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS @arguments
    if ($LASTEXITCODE -ne 0) { throw "Native build failed ($LASTEXITCODE)." }
}
Push-Location $bin
try {
    Compile x64 @("$project\src\host.cpp", "$project\launcher\settings.cpp", "$project\audio\game_audio_target.cpp", "$project\audio\session_mute.cpp", '/Fe:MorrowindModern.exe', '/link', '/MANIFEST:EMBED', 'ole32.lib', 'uuid.lib')
    Compile x86 @("$project\src\runner.cpp", "$project\audio\game_audio_target.cpp", "$project\shutdown\exit_hang_watchdog.cpp", '/Fe:runner.exe')
    Compile x86 @("$project\src\cache_probe.cpp", '/Fe:cache_probe.exe')
    Compile x86 @(
        '/LD', "$project\src\cache_hook.cpp", "$project\resolution\resolution.cpp",
        "$project\audio\audio_trace.cpp",
        "$project\audio\music_volume.cpp", "$project\display\display.cpp",
        "$project\ui\ui.cpp", "$project\ui\menu_style.cpp", "$project\ui\options_layout.cpp", "$project\ui\video_modes.cpp",
        "$project\ui\fillbar_fonts.cpp", "$project\ui\screenshot.cpp",
        '/Fe:modernizer.dll', '/link', "/DEF:$project\src\modernizer.def", 'user32.lib', 'advapi32.lib'
    )
} finally { Pop-Location }
Write-Output "Built $bin"
