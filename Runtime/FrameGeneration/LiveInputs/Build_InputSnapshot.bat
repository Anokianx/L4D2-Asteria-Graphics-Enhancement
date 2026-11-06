@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist bin mkdir bin
cl /nologo /LD /MT /EHsc /std:c++17 /O2 /DNOMINMAX /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /I ..\..\sdk\reshade /I ..\..\sdk\json /Fo:bin\ReShadeInputSnapshot.obj /Fe:bin\AsteriaInputSnapshot.addon ReShadeInputSnapshot.cpp
if errorlevel 1 exit /b 1
cl /nologo /LD /MT /EHsc /std:c++17 /O2 /DNOMINMAX /DASTERIA_SNAPSHOT_SOURCE_OVERWRITE_FIXTURE=1 /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /I ..\..\sdk\reshade /I ..\..\sdk\json /Fo:bin\ReShadeInputSnapshotFixture.obj /Fe:bin\AsteriaInputSnapshotFixture.addon ReShadeInputSnapshot.cpp
if errorlevel 1 exit /b 1
cl /nologo /MT /EHsc /std:c++17 /O2 /DNOMINMAX /D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR /I ..\..\sdk\json /Fo:bin\InputSnapshotHost.obj /Fe:bin\AsteriaInputSnapshotHost.exe InputSnapshotHost.cpp d3d9.lib user32.lib
exit /b %errorlevel%
