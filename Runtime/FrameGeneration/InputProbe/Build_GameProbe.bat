@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist bin mkdir bin
if not exist build mkdir build
cl /nologo /MT /EHsc /std:c++17 /O2 /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /I ..\..\sdk\json /Fo:build\NativeInputGameProbe.obj /Fe:bin\AsteriaNativeInputGameProbe.exe NativeInputGameProbe.cpp user32.lib advapi32.lib
if errorlevel 1 exit /b 1
echo Built isolated x86 L4D2 native input game probe.
endlocal
