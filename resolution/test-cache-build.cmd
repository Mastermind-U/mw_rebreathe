@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl.exe /nologo /W4 /WX /O2 /EHsc /std:c++17 /MT /DUNICODE /D_UNICODE test_cache_handle_reuse.cpp /Fe:test_cache_handle_reuse.exe /Fo:test_cache_handle_reuse.obj
if errorlevel 1 exit /b 1
test_cache_handle_reuse.exe
exit /b %errorlevel%
