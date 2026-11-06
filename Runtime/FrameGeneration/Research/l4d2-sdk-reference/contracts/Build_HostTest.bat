@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist build mkdir build
cl /nologo /MT /EHsc /std:c++17 /O2 /I ..\..\..\..\sdk\json /Fo:build\EngineCameraHostTest.obj /Fe:build\EngineCameraHostTest.exe EngineCameraHostTest.cpp
if errorlevel 1 exit /b 1
build\EngineCameraHostTest.exe
if errorlevel 1 exit /b 1
endlocal
