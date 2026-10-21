# Font profiles

Run commands from the repository root with Morrowind closed. Explicitly pass the installed game directory.

```powershell
.\Install.ps1 -GameDirectory 'D:\Games\Morrowind' -FontStyle ClassicReadable -FontSize 16
```

ClassicReadable selects the existing Century Gothic game font; no original font assets are redistributed. Size 16 is the recommended starting point with UI scaling. Size 20 enlarges dialogue text, while compact captions remain size 16; some fixed-size widgets can clip large text.

ModernSans is generated from the included OFL-licensed Noto Sans source:

```powershell
.\fonts\build.ps1
.\Install.ps1 -GameDirectory 'D:\Games\Morrowind' -FontStyle ModernSans -FontEncoding cp437 -FontSize 16
```

`cp437` is the tested English encoding. `cp1251` assets support that character encoding, but do not add support for an unverified executable. See `OFL.txt` and `source/provenance.json` for licensing and provenance.

The installer backs up the original font configuration in the game's `.modernization-backup` directory. Restore an existing profile before choosing another:

```powershell
.\Uninstall.ps1 -GameDirectory 'D:\Games\Morrowind'
```

Unrelated INI changes and user-modified files are preserved. `Test-Install.ps1` exercises reversible installation against private mock game folders; it does not install fonts in the real game.
