@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist bin mkdir bin
cl /nologo /MT /EHs /std:c++17 /O2 /DNOMINMAX /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /I ..\..\sdk\json /Fe:bin\AsteriaFrameAssociationContract.exe /Fo:bin\ FrameAssociationContract.cpp
if errorlevel 1 exit /b 1
endlocal
