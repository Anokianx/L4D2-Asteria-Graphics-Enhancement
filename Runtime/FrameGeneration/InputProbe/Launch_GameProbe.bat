@echo off
setlocal
cd /d "%~dp0"
if not exist bin\AsteriaNativeInputProbe.addon (
    call Build_InputProbe.bat
    if errorlevel 1 exit /b 1
)
if not exist bin\AsteriaNativeInputGameProbe.exe (
    call Build_GameProbe.bat
    if errorlevel 1 exit /b 1
)
bin\AsteriaNativeInputGameProbe.exe --game "H:\SteamLibrary\steamapps\common\Left 4 Dead 2" %*
exit /b %errorlevel%
