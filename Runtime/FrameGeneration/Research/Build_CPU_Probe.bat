@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
set "TASK_CMAKE=%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
"%TASK_CMAKE%" -S . -B build-cpu-x86 -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release > cpu-source-build-x86.log 2>&1
if errorlevel 1 exit /b 1
"%TASK_CMAKE%" --build build-cpu-x86 >> cpu-source-build-x86.log 2>&1
if errorlevel 1 exit /b 1
build-cpu-x86\AsteriaOfficialFGCoreProbe.exe > cpu-probe-result.json
exit /b %errorlevel%
