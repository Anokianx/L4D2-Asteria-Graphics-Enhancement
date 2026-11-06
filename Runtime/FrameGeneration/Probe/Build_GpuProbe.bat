@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist bin mkdir bin
cl /nologo /MT /EHsc /std:c++17 /O2 /I ..\Layer\ThirdParty\Vulkan-Headers\include /I ..\..\sdk\json /Fe:bin\AsteriaFGGpuProbe.exe /Fo:bin\GpuProbe.obj GpuProbe.cpp
if errorlevel 1 exit /b 1
echo Asteria FG GPU probe built.
endlocal
