@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist bin mkdir bin
cl /nologo /LD /MT /EHs /std:c++17 /O2 /DNOMINMAX /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /I ThirdParty\Vulkan-Headers\include /I ..\..\sdk\json /I ..\Port\include /I ..\Port\sources /I ..\Research\sdk-v1.1.4\sdk\include /Fe:bin\AsteriaFrameGenerationLayer.dll AsteriaFrameGenerationLayer.cpp ..\Port\sources\VulkanShim.cpp /link /MAP:bin\AsteriaFrameGenerationLayer.map /DEF:AsteriaFrameGenerationLayer.def "..\Port\build\x86\AsteriaFGBackend.lib" "..\Port\build\x86\AsteriaFGPresenter.lib" "..\Port\Shaders\build\x86\AsteriaFGShaders.lib" "..\Research\build-cpu-x86\frameinterpolation\ffx_frameinterpolation_x86.lib" "..\Research\build-cpu-x86\opticalflow\ffx_opticalflow_x86.lib" user32.lib
if errorlevel 1 exit /b 1
del /q AsteriaFrameGenerationLayer.obj >nul 2>nul
del /q VulkanShim.obj >nul 2>nul
del /q DepthCapture.obj >nul 2>nul
endlocal
