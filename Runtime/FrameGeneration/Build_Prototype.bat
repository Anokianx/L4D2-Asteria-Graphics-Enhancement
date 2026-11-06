@echo off
setlocal
set "FG_ROOT=%~dp0"
call "%FG_ROOT%Research\Build_CPU_Probe.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Port\Shaders\Build_Shaders.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Port\Shaders\Build_VulkanShim.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Port\Build_Backend.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Port\Build_Presenter.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Port\Build_Handle_Probe.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Port\Build_LowerDispatchProbe.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Port\Build_InterpolationProbe.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Port\Build_PresentationProbe.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Layer\Build_Layer_Probe.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Layer\Tests\Build_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Layer\Tests\Build_Viewport_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Layer\Tests\Build_GraphicsViewport_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Layer\Tests\Build_DrawViewport_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Layer\Tests\Build_PipelineDiagnostic_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Layer\Tests\Build_PipelineDiagnosticCache_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Layer\Tests\Build_NativeSubmitObserver_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Layer\Tests\Build_Swapchain_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Layer\Tests\Build_Input_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Layer\Tests\Build_InputBridge_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Layer\Tests\Build_DeviceFeature_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%Probe\Build_GpuProbe.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%DepthCapture\Build_DepthCapture.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%DepthCapture\Build_Semantic_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%DepthCapture\Build_Semantic_Live.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%FrameAssociation\Build_Contract.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%FrameAssociation\Build_Live_Contract.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%FrameAssociation\Build_PresentTicket_Contract.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%InputProbe\Build_InputProbe.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%InputProbe\Build_HostTest.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%InputProbe\Build_GameProbe.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%InputProbe\Tests\Build_Uniform_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%LiveInputs\Build_InputSnapshot.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%LiveInputs\Build_ProducerCoordinator_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%MotionReconstruction\Build_Motion_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%EstimatedMotion\Build_Flow_Tests.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%EstimatedMotion\Build_EstimatedInterpolation.bat"
if errorlevel 1 exit /b 1
call "%FG_ROOT%SceneInputs\Build_SceneInterpolation.bat"
exit /b %errorlevel%
