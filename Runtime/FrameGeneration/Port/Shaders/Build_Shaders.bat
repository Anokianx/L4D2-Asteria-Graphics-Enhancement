@echo off
setlocal
cd /d "%~dp0"
call "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist build\x86 mkdir build\x86
set "FGSDK=..\..\Research\sdk-v1.1.4\sdk"
set "FGFLAGS=/nologo /c /EHsc /std:c++17 /MT /O2 /DFFX_FI /DFFX_OF /I"Generated" /I"%FGSDK%\include" /I"%FGSDK%\src\components" /I"%FGSDK%\src\backends\shared""
cl %FGFLAGS% /Fo"build\x86\ffx_shader_blobs.obj" "%FGSDK%\src\backends\shared\ffx_shader_blobs.cpp" > "build\x86\shader-build.log" 2>&1
if errorlevel 1 exit /b 1
cl %FGFLAGS% /Fo"build\x86\ffx_frameinterpolation_shaderblobs.obj" "%FGSDK%\src\backends\shared\blob_accessors\ffx_frameinterpolation_shaderblobs.cpp" >> "build\x86\shader-build.log" 2>&1
if errorlevel 1 exit /b 1
cl %FGFLAGS% /Fo"build\x86\ffx_opticalflow_shaderblobs.obj" "%FGSDK%\src\backends\shared\blob_accessors\ffx_opticalflow_shaderblobs.cpp" >> "build\x86\shader-build.log" 2>&1
if errorlevel 1 exit /b 1
lib /nologo /out:"build\x86\AsteriaFGShaders.lib" "build\x86\ffx_shader_blobs.obj" "build\x86\ffx_frameinterpolation_shaderblobs.obj" "build\x86\ffx_opticalflow_shaderblobs.obj" >> "build\x86\shader-build.log" 2>&1
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /MT /O2 /I"%FGSDK%\include" /I"%FGSDK%\src\backends\shared" /Fe:"build\x86\AsteriaFGShaderProbe.exe" /Fo"build\x86\ShaderBlobProbe.obj" ShaderBlobProbe.cpp "build\x86\AsteriaFGShaders.lib" >> "build\x86\shader-build.log" 2>&1
if errorlevel 1 exit /b 1
build\x86\AsteriaFGShaderProbe.exe > "build\x86\shader-probe-result.json"
exit /b %errorlevel%
