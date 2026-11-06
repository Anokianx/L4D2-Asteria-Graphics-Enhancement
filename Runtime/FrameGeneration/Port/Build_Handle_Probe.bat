@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist build\x86 mkdir build\x86
cl /nologo /MT /EHsc /std:c++17 /O2 /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /Iinclude /I..\Layer\ThirdParty\Vulkan-Headers\include /Fe:build\x86\AsteriaHandleProbe.exe /Fo:build\x86\ AsteriaVkHandles.cpp HandleProbe.cpp
if errorlevel 1 exit /b 1
build\x86\AsteriaHandleProbe.exe
exit /b %errorlevel%
