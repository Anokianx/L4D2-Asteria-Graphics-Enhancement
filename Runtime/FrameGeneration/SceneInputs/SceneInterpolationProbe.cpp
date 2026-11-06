#define main AsteriaSyntheticEstimatedInterpolationMain
#include "../EstimatedMotion/EstimatedInterpolationProbe.cpp"
#undef main
#include "GpuScene.h"
#include "../Layer/FrameGenerationInputs.h"
#include <mutex>

#include "CrossDllSceneSession.h"

static void ClearMask(Gpu &gpu,Image &image)
{
    const auto clear=reinterpret_cast<PFN_vkCmdClearColorImage>(vkGetDeviceProcAddr(gpu.device,"vkCmdClearColorImage"));
    Require(clear!=nullptr,"GPU mask clear entry point unavailable");
    gpu.Barrier(image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue value{};
    const VkImageSubresourceRange pixels{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    clear(gpu.command,image.handle,image.layout,&value,1,&pixels);
    gpu.Barrier(image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static void CopyImage(Gpu &gpu,Image &source,Image &destination)
{
    const auto copy=reinterpret_cast<PFN_vkCmdCopyImage>(vkGetDeviceProcAddr(gpu.device,"vkCmdCopyImage"));
    Require(copy!=nullptr,"GPU history copy entry point unavailable");
    gpu.Barrier(source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); gpu.Barrier(destination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy region{};
    region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    region.extent={Width,Height,1};
    copy(gpu.command,source.handle,source.layout,destination.handle,destination.layout,1,&region);
    gpu.Barrier(source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); gpu.Barrier(destination,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

int main(int argc,char **argv)
{
    const std::filesystem::path root=argc>1 ? argv[1] : ".";
    const auto output=root/"results"/"SceneInterpolation";
    std::filesystem::create_directories(output);
    constexpr uint32_t InputFrames=8;
    constexpr int StepPixels=12;
    json report={{"author","Anokianx"},{"passed",false},{"process_bits",sizeof(void*)*8},
        {"synthetic_scene",true},{"color_and_depth_written_by_same_gpu_graphics_pass",true},
        {"cpu_color_uploaded",false},{"cpu_depth_uploaded",false},{"cpu_motion_uploaded",false},
        {"depth_copied_before_original_clear",true},{"normal_z",true},{"source_samples",4},
        {"width",Width},{"height",Height},{"source_depth_format","VK_FORMAT_D24_UNORM_S8_UINT"},
        {"resolved_depth_format","VK_FORMAT_R32_SFLOAT"},{"synchronous_queue_idle_fixture",true},{"camera_source","explicit_controlled_fixture_constants"},{"hud_absent_in_controlled_fixture",true},
        {"provider_scope","cross_dll_C_ABI_controlled_fixture_only"},{"production_provider_registered",false},
        {"main_game_view_confirmed",false},{"gpu_l4d2_frame_association_verified",false},{"real_game_inputs",false},{"game_started",false},{"game_frame_generation",false},
        {"motion_source","Estimated"},{"motion_input_origin","GPU_AMD_OF_bidirectional_confidence_filter"},
        {"ground_truth_motion_uploaded",false},{"motion_uv_convention","previousUV-currentUV; top-left origin; scale=(width,height)"},
        {"synthetic_static_warmup_dispatches_per_context",6},{"warmup_frames_are_real_game_frames",false},
        {"active_optical_flow_dispatches_per_input_frame",3},{"input_frames",InputFrames},
        {"generated_frames_presented",0},{"hud_composition_validated",false},{"frames",json::array()}};
    std::string stage="initialize";
    try
    {
        Require(sizeof(void*)==4,"Estimated interpolation fixture requires x86");
        auto session=std::make_shared<CompletedSceneSession>(root);
        report["controlled_provider_registered_before_device"] = true;
        auto gpuOwner=std::make_shared<Gpu>(); auto &gpu=*gpuOwner;
        gpu.Initialize(); gpu.images.reserve(24);
        session->AttachDevice(reinterpret_cast<uintptr_t>(gpu.device));
        GpuScene scene(gpu,root,root/"../DepthCapture/bin/ResolveDepth.spv");
        Require(!session->Acquire(0),"Session must not expose a frame before GPU completion");
        GpuTimestamps timestamps(gpu); report["gpu_timing"]=timestamps.Metadata();
        report["gpu_timing"]["excludes_scene_graphics_capture_copy_and_readback"]=true;
        report["gpu"]=gpu.properties.deviceName;
        VkDeviceContext deviceContext{gpu.device,gpu.physical,vkGetDeviceProcAddr};
        const size_t scratchSize=ffxGetScratchMemorySizeVK(gpu.physical,3);
        std::vector<uint8_t> scratch(scratchSize); FfxInterface backend{};
        CheckFfx(ffxGetInterfaceVK(&backend,ffxGetDeviceVK(&deviceContext),scratch.data(),scratch.size(),3),"ffxGetInterfaceVK(three contexts)");
        report["backend_max_contexts"]=3; report["scratch_bytes"]=scratchSize;
        Views views{gpu.device}; FlowContexts flow; flow.device=gpu.device;
        FfxOpticalflowContextDescription flowDescription{}; flowDescription.resolution={Width,Height}; flowDescription.backendInterface=backend;
        CheckFfx(ffxOpticalflowContextCreate(flow.forward.get(),&flowDescription),"forward optical flow context"); flow.forwardCreated=true;
        CheckFfx(ffxOpticalflowContextCreate(flow.reverse.get(),&flowDescription),"reverse optical flow context"); flow.reverseCreated=true;
        Effects interpolation; interpolation.device=gpu.device;
        FfxFrameInterpolationContextDescription description{};
        description.maxRenderSize=description.displaySize={Width,Height}; description.backendInterface=backend;
        description.backBufferFormat=description.previousInterpolationSourceFormat=FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
        CheckFfx(ffxFrameInterpolationContextCreate(interpolation.interpolation.get(),&description),"frame interpolation context");
        interpolation.interpolationCreated=true;
        FfxOpticalflowSharedResourceDescriptions ofShared{}; FfxFrameInterpolationSharedResourceDescriptions fiShared{};
        CheckFfx(ffxOpticalflowGetSharedResourceDescriptions(flow.forward.get(),&ofShared),"optical flow shared descriptions");
        CheckFfx(ffxFrameInterpolationGetSharedResourceDescriptions(interpolation.interpolation.get(),&fiShared),"interpolation shared descriptions");
        auto &previous=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R8G8B8A8_UNORM));
        auto &current=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R8G8B8A8_UNORM));
        auto &oldDepth=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R32_FLOAT));
        auto &newDepth=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R32_FLOAT));
        auto &oldMask=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R32_FLOAT));
        auto &newMask=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R32_FLOAT));
        auto &forward=gpu.CreateImage(ofShared.opticalFlowVector.resourceDescription);
        auto &reverse=gpu.CreateImage(ofShared.opticalFlowVector.resourceDescription);
        auto &forwardScd=gpu.CreateImage(ofShared.opticalFlowSCD.resourceDescription);
        auto &reverseScd=gpu.CreateImage(ofShared.opticalFlowSCD.resourceDescription);
        auto &motion=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R16G16_FLOAT));
        auto &confidence=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R32_FLOAT));
        auto &reconstructed=gpu.CreateImage(fiShared.reconstructedPrevNearestDepth.resourceDescription);
        auto &dilatedDepth=gpu.CreateImage(fiShared.dilatedDepth.resourceDescription);
        auto &dilatedMotion=gpu.CreateImage(fiShared.dilatedMotionVectors.resourceDescription);
        auto &generated=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R8G8B8A8_UNORM));
        auto &readback=gpu.CreateBuffer();
        const std::array<ImageBinding,10> bindings={{{views.Create(current),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {views.Create(previous),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{views.Create(newDepth),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {views.Create(oldDepth),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{views.Create(forward),VK_IMAGE_LAYOUT_GENERAL},
            {views.Create(reverse),VK_IMAGE_LAYOUT_GENERAL},{views.Create(newMask),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {views.Create(oldMask),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{views.Create(forwardScd),VK_IMAGE_LAYOUT_GENERAL},
            {views.Create(reverseScd),VK_IMAGE_LAYOUT_GENERAL}}};
        const auto motionView=views.Create(motion),confidenceView=views.Create(confidence);
        BidirectionalFlowMotion filter;
        Require(filter.Initialize(gpu.device,LoadCode(root/"../EstimatedMotion/FilterFlow.spv")),"Estimated motion filter initialization failed");
        gpu.Begin();
        for(auto image:{&forward,&reverse,&forwardScd,&reverseScd,&motion,&confidence,&reconstructed,&dilatedDepth,&dilatedMotion,&generated})
            gpu.Barrier(*image,VK_IMAGE_LAYOUT_GENERAL);
        gpu.Submit();
        stage="synthetic_static_warmup";
        gpu.Begin();
        scene.Record(gpu,previous,oldDepth,0,0,0);
        ClearMask(gpu,oldMask); ClearMask(gpu,newMask);
        gpu.Submit();
        auto precedingColor=gpu.Readback(previous,readback);
        auto precedingDepth=gpu.Readback(oldDepth,readback);
        gpu.Begin(); gpu.Barrier(previous,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        gpu.Barrier(oldDepth,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); gpu.Submit();
        uint32_t forwardCalls=0,reverseCalls=0;
        for(uint32_t i=0;i<6;++i)
        {
            DispatchFlow(gpu,flow.forward.get(),previous,forward,forwardScd,i==0); ++forwardCalls;
            DispatchFlow(gpu,flow.reverse.get(),previous,reverse,reverseScd,i==0); ++reverseCalls;
        }
        uint32_t verified=0;
        for(uint32_t frame=0;frame<InputFrames;++frame)
        {
            stage="continuous_frame_"+std::to_string(frame);
            const auto started=std::chrono::steady_clock::now();
            gpu.Begin(); scene.Record(gpu,current,newDepth,int(frame)*StepPixels,0,frame); gpu.Submit();
            const auto incomingColor=gpu.Readback(current,readback);
            const auto incomingDepth=gpu.Readback(newDepth,readback);
            const auto expectedScene=MakeScene(int(frame)*StepPixels,0);
            double maxDepthError=0;
            uint32_t depthPixels=0;
            for(size_t pixel=0;pixel<size_t(Width)*Height;++pixel)
            {
                const auto depth=ReadFloat(incomingDepth,pixel);
                const auto depthError=std::abs(double(depth)-expectedScene.depth[pixel]);
                Require(std::isfinite(depth)&&depthError<1e-6,"Captured graphics depth must match the same-frame scene after source clear");
                maxDepthError=std::max(maxDepthError,depthError); ++depthPixels;
                for(size_t channel=0;channel<4;++channel)
                    Require(std::abs(int(incomingColor[pixel*4+channel])-int(expectedScene.color[pixel*4+channel]))<=1,
                            "Readback graphics color must match same-frame depth scene");
            }
            gpu.Begin();
            for(auto image:{&previous,&current,&oldDepth,&newDepth}) gpu.Barrier(*image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            gpu.Submit();
            TimedDispatchFlow(gpu,flow.forward.get(),current,forward,forwardScd,timestamps,0); ++forwardCalls;
            TimedDispatchFlow(gpu,flow.reverse.get(),current,reverse,reverseScd,timestamps,2); ++reverseCalls;
            TimedDispatchFlow(gpu,flow.reverse.get(),previous,reverse,reverseScd,timestamps,4); ++reverseCalls;
            const auto forwardHistory=SceneHistory(gpu.Readback(forwardScd,readback));
            const auto reverseHistory=SceneHistory(gpu.Readback(reverseScd,readback));
            const auto rawForward=gpu.Readback(forward,readback),rawReverse=gpu.Readback(reverse,readback);
            Parameters parameters; parameters.width=Width; parameters.height=Height;
            parameters.historyValid=((forwardHistory|reverseHistory)&15u)==0 ? 1 : 0;
            parameters.forwardHistoryFrames=forwardCalls-1; parameters.reverseHistoryFrames=reverseCalls-1;
            parameters.photometricLimit=0.06f;
            gpu.Begin(); timestamps.Begin(gpu.command,6); gpu.Barrier(motion,VK_IMAGE_LAYOUT_GENERAL); gpu.Barrier(confidence,VK_IMAGE_LAYOUT_GENERAL);
            VkMemoryBarrier visibility{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; visibility.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
            visibility.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(gpu.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&visibility,0,nullptr,0,nullptr);
            auto filterLifetime=filter.Record(gpu.command,bindings,motionView,confidenceView,parameters);
            Require(bool(filterLifetime),"Estimated interpolation filter record failed"); timestamps.End(gpu.command,6); gpu.Submit(); filterLifetime.reset();
            const auto estimated=gpu.Readback(motion,readback),quality=gpu.Readback(confidence,readback);
            uint32_t accepted=0,tested=0; double error=0;
            for(int y=128;y<224;++y)
                for(int x=240+int(frame)*StepPixels;x<312+int(frame)*StepPixels;++x)
                {
                    ++tested; const auto pixel=size_t(y)*Width+x;
                    if(ReadFloat(quality,pixel)>=parameters.minimumConfidence)
                    {
                        ++accepted; const auto vector=ReadMotion(estimated,pixel);
                        error+=std::hypot(vector[0]+(frame?StepPixels:0),vector[1]);
                    }
                }
            const double coverage=double(accepted)/tested,meanError=accepted?error/accepted:1e9;
            Require(coverage>0.30 && meanError<1.25,"Continuous estimated motion coverage or endpoint accuracy failed");
            gpu.Begin(); gpu.Barrier(motion,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            gpu.Submit();
            auto frameInput=std::make_shared<asteria_fg_runtime::FrameInputs>();
            frameInput->device=reinterpret_cast<uintptr_t>(gpu.device); frameInput->frameID=frame;
            frameInput->width=Width; frameInput->height=Height; frameInput->deltaMilliseconds=1000.0f/60;
            frameInput->depth=gpu.Resource(newDepth,L"Captured GPU scene depth");
            frameInput->motion=gpu.Resource(motion,L"GPU estimated scene motion");
            frameInput->motionScale={float(Width),float(Height)};
            frameInput->nearPlane=0.1f; frameInput->farPlane=100; frameInput->verticalFovRadians=1;
            frameInput->metersPerUnit=1; frameInput->up[1]=frameInput->right[0]=frameInput->forward[2]=1;
            frameInput->motionSource=asteria_fg_runtime::MotionInputSource::Estimated;
            frameInput->sceneDepth=frameInput->synchronized=frameInput->hudCompositionVerified=true;
            frameInput->historyReset=frame==0; frameInput->lifetime=gpuOwner;
            Require(!session->Acquire(frame),"Unpublished frame must remain unavailable");
            Require(session->Publish(frameInput),"Completed GPU inputs must publish through their exact device and frame session");
            auto acquired=session->Acquire(frame);
            Require(acquired&& !session->Acquire(frame+1),"Provider session must accept only the exact published frame ticket");
            frameInput.reset();
            gpu.Begin(); timestamps.Begin(gpu.command,8);
            FfxFrameInterpolationPrepareDescription prepare{}; prepare.commandList=ffxGetCommandListVK(gpu.command);
            prepare.renderSize={Width,Height}; prepare.motionVectorScale=acquired->motionScale;
            prepare.frameTimeDelta=acquired->deltaMilliseconds; prepare.cameraNear=acquired->nearPlane; prepare.cameraFar=acquired->farPlane; prepare.cameraFovAngleVertical=acquired->verticalFovRadians;
            prepare.viewSpaceToMetersFactor=acquired->metersPerUnit; prepare.depth=acquired->depth;
            prepare.motionVectors=acquired->motion; prepare.frameID=acquired->frameID;
            prepare.dilatedDepth=gpu.Resource(dilatedDepth,L"Estimated interpolation dilated depth",FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            prepare.dilatedMotionVectors=gpu.Resource(dilatedMotion,L"Estimated interpolation dilated motion",FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            prepare.reconstructedPrevDepth=gpu.Resource(reconstructed,L"Estimated interpolation reconstructed previous depth",FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            prepare.cameraUp[1]=1; prepare.cameraRight[0]=1; prepare.cameraForward[2]=1;
            CheckFfx(ffxFrameInterpolationPrepare(interpolation.interpolation.get(),&prepare),"estimated frame interpolation prepare");
            FfxFrameInterpolationDispatchDescription dispatch{}; dispatch.commandList=prepare.commandList;
            dispatch.displaySize=dispatch.renderSize={Width,Height}; dispatch.currentBackBuffer=gpu.Resource(current,L"Synthetic current image");
            dispatch.output=gpu.Resource(generated,L"Estimated motion generated intermediate",FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            dispatch.interpolationRect={0,0,Width,Height};
            dispatch.opticalFlowVector=gpu.Resource(forward,L"Official AMD optical flow",FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            dispatch.opticalFlowSceneChangeDetection=gpu.Resource(forwardScd,L"Official AMD SCD",FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            dispatch.opticalFlowBufferSize={forward.description.width,forward.description.height};
            dispatch.opticalFlowScale={1.0f/Width,1.0f/Height}; dispatch.opticalFlowBlockSize=8;
            dispatch.cameraNear=prepare.cameraNear; dispatch.cameraFar=prepare.cameraFar;
            dispatch.cameraFovAngleVertical=prepare.cameraFovAngleVertical; dispatch.viewSpaceToMetersFactor=prepare.viewSpaceToMetersFactor;
            dispatch.frameTimeDelta=prepare.frameTimeDelta; dispatch.reset=frame==0; dispatch.frameID=frame;
            dispatch.backBufferTransferFunction=FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB; dispatch.minMaxLuminance[0]=0; dispatch.minMaxLuminance[1]=1;
            dispatch.dilatedDepth=prepare.dilatedDepth; dispatch.dilatedMotionVectors=prepare.dilatedMotionVectors;
            dispatch.reconstructedPrevDepth=prepare.reconstructedPrevDepth;
            CheckFfx(ffxFrameInterpolationDispatch(interpolation.interpolation.get(),&dispatch),"estimated frame interpolation dispatch"); timestamps.End(gpu.command,8); gpu.Submit();
            const auto gpuTiming=timestamps.ReadFrame();
            const auto middle=gpu.Readback(generated,readback);
            const double before=Centroid(precedingColor),after=Centroid(incomingColor),intermediate=Centroid(middle);
            const size_t differentPrevious=DifferentPixels(middle,precedingColor),differentCurrent=DifferentPixels(middle,incomingColor);
            const double expected=(before+after)*0.5,centroidError=std::abs(intermediate-expected);
            const bool midpoint=frame && intermediate>before+1 && intermediate<after-1 && centroidError<1.0 && differentPrevious>500 && differentCurrent>500;
            if(frame) { Require(midpoint,"Estimated MV generated frame failed actual midpoint position and distinct-color checks"); ++verified; }
            const auto directory=output/("frame_"+std::to_string(frame)); std::filesystem::create_directories(directory);
            SaveImage(directory/"previous.ppm",precedingColor); SaveImage(directory/"current.ppm",incomingColor); SaveImage(directory/"generated.ppm",middle);
            SaveBytes(directory/"previous_depth.r32f",precedingDepth,size_t(Width)*Height*4);
            SaveBytes(directory/"current_depth.r32f",incomingDepth,size_t(Width)*Height*4);
            SaveBytes(directory/"estimated_motion.rg16f",estimated,size_t(Width)*Height*4);
            SaveBytes(directory/"confidence.r32f",quality,size_t(Width)*Height*4);
            SaveBytes(directory/"forward_flow.rg16i",rawForward,size_t(forward.description.width)*forward.description.height*4);
            SaveBytes(directory/"reverse_flow.rg16i",rawReverse,size_t(reverse.description.width)*reverse.description.height*4);
            const double wallMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            json item={{"frame_id",frame},{"reset",frame==0},{"optical_flow_dispatches",3},{"forward_context_frame_index",forwardCalls-1},
                {"reverse_context_frame_index",reverseCalls-1},{"forward_scd_history",forwardHistory},{"reverse_scd_history",reverseHistory},
                {"depth_pixels_verified",depthPixels},{"max_depth_absolute_error",maxDepthError},{"provider_frame_id",acquired->frameID},{"coverage",coverage},{"accepted_object_pixels",accepted},{"tested_object_pixels",tested},{"mean_motion_endpoint_error_pixels",meanError},
                {"previous_centroid_x",before},{"current_centroid_x",after},{"generated_centroid_x",intermediate},{"expected_centroid_x",expected},
                {"midpoint_centroid_error_pixels",centroidError},{"pixels_different_from_previous",differentPrevious},{"pixels_different_from_current",differentCurrent},
                {"midpoint_verified",midpoint},{"synchronous_fixture_wall_ms_including_graphics_capture_readback_and_file_io",wallMilliseconds},
                {"gpu_timing_available",gpuTiming["available"]},{"gpu_work_ms",gpuTiming["gpu_work_ms"]},
                {"gpu_timestamp_ticks",gpuTiming["raw_ticks"]},{"gpu_timing_unavailable_reason",gpuTiming["unavailable_reason"]},
                {"readback_directory",directory.string()}};
            report["frames"].push_back(item); std::cout<<item.dump()<<std::endl;
            acquired.reset();
            session->RetireAfterGpuIdle();
            Require(!session->Acquire(frame),"Completed input must retire before its resource slot is reused");
            gpu.Begin(); CopyImage(gpu,current,previous); CopyImage(gpu,newDepth,oldDepth); gpu.Submit();
            precedingColor=incomingColor; precedingDepth=incomingDepth;
        }
        report["official_interpolation_dispatches"]=InputFrames; report["verified_estimated_motion_midpoints"]=verified;
        report["forward_of_dispatches_total"]=forwardCalls; report["reverse_of_dispatches_total"]=reverseCalls;
        report["all_of_dispatches_including_synthetic_warmup"]=forwardCalls+reverseCalls;
        report["gpu_timing_summary"]=TimingSummary(report["frames"]);
        report["passed"]=verified==InputFrames-1;
        report["gpu_scene_depth_estimated_interpolation_verified"]=report["passed"];
        report["frame_input_session_contract_verified"]=true;
        Require(session->SubmittedLeases()==InputFrames && session->ReleasedLeases()==InputFrames,"All cross-DLL GPU input leases must retire after completion");
        report["cross_dll_native_image_leases"]={{"submissions",session->SubmittedLeases()},{"releases",session->ReleasedLeases()},{"actual_gpu_images",true},{"consumer_session_is_controlled",true},{"actual_swapchain_present_ticket",false}};
        stage="complete";
    }
    catch(const std::exception &error) { report["error"]=error.what(); std::cerr<<stage<<": "<<error.what()<<std::endl; }
    report["stage"]=stage;
    std::ofstream file(output/"scene_interpolation.json"); file<<report.dump(2)<<'\n';
    return file && report["passed"].get<bool>()?0:1;
}
