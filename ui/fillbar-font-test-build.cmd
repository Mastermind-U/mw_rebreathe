@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl.exe /nologo /W4 /O2 /EHsc /MT /std:c++17 fillbar_fonts.cpp fillbar_font_test.cpp /Fe:fillbar-font-test.exe
if errorlevel 1 exit /b 1
fillbar-font-test.exe "..\..\Morrowind.exe"
