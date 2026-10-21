@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl.exe /nologo /W4 /WX /O2 /EHsc /std:c++17 /MT /DUNICODE /D_UNICODE menu_style-test.cpp /Fe:menu_style-test.exe /Fo:menu_style-test.obj
if errorlevel 1 exit /b 1
menu_style-test.exe "..\..\Morrowind.exe"
exit /b %errorlevel%
