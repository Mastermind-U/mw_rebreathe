@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist "artifacts\exit-wait" mkdir "artifacts\exit-wait"
cl.exe /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /WX /DEXIT_HANG_FIXTURE exit_hang_watchdog_test.cpp /Fo:artifacts\exit-wait\fixture.obj /Fe:artifacts\exit-wait\Morrowind.exe /link /BASE:0x400000 /FIXED /DYNAMICBASE:NO
if errorlevel 1 exit /b 1
cl.exe /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /WX exit_hang_wait_test.cpp exit_hang_watchdog.cpp ..\audio\game_audio_target.cpp /Fo:artifacts\exit-wait\ /Fe:artifacts\exit-wait\exit_hang_wait_test.exe /link dbghelp.lib
if errorlevel 1 exit /b 1
artifacts\exit-wait\exit_hang_wait_test.exe "%~dp0artifacts\exit-wait\Morrowind.exe"
exit /b %errorlevel%
