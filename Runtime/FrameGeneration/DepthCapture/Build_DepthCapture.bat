@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist bin mkdir bin
..\Port\Shaders\Tools\glslangValidator.exe -V --target-env vulkan1.1 Shaders\ResolveDepth.comp -o bin\ResolveDepth.spv
if errorlevel 1 exit /b 1
..\Port\Shaders\Tools\glslangValidator.exe -V --target-env vulkan1.1 Shaders\Fixture.vert -o bin\Fixture.vert.spv
if errorlevel 1 exit /b 1
..\Port\Shaders\Tools\glslangValidator.exe -V --target-env vulkan1.1 Shaders\Fixture.frag -o bin\Fixture.frag.spv
if errorlevel 1 exit /b 1
cl /nologo /MT /EHs /std:c++17 /O2 /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /I ..\Layer\ThirdParty\Vulkan-Headers\include /I ..\..\sdk\json /Fe:bin\AsteriaDepthCaptureHost.exe /Fo:bin\ HostTest.cpp DepthCapture.cpp
if errorlevel 1 exit /b 1
endlocal
