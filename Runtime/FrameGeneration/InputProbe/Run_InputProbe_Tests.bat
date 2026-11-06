@echo off
setlocal
cd /d "%~dp0"
call Build_InputProbe.bat
if errorlevel 1 exit /b 1
call Build_HostTest.bat
if errorlevel 1 exit /b 1
bin\AsteriaNativeInputHostTest.exe
if errorlevel 1 exit /b 1
bin\AsteriaNativeInputHostTest.exe --disabled
if errorlevel 1 exit /b 1
echo Native input discovery tests passed. Native frame generation remains unbound.
endlocal
