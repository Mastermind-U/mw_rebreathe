# mw_rebreathe

A native modernization mod for **Morrowind**, with a RAM resource cache and the original cell-loading system.

The stable profile keeps the tested display, interface and audio improvements. Experimental neighbor-cell preparation, speculative map generation and loader/worker patches are excluded from the stable DLL. Their sources are preserved locally in `.misc/backup/neighbor-streaming`, outside version control.

## Features

- Preloads regular, nonempty files from `Data Files` into a separate **64-bit RAM-cache process** before starting the game. Physical RAM locking is enabled by default.
- Serves supported Win32, legacy CRT, Bink and WAV/MMIO reads from the cache. Unsupported I/O falls back to the original implementation.
- Adds modern resolutions, interface scaling, readable font profiles and a main menu styled around the original palette.
- Provides Fullscreen, Borderless window and Window modes. Fullscreen and Borderless fill the monitor while retaining the selected render resolution; both use windowed Direct3D presentation and leave the desktop resolution unchanged.
- Keeps the Master Volume fixes, system hotkeys and crash/exit diagnostics.

**Cell transitions and their loading screens remain.** Resource caching does not instantiate the entire world or eliminate CPU work such as scene construction, collisions and local-map generation. The original executable on disk is unchanged; hooks are applied to the launched process in memory. Saves are not patched.

## Requirements

- 64-bit Windows; a legally installed **Steam Morrowind 1.6.1820 English**.
- Available RAM exceeding the size of `Data Files` by at least **2 GiB**. The tested installation used about 1.07 GiB for cached resources; your installation may differ.
- The original, unmodified executable with SHA-256:

```text
eac72e1b3524ee0012d080d3c2e2e00e401464e41ee54f9afd6aaaadf2c221f5
```

Other executable versions and executables modified by MCP, MGE/MWSE or a 4 GB patch are not supported by the executable guard. Compatibility with other injected DLLs is unverified. Keep Steam available for the Steam installation.

## Build from source

The project lives directly in the repository root. Install **Visual Studio 2022**, the **Desktop development with C++** workload (x86 and x64 MSVC tools), and a Windows 10/11 SDK. No game files are needed to compile C++. The optional font generator uses the .NET Framework 4 compiler included with Windows.

From the repository root, run PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\fonts\build.ps1
```

The build produces `bin/MorrowindModern.exe` (x64), `runner.exe`, `modernizer.dll` and `cache_probe.exe` (x86). The CRT is linked statically. The second command generates the optional Noto Sans font assets and validates them.

For another Visual Studio edition or location:

```powershell
.\Build.ps1 -VisualStudio 'C:\Program Files\Microsoft Visual Studio\2022\Professional'
```

### Direct compiler commands

To compile without `Build.ps1`, open **x64 Native Tools Command Prompt for VS 2022**, change to the repository root, and run:

```bat
if not exist bin mkdir bin
cl /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS src\host.cpp launcher\settings.cpp audio\game_audio_target.cpp audio\session_mute.cpp /Fo:bin\ /Fe:bin\MorrowindModern.exe /link /MANIFEST:EMBED ole32.lib uuid.lib
```

Then open **x86 Native Tools Command Prompt for VS 2022**, change to the same root, and run:

```bat
cl /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS src\runner.cpp audio\game_audio_target.cpp shutdown\exit_hang_watchdog.cpp /Fo:bin\ /Fe:bin\runner.exe
cl /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS src\cache_probe.cpp /Fo:bin\ /Fe:bin\cache_probe.exe
cl /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /LD src\cache_hook.cpp resolution\resolution.cpp audio\audio_trace.cpp audio\music_volume.cpp display\display.cpp ui\ui.cpp ui\menu_style.cpp ui\options_layout.cpp ui\video_modes.cpp ui\fillbar_fonts.cpp ui\screenshot.cpp /Fo:bin\ /Fe:bin\modernizer.dll /link /DEF:src\modernizer.def user32.lib advapi32.lib
```

Run `fonts/build.ps1` separately if you want Modern Sans fonts or a complete release archive. ClassicReadable uses the game's existing fonts and needs no font generation.

## Install and launch manually

1. Close Morrowind and its mod launcher.
2. Build the mod, or extract a release archive into its own folder, for example `D:\Games\Morrowind\mw_rebreathe`. Do not copy or replace `Morrowind.exe`.
3. Open PowerShell in that folder. Explicitly pass your game directory when installing a font profile:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -GameDirectory 'D:\Games\Morrowind' -FontStyle ClassicReadable -FontSize 16
```

ClassicReadable selects the game's existing Century Gothic font. Start with size 16 and adjust UI scaling in the launcher. Size 20 is optional; some fixed-size game widgets can clip larger text. The installer records the previous font configuration in the game's `.modernization-backup` folder.

For the included Noto Sans profile instead:

```powershell
.\Install.ps1 -GameDirectory 'D:\Games\Morrowind' -FontStyle ModernSans -FontEncoding cp437 -FontSize 16
```

Uninstall an existing font profile before selecting another. `cp437` is the tested English encoding; `cp1251` font assets do not imply support for another game's executable.

4. Start the mod launcher with the same explicit game path:

```powershell
.\bin\MorrowindModern.exe --game-dir 'D:\Games\Morrowind' --settings
```

Choose a display mode, resolution and UI scale, and press Play. Leave RAM locking enabled for a physically resident resource cache. Cache creation runs once per launch; the cache process must remain open while playing. The regular Steam shortcut starts the original game without the mod.

For a direct launch without the settings dialog:

```powershell
.\bin\MorrowindModern.exe --game-dir 'D:\Games\Morrowind' --screen-mode borderless --resolution 1920x1080 --ui-scale 1.5
```

Display settings are saved in `settings.ini` beside the project, and runtime logs are written to `logs/`. `--allow-pageable` permits Windows to evict cached pages if physical RAM locking fails. `--trace` enables additional audio diagnostics. The stable build has no experimental preload option and ignores old `PreloadAdjacentCells` settings.

## Verify and create a release

After the final build and font generation:

```powershell
.\Verify.ps1 -GameDirectory 'D:\Games\Morrowind'
.\Package.ps1 -Version '0.2.0'
```

Verification runs the injected x86 RAM-cache probe and font-installation tests against temporary mock installations. It checks Win32 reads, legacy CRT reads, WAV/MMIO caching and that the original executable remains unchanged. This is not a full interactive gameplay test. The package records hashes of the verified binaries and includes only the files explicitly listed in `release-files.txt`.

The archive and SHA-256 checksum appear in `dist/`. Its contents are rooted at the project level, without a `modernization/` wrapper. Game resources, saves, local settings, raw logs, dumps and the research backup are excluded. The research backup is kept locally under `.misc/` and is not versioned.

Retained component checks can also be run individually:

```powershell
.\launcher\settings-test-build.cmd
.\display\runtime_modes_test-build.cmd
.\display\keyboard-input-test-build.cmd
.\ui\options-layout-test-build.cmd
.\audio\session-mute-test-build.cmd
.\shutdown\exit-hang-watchdog-test-build.cmd
```

The `.cmd` test scripts default to Visual Studio Community. Their artificial windows/processes and test outputs are separate from your installed game.

## Uninstall

Close the game and launcher, then restore the previous font configuration:

```powershell
.\Uninstall.ps1 -GameDirectory 'D:\Games\Morrowind'
```

The script preserves unrelated INI edits and user-modified files. Start the original game executable or Steam shortcut to play without the runtime mod. You can then remove the mod's own folder; retain `.modernization-backup` until restoration is complete.

## Local research archive

Stopped neighbor-streaming work is stored locally in `.misc/backup/neighbor-streaming/`. `STATUS.md` there records the measured progress and unfinished integration. The experiments reached native terrain/filter ownership checks and isolated independent geometry-copy tests; a complete future-scene pipeline and map-cache publication were not finished.

`.misc/.gitignore` ignores everything except itself. The research sources and status files are therefore absent from Git checkouts and release archives. Neither is needed to build or install the stable mod.

## Licenses

Project code, tools, tests and documentation, including the archived development sources, use **GNU GPL version 3 only**. The main [LICENSE](LICENSE) includes the project scope and third-party notices. Noto Sans and the generated bitmap font software retain the [SIL Open Font License 1.1](fonts/OFL.txt), with [source provenance](fonts/source/provenance.json). The font generator is independently written; OpenMW is a format reference. Original game executables, Bethesda assets and game-derived diagnostic captures are not distributed.
