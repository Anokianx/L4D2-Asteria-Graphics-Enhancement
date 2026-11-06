@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist build\x86 mkdir build\x86
cl /nologo /EHsc /std:c++17 /MT /O2 /DNOMINMAX /DUNICODE /D_UNICODE /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /I"include" /I"..\Research\vulkan-headers-v1.3.250\include" /I"..\Research\sdk-v1.1.4\sdk\include" /I"..\..\sdk\json" /Fo"build\x86\InterpolationProbe.obj" /Fe:"build\x86\AsteriaInterpolationProbe.exe" InterpolationProbe.cpp build\x86\VulkanShim.obj build\x86\AsteriaFGBackend.lib build\x86\AsteriaFGPresenter.lib Shaders\build\x86\AsteriaFGShaders.lib ..\Research\build-cpu-x86\frameinterpolation\ffx_frameinterpolation_x86.lib ..\Research\build-cpu-x86\opticalflow\ffx_opticalflow_x86.lib user32.lib > build\x86\interpolation-build.log 2>&1
exit /b %errorlevel%
