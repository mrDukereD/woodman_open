@echo off
rem Builds wood_server.exe (x64). Run from "x64 Native Tools Command Prompt for VS" (or after vcvars64.bat).
rem For x86 use vcvars32.bat, same sources. Output: ..\build\wood_server.exe
setlocal
cd /d "%~dp0"
set OBJ=..\build\obj
set MZ=..\third_party\miniz
if not exist "%OBJ%" mkdir "%OBJ%"
rc /nologo /Fo"%OBJ%\wood_server.res" wood_server.rc || exit /b 1
cl /nologo /O2 /MT /EHsc /utf-8 /W3 /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /D_CRT_SECURE_NO_WARNINGS /I"%MZ%" /Fo"%OBJ%\\" /Fe"..\build\wood_server.exe" main.cpp "%OBJ%\wood_server.res" "%MZ%\miniz.c" "%MZ%\miniz_tdef.c" "%MZ%\miniz_tinfl.c" "%MZ%\miniz_zip.c" || exit /b 1
echo Done: ..\build\wood_server.exe
