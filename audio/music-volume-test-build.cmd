@echo off
setlocal
set "GAME_EXE=%~1"
if not defined GAME_EXE set "GAME_EXE=%~dp0..\..\Morrowind.exe"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl.exe /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /WX /DMUSIC_VOLUME_TESTING music_volume_test.cpp music_volume.cpp /Fe:music_volume_test.exe
if errorlevel 1 exit /b 1
music_volume_test.exe "%GAME_EXE%"
exit /b %errorlevel%
