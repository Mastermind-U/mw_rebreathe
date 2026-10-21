@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist "artifacts\exit-hang" mkdir "artifacts\exit-hang"
cl.exe /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /WX /DEXIT_HANG_FIXTURE exit_hang_watchdog_test.cpp /Fo:artifacts\exit-hang\fixture.obj /Fe:artifacts\exit-hang\Morrowind.exe /link /BASE:0x400000 /FIXED /DYNAMICBASE:NO
if errorlevel 1 exit /b 1
cl.exe /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /WX /DEXIT_HANG_WATCHDOG_TESTING exit_hang_watchdog_test.cpp exit_hang_watchdog.cpp /Fo:artifacts\exit-hang\ /Fe:artifacts\exit-hang\exit_hang_watchdog_test.exe /link dbghelp.lib
if errorlevel 1 exit /b 1
artifacts\exit-hang\exit_hang_watchdog_test.exe "%~dp0artifacts\exit-hang\Morrowind.exe"
exit /b %errorlevel%
