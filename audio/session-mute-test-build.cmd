@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl.exe /nologo /std:c++17 /utf-8 /EHsc /MT /W4 /WX session_probe.cpp /Fe:session_probe.exe /link ole32.lib uuid.lib
if errorlevel 1 exit /b 1
if "%~1"=="--observe-only" goto observe
cl.exe /nologo /std:c++17 /utf-8 /EHsc /MT /W4 /WX /DAUDIO_SESSION_MUTE_TESTING session_mute_test.cpp session_mute.cpp /Fe:session_mute_test.exe /link ole32.lib uuid.lib
if errorlevel 1 exit /b 1
session_mute_test.exe
exit /b %errorlevel%
:observe
session_probe.exe --observe-only %2
exit /b %errorlevel%
