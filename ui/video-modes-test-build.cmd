@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl.exe /nologo /W4 /WX /O2 /EHsc /std:c++17 /MT /DUNICODE /D_UNICODE video_modes_test.cpp video_modes.cpp /Fe:video-modes-test.exe
if errorlevel 1 exit /b 1
video-modes-test.exe "..\..\Morrowind.exe"
exit /b %errorlevel%
