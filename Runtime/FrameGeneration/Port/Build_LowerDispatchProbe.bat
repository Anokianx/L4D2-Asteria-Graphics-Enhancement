@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist build\x86 mkdir build\x86
py -3 Shaders\Generate_Vulkan_Shim.py
if errorlevel 1 exit /b 1
cl /nologo /EHs /std:c++17 /MT /O2 /DNOMINMAX /DUNICODE /D_UNICODE /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /Iinclude /I..\Research\vulkan-headers-v1.3.250\include /I..\..\sdk\json /Fo:build\x86\ /Fe:build\x86\AsteriaLowerDispatchProbe.exe sources\VulkanShim.cpp LowerDispatchProbe.cpp user32.lib > build\x86\lower-dispatch-build.log 2>&1
if errorlevel 1 exit /b 1
build\x86\AsteriaLowerDispatchProbe.exe
exit /b %errorlevel%
