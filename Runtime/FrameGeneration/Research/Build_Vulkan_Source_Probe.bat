@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist build-vulkan-x86 mkdir build-vulkan-x86
cl /nologo /c /EHsc /std:c++17 /MT /O2 /DUNICODE /D_UNICODE /DNOMINMAX /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /I"vulkan-headers-v1.3.250\include" /I"sdk-v1.1.4\sdk\include" /I"sdk-v1.1.4\sdk\src\shared" /I"sdk-v1.1.4\sdk\src\backends\shared" /Fo"build-vulkan-x86\\" sdk-v1.1.4\sdk\src\backends\vk\ffx_vk.cpp sdk-v1.1.4\sdk\src\backends\vk\FrameInterpolationSwapchain\FrameInterpolationSwapchainVK.cpp sdk-v1.1.4\sdk\src\backends\vk\FrameInterpolationSwapchain\FrameInterpolationSwapchainVK_Helpers.cpp sdk-v1.1.4\sdk\src\backends\vk\FrameInterpolationSwapchain\FrameInterpolationSwapchainVK_UiComposition.cpp sdk-v1.1.4\sdk\src\backends\vk\FrameInterpolationSwapchain\FrameInterpolationSwapchainVK_DebugPacing.cpp
exit /b %errorlevel%
