@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl.exe /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /DUNICODE /D_UNICODE native-ni-abi-test.cpp /Fe:native-ni-abi-test.exe
if errorlevel 1 exit /b 1
native-ni-abi-test.exe "..\..\Morrowind.exe"
