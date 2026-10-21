@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl.exe /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /WX /DUNICODE /D_UNICODE runtime_modes_test.cpp /Fe:runtime_modes_test.exe
if errorlevel 1 exit /b 1
runtime_modes_test.exe
if errorlevel 1 exit /b 1
cl.exe /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /WX /DUNICODE /D_UNICODE test_display.cpp /Fe:test_display.exe
if errorlevel 1 exit /b 1
test_display.exe
