@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist bin mkdir bin
cl /nologo /LD /MT /EHsc /std:c++17 /O2 /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /I ..\..\sdk\reshade /I ..\..\sdk\json /Fo:bin\SemanticDepthLiveProbe.obj /Fe:bin\AsteriaDepthSemanticLiveProbe.addon SemanticDepthLiveProbe.cpp
if errorlevel 1 exit /b 1
cl /nologo /MT /EHsc /std:c++17 /O2 /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /I ..\..\sdk\json /Fo:bin\SemanticDepthHostTest.obj /Fe:bin\AsteriaDepthSemanticHost.exe SemanticDepthHostTest.cpp d3d9.lib user32.lib
if errorlevel 1 exit /b 1
endlocal
