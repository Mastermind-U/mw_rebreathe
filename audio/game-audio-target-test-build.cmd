@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl.exe /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /WX /DMW_AUDIO_TARGET_TESTING game_audio_target_test.cpp game_audio_target.cpp /Fe:game_audio_target_child.exe /link /BASE:0x400000 /DYNAMICBASE:NO
if errorlevel 1 exit /b 1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 exit /b 1
cl.exe /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /WX /DMW_AUDIO_TARGET_TESTING game_audio_target_test.cpp game_audio_target.cpp /Fe:game_audio_target_test.exe
if errorlevel 1 exit /b 1
game_audio_target_test.exe "%~dp0game_audio_target_child.exe"
exit /b %errorlevel%
