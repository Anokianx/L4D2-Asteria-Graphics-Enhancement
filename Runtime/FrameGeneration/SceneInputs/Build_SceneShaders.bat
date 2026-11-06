@echo off
setlocal
cd /d "%~dp0"
if not exist bin mkdir bin
..\Port\Shaders\Tools\glslangValidator.exe -V --target-env vulkan1.1 Shaders\Scene.vert -o bin\Scene.vert.spv
if errorlevel 1 exit /b 1
..\Port\Shaders\Tools\glslangValidator.exe -V --target-env vulkan1.1 Shaders\Scene.frag -o bin\Scene.frag.spv
if errorlevel 1 exit /b 1
endlocal
