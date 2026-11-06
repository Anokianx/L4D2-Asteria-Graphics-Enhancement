@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist build\x86 mkdir build\x86
cl /nologo /c /EHsc /std:c++17 /MT /O2 /DUNICODE /D_UNICODE /DNOMINMAX /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /I"include" /I"..\Research\vulkan-headers-v1.3.250\include" /I"..\Research\sdk-v1.1.4\sdk\include" /I"..\Research\sdk-v1.1.4\sdk\src\shared" /I"..\Research\sdk-v1.1.4\sdk\src\backends\shared" /Fo"build\x86\\" sources\FrameInterpolationSwapchain\FrameInterpolationSwapchainVK.cpp sources\FrameInterpolationSwapchain\FrameInterpolationSwapchainVK_UiComposition.cpp sources\FrameInterpolationSwapchain\FrameInterpolationSwapchainVK_DebugPacing.cpp sources\FrameInterpolationSwapchain\FrameInterpolationSwapchainVK_Helpers.cpp > build\x86\presenter-build.log 2>&1
if errorlevel 1 exit /b 1
lib /nologo /out:build\x86\AsteriaFGPresenter.lib build\x86\FrameInterpolationSwapchainVK.obj build\x86\FrameInterpolationSwapchainVK_UiComposition.obj build\x86\FrameInterpolationSwapchainVK_DebugPacing.obj build\x86\FrameInterpolationSwapchainVK_Helpers.obj >> build\x86\presenter-build.log 2>&1
exit /b %errorlevel%
