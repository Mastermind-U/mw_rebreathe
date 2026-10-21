@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl.exe /nologo /std:c++17 /utf-8 /O2 /EHsc /MT /W4 /WX /DAUDIO_TRACE_TESTING audio_trace_test.cpp audio_trace.cpp /Fe:audio_trace_test.exe
if errorlevel 1 exit /b 1
audio_trace_test.exe "..\..\Morrowind.exe"
