@echo off
setlocal
cd /d "%~dp0\.."
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist build\x86 mkdir build\x86
if not exist Shaders\build\x86 mkdir Shaders\build\x86
cl /nologo /c /EHs /std:c++17 /MT /O2 /DNOMINMAX /DUNICODE /D_UNICODE /I"include" /I"..\Research\vulkan-headers-v1.3.250\include" /Fo"build\x86\VulkanShim.obj" "sources\VulkanShim.cpp" > "Shaders\build\x86\vulkan-shim-build.log" 2>&1
if errorlevel 1 exit /b 1
cl /nologo /EHs /std:c++17 /MT /O2 /DNOMINMAX /DUNICODE /D_UNICODE /I"include" /I"..\Research\vulkan-headers-v1.3.250\include" /Fo"Shaders\build\x86\VulkanShimProbe.obj" /Fe:"Shaders\build\x86\AsteriaVulkanShimProbe.exe" "Shaders\VulkanShimProbe.cpp" "build\x86\VulkanShim.obj" >> "Shaders\build\x86\vulkan-shim-build.log" 2>&1
if errorlevel 1 exit /b 1
Shaders\build\x86\AsteriaVulkanShimProbe.exe > "Shaders\build\x86\vulkan-shim-probe-result.json"
exit /b %errorlevel%
