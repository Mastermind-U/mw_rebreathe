@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 ui-test.cpp /Fe:ui-test.exe /Fo:ui-test.obj
if errorlevel 1 exit /b 1
ui-test.exe "..\..\Morrowind.exe"
