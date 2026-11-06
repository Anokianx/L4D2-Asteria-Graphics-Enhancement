@echo off
setlocal
set "FG_ROOT=%~dp0"
if not exist "%FG_ROOT%Port\results" mkdir "%FG_ROOT%Port\results"
call "%FG_ROOT%Layer\Tests\Run_Tests.bat"
if errorlevel 1 exit /b 1
py -3 "%FG_ROOT%Layer\Tests\Run_InputFallback.py"
if errorlevel 1 exit /b 1
"%FG_ROOT%Port\build\x86\AsteriaHandleProbe.exe" > "%FG_ROOT%Port\results\handle-probe.json"
if errorlevel 1 exit /b 1
"%FG_ROOT%Port\Shaders\build\x86\AsteriaFGShaderProbe.exe" > "%FG_ROOT%Port\results\shader-probe.json"
if errorlevel 1 exit /b 1
"%FG_ROOT%Port\Shaders\build\x86\AsteriaVulkanShimProbe.exe" > "%FG_ROOT%Port\results\vulkan-shim-probe.json"
if errorlevel 1 exit /b 1
"%FG_ROOT%Port\build\x86\AsteriaLowerDispatchProbe.exe"
if errorlevel 1 exit /b 1
"%FG_ROOT%Probe\bin\AsteriaFGGpuProbe.exe" --layer "%FG_ROOT%Layer\bin\AsteriaFrameGenerationLayer.dll" --validate-lower-dispatch --output "%FG_ROOT%Port\results\layer-routing\gpu-probe.json"
if errorlevel 1 exit /b 1
"%FG_ROOT%Port\build\x86\AsteriaInterpolationProbe.exe" "%FG_ROOT%Port\results\interpolation\interpolation-probe.json"
if errorlevel 1 exit /b 1
"%FG_ROOT%Port\build\x86\AsteriaPresentationProbe.exe" "%FG_ROOT%Port\results\presentation\presentation-probe.json"
if errorlevel 1 exit /b 1
py -3 "%FG_ROOT%Run_EstimatedInput_Tests.py" %*
if errorlevel 1 exit /b 1
py -3 "%FG_ROOT%Validate_Results.py" %*
exit /b %errorlevel%
