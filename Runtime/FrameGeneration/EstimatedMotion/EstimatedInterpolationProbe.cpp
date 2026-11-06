#define ASTERIA_FLOW_PROBE_LIBRARY
#include "GpuFlowContract.cpp"
#include <chrono>

class GpuTimestamps
{
    VkDevice device=VK_NULL_HANDLE;
    VkQueryPool pool=VK_NULL_HANDLE;
    PFN_vkDestroyQueryPool destroy=nullptr;
    PFN_vkCmdResetQueryPool reset=nullptr;
    PFN_vkCmdWriteTimestamp write=nullptr;
    PFN_vkGetQueryPoolResults read=nullptr;
    uint32_t validBits=0;
    double period=0;
    std::string unavailable="GPU timestamps unsupported";
public:
    inline static const std::array<const char *,5> Segments{{"forward_of","reverse_seed_of","reverse_pair_of","filter","prepare_and_fi"}};
    explicit GpuTimestamps(const Gpu &gpu) : device(gpu.device)
    {
        uint32_t count=0; vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical,&count,nullptr);
        std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical,&count,families.data());
        if(gpu.family>=count) { unavailable="Selected queue family unavailable"; return; }
        validBits=families[gpu.family].timestampValidBits; period=gpu.properties.limits.timestampPeriod;
        if(!validBits || validBits>64 || !std::isfinite(period) || period<=0) { unavailable="Queue timestamp bits or period unsupported"; return; }
        const auto create=reinterpret_cast<PFN_vkCreateQueryPool>(vkGetDeviceProcAddr(device,"vkCreateQueryPool"));
        destroy=reinterpret_cast<PFN_vkDestroyQueryPool>(vkGetDeviceProcAddr(device,"vkDestroyQueryPool"));
        reset=reinterpret_cast<PFN_vkCmdResetQueryPool>(vkGetDeviceProcAddr(device,"vkCmdResetQueryPool"));
        write=reinterpret_cast<PFN_vkCmdWriteTimestamp>(vkGetDeviceProcAddr(device,"vkCmdWriteTimestamp"));
        read=reinterpret_cast<PFN_vkGetQueryPoolResults>(vkGetDeviceProcAddr(device,"vkGetQueryPoolResults"));
        if(!create || !destroy || !reset || !write || !read) { unavailable="Required device timestamp entry point unavailable"; return; }
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; info.queryType=VK_QUERY_TYPE_TIMESTAMP; info.queryCount=10;
        const auto result=create(device,&info,nullptr,&pool);
        if(result!=VK_SUCCESS) { pool=VK_NULL_HANDLE; unavailable="vkCreateQueryPool returned "+std::to_string(result); return; }
        unavailable.clear();
    }
    GpuTimestamps(const GpuTimestamps &)=delete;
    GpuTimestamps &operator=(const GpuTimestamps &)=delete;
    ~GpuTimestamps()
    {
        if(pool) { vkDeviceWaitIdle(device); destroy(device,pool,nullptr); }
    }
    void Begin(VkCommandBuffer command,uint32_t slot)
    {
        if(pool) { reset(command,pool,slot,2); write(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool,slot); }
    }
    void End(VkCommandBuffer command,uint32_t slot)
    {
        if(pool) write(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pool,slot+1);
    }
    json Metadata() const
    {
        return {{"available",pool!=VK_NULL_HANDLE},{"unavailable_reason",pool ? json(nullptr) : json(unavailable)},
            {"queue_timestamp_valid_bits",validBits},{"timestamp_period_nanoseconds",std::isfinite(period)&&period>0 ? json(period) : json(nullptr)},
            {"query_slots",10},{"stages","TOP_OF_PIPE_to_BOTTOM_OF_PIPE"},{"counter_difference","masked_modulo_queue_timestampValidBits"},
            {"scope","640x360_synchronous_synthetic_fixture_GPU_commands_only"},{"game_fps_measured",false},{"four_k_performance_measured",false}};
    }
    json ReadFrame()
    {
        json milliseconds=json::object(),raw=json::object();
        for(const auto name:Segments) milliseconds[name]=nullptr;
        milliseconds["sum"]=nullptr;
        json result={{"available",false},{"gpu_work_ms",milliseconds},{"raw_ticks",raw},{"unavailable_reason",unavailable}};
        if(!pool) return result;
        std::array<uint64_t,10> ticks{};
        const auto status=read(device,pool,0,10,sizeof(ticks),ticks.data(),sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);
        if(status!=VK_SUCCESS) { result["unavailable_reason"]="vkGetQueryPoolResults returned "+std::to_string(status); return result; }
        const uint64_t mask=validBits==64 ? UINT64_MAX : (uint64_t(1)<<validBits)-1;
        double sum=0;
        for(uint32_t segment=0;segment<Segments.size();++segment)
        {
            const uint64_t first=ticks[segment*2]&mask,last=ticks[segment*2+1]&mask,delta=(last-first)&mask;
            const double duration=double(delta)*period/1000000.0;
            milliseconds[Segments[segment]]=duration; sum+=duration;
            raw[Segments[segment]]={{"begin",first},{"end",last},{"masked_delta",delta}};
        }
        milliseconds["sum"]=sum;
        return {{"available",true},{"gpu_work_ms",milliseconds},{"raw_ticks",raw},{"unavailable_reason",nullptr}};
    }
};

static void TimedDispatchFlow(Gpu &gpu,FfxOpticalflowContext *context,Image &color,Image &vectors,Image &scd,
                              GpuTimestamps &timestamps,uint32_t slot)
{
    gpu.Begin(); timestamps.Begin(gpu.command,slot);
    FfxOpticalflowDispatchDescription description{};
    description.commandList=ffxGetCommandListVK(gpu.command); description.color=gpu.Resource(color,L"Timed estimated motion color");
    description.opticalFlowVector=gpu.Resource(vectors,L"Timed estimated optical flow",FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    description.opticalFlowSCD=gpu.Resource(scd,L"Timed estimated scene change",FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    description.reset=false; description.backbufferTransferFunction=FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB; description.minMaxLuminance={0,1};
    CheckFfx(ffxOpticalflowContextDispatch(context,&description),"Timed optical flow dispatch");
    timestamps.End(gpu.command,slot); gpu.Submit();
}

static json TimingSummary(const json &frames)
{
    json summary={{"available",false},{"scope","640x360_synchronous_synthetic_fixture_not_game_or_4k"},
                  {"frame_count",frames.size()},{"timed_frame_count",0},{"gpu_work_ms",json::object()}};
    std::vector<std::string> names;
    for(const auto name:GpuTimestamps::Segments) names.emplace_back(name);
    names.emplace_back("sum");
    for(const auto &name:names)
    {
        std::vector<double> samples;
        for(const auto &frame:frames)
            if(frame.value("gpu_timing_available",false) && frame.at("gpu_work_ms").at(name).is_number())
                samples.push_back(frame.at("gpu_work_ms").at(name).get<double>());
        if(samples.size()!=frames.size() || samples.empty())
        {
            summary["gpu_work_ms"][name]={{"median",nullptr},{"max",nullptr}};
            continue;
        }
        std::sort(samples.begin(),samples.end());
        const size_t middle=samples.size()/2;
        const double median=samples.size()%2 ? samples[middle] : (samples[middle-1]+samples[middle])*0.5;
        summary["gpu_work_ms"][name]={{"median",median},{"max",samples.back()}};
        summary["timed_frame_count"]=samples.size();
    }
    summary["available"]=!frames.empty() && summary["timed_frame_count"]==frames.size();
    return summary;
}

static void SaveBytes(const std::filesystem::path &path,const std::vector<uint8_t> &bytes,size_t count)
{
    Require(count<=bytes.size(),"Estimated interpolation readback byte count invalid");
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()),count);
    Require(bool(file),"Estimated interpolation readback write incomplete");
}

static uint32_t SceneHistory(const std::vector<uint8_t> &bytes)
{
    Require(bytes.size()>=8,"Scene change history readback incomplete");
    uint32_t result=0; std::memcpy(&result,bytes.data()+4,4); return result;
}

int main(int argc,char **argv)
{
    const std::filesystem::path root=argc>1 ? argv[1] : ".";
    const auto output=root/"results"/"EstimatedInterpolation";
    std::filesystem::create_directories(output);
    constexpr uint32_t InputFrames=8;
    constexpr int StepPixels=12;
    json report={{"author","Anokianx"},{"passed",false},{"process_bits",sizeof(void*)*8},
        {"synthetic_scene",true},{"real_game_inputs",false},{"game_started",false},{"game_frame_generation",false},
        {"motion_source","Estimated"},{"motion_input_origin","GPU_AMD_OF_bidirectional_confidence_filter"},
        {"ground_truth_motion_uploaded",false},{"motion_uv_convention","previousUV-currentUV; top-left origin; scale=(width,height)"},
        {"synthetic_static_warmup_dispatches_per_context",6},{"warmup_frames_are_real_game_frames",false},
        {"active_optical_flow_dispatches_per_input_frame",3},{"input_frames",InputFrames},
        {"generated_frames_presented",0},{"hud_composition_validated",false},{"frames",json::array()}};
    std::string stage="initialize";
    try
    {
        Require(sizeof(void*)==4,"Estimated interpolation fixture requires x86");
        Gpu gpu; gpu.Initialize(); gpu.images.reserve(24);
        GpuTimestamps timestamps(gpu); report["gpu_timing"]=timestamps.Metadata();
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
        auto &upload=gpu.CreateBuffer(); auto &readback=gpu.CreateBuffer();
        const std::array<ImageBinding,10> bindings={{{views.Create(current),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {views.Create(previous),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{views.Create(newDepth),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {views.Create(oldDepth),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{views.Create(forward),VK_IMAGE_LAYOUT_GENERAL},
            {views.Create(reverse),VK_IMAGE_LAYOUT_GENERAL},{views.Create(newMask),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {views.Create(oldMask),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{views.Create(forwardScd),VK_IMAGE_LAYOUT_GENERAL},
            {views.Create(reverseScd),VK_IMAGE_LAYOUT_GENERAL}}};
        const auto motionView=views.Create(motion),confidenceView=views.Create(confidence);
        BidirectionalFlowMotion filter;
        Require(filter.Initialize(gpu.device,LoadCode(root/"FilterFlow.spv")),"Estimated motion filter initialization failed");
        gpu.Begin();
        for(auto image:{&forward,&reverse,&forwardScd,&reverseScd,&motion,&confidence,&reconstructed,&dilatedDepth,&dilatedMotion,&generated})
            gpu.Barrier(*image,VK_IMAGE_LAYOUT_GENERAL);
        gpu.Submit();
        stage="synthetic_static_warmup";
        auto preceding=MakeScene(0,0);
        gpu.Upload(previous,upload,preceding.color.data(),preceding.color.size());
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
            const auto incoming=MakeScene(int(frame)*StepPixels,0);
            gpu.Upload(previous,upload,preceding.color.data(),preceding.color.size());
            gpu.Upload(current,upload,incoming.color.data(),incoming.color.size());
            gpu.Upload(oldDepth,upload,preceding.depth.data(),preceding.depth.size()*sizeof(float));
            gpu.Upload(newDepth,upload,incoming.depth.data(),incoming.depth.size()*sizeof(float));
            gpu.Upload(oldMask,upload,preceding.mask.data(),preceding.mask.size()*sizeof(float));
            gpu.Upload(newMask,upload,incoming.mask.data(),incoming.mask.size()*sizeof(float));
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
            gpu.Begin(); timestamps.Begin(gpu.command,8); gpu.Barrier(motion,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            FfxFrameInterpolationPrepareDescription prepare{}; prepare.commandList=ffxGetCommandListVK(gpu.command);
            prepare.renderSize={Width,Height}; prepare.motionVectorScale={float(Width),float(Height)};
            prepare.frameTimeDelta=1000.0f/60; prepare.cameraNear=0.1f; prepare.cameraFar=100; prepare.cameraFovAngleVertical=1;
            prepare.viewSpaceToMetersFactor=1; prepare.depth=gpu.Resource(newDepth,L"Synthetic current scene depth");
            prepare.motionVectors=gpu.Resource(motion,L"GPU estimated current to previous UV motion"); prepare.frameID=frame;
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
            const double before=Centroid(preceding.color),after=Centroid(incoming.color),intermediate=Centroid(middle);
            const size_t differentPrevious=DifferentPixels(middle,preceding.color),differentCurrent=DifferentPixels(middle,incoming.color);
            const double expected=(before+after)*0.5,centroidError=std::abs(intermediate-expected);
            const bool midpoint=frame && intermediate>before+1 && intermediate<after-1 && centroidError<1.0 && differentPrevious>500 && differentCurrent>500;
            if(frame) { Require(midpoint,"Estimated MV generated frame failed actual midpoint position and distinct-color checks"); ++verified; }
            const auto directory=output/("frame_"+std::to_string(frame)); std::filesystem::create_directories(directory);
            SaveImage(directory/"previous.ppm",preceding.color); SaveImage(directory/"current.ppm",incoming.color); SaveImage(directory/"generated.ppm",middle);
            SaveBytes(directory/"estimated_motion.rg16f",estimated,size_t(Width)*Height*4);
            SaveBytes(directory/"confidence.r32f",quality,size_t(Width)*Height*4);
            SaveBytes(directory/"forward_flow.rg16i",rawForward,size_t(forward.description.width)*forward.description.height*4);
            SaveBytes(directory/"reverse_flow.rg16i",rawReverse,size_t(reverse.description.width)*reverse.description.height*4);
            const double wallMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            json item={{"frame_id",frame},{"reset",frame==0},{"optical_flow_dispatches",3},{"forward_context_frame_index",forwardCalls-1},
                {"reverse_context_frame_index",reverseCalls-1},{"forward_scd_history",forwardHistory},{"reverse_scd_history",reverseHistory},
                {"coverage",coverage},{"accepted_object_pixels",accepted},{"tested_object_pixels",tested},{"mean_motion_endpoint_error_pixels",meanError},
                {"previous_centroid_x",before},{"current_centroid_x",after},{"generated_centroid_x",intermediate},{"expected_centroid_x",expected},
                {"midpoint_centroid_error_pixels",centroidError},{"pixels_different_from_previous",differentPrevious},{"pixels_different_from_current",differentCurrent},
                {"midpoint_verified",midpoint},{"synchronous_fixture_wall_ms_including_upload_readback_and_file_io",wallMilliseconds},
                {"gpu_timing_available",gpuTiming["available"]},{"gpu_work_ms",gpuTiming["gpu_work_ms"]},
                {"gpu_timestamp_ticks",gpuTiming["raw_ticks"]},{"gpu_timing_unavailable_reason",gpuTiming["unavailable_reason"]},
                {"readback_directory",directory.string()}};
            report["frames"].push_back(item); std::cout<<item.dump()<<std::endl; preceding=incoming;
        }
        report["official_interpolation_dispatches"]=InputFrames; report["verified_estimated_motion_midpoints"]=verified;
        report["forward_of_dispatches_total"]=forwardCalls; report["reverse_of_dispatches_total"]=reverseCalls;
        report["all_of_dispatches_including_synthetic_warmup"]=forwardCalls+reverseCalls;
        report["gpu_timing_summary"]=TimingSummary(report["frames"]);
        stage="independent_descriptor_sets_and_pipeline_lifetime";
        auto &secondMotion=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R16G16_FLOAT));
        auto &secondConfidence=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R32_FLOAT));
        const auto secondMotionView=views.Create(secondMotion),secondConfidenceView=views.Create(secondConfidence);
        Parameters firstParameters; firstParameters.width=Width; firstParameters.height=Height; firstParameters.historyValid=1;
        firstParameters.forwardHistoryFrames=forwardCalls-1; firstParameters.reverseHistoryFrames=reverseCalls-1;
        firstParameters.photometricLimit=0.06f;
        auto secondParameters=firstParameters; secondParameters.historyValid=0;
        gpu.Begin();
        const VkImageSubresourceRange allPixels{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        for(auto image:{&motion,&confidence,&secondMotion,&secondConfidence})
        {
            gpu.Barrier(*image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue sentinel{}; sentinel.float32[0]=-1; sentinel.float32[1]=-1;
            vkCmdClearColorImage(gpu.command,image->handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&sentinel,1,&allPixels);
            gpu.Barrier(*image,VK_IMAGE_LAYOUT_GENERAL);
        }
        auto firstLifetime=filter.Record(gpu.command,bindings,motionView,confidenceView,firstParameters);
        auto secondLifetime=filter.Record(gpu.command,bindings,secondMotionView,secondConfidenceView,secondParameters);
        Require(bool(firstLifetime)&&bool(secondLifetime),"Independent descriptor dispatches must both retain lifetimes");
        Require(firstLifetime->set!=secondLifetime->set && firstLifetime->pool!=secondLifetime->pool,
                "Each recorded dispatch requires its own descriptor pool and set");
        Require(firstLifetime->state==secondLifetime->state,"Independent dispatches share immutable pipeline state");
        const std::weak_ptr<const asteria_estimated_motion::PipelineState> weakPipeline=firstLifetime->state;
        filter.Destroy();
        Require(!weakPipeline.expired(),"Recorded dispatch tokens retain pipeline after filter owner destruction before submit");
        gpu.Submit();
        const auto firstQuality=gpu.Readback(confidence,readback),secondQuality=gpu.Readback(secondConfidence,readback),secondValues=gpu.Readback(secondMotion,readback);
        uint32_t retainedValidPixels=0;
        for(uint32_t y=0;y<Height;++y)
            for(uint32_t x=0;x<Width;++x)
            {
                const auto pixel=size_t(y)*Width+x; const float quality=ReadFloat(firstQuality,pixel);
                Require(std::isfinite(quality)&&quality>=0&&quality<=1,"First independent dispatch must overwrite all sentinel confidence pixels");
                Require(ReadFloat(secondQuality,pixel)==0,"Second independent reset dispatch must clear confidence");
                const auto value=ReadMotion(secondValues,pixel);
                Require(value[0]==0&&value[1]==0,"Second independent reset dispatch must clear motion");
                if(y>=128&&y<224&&x>=240+(InputFrames-1)*StepPixels&&x<312+(InputFrames-1)*StepPixels)
                    retainedValidPixels+=quality>=firstParameters.minimumConfidence;
            }
        Require(retainedValidPixels==6912,"First dispatch keeps its valid bindings despite recording second reset dispatch");
        SaveBytes(output/"independent_first_confidence.r32f",firstQuality,size_t(Width)*Height*4);
        SaveBytes(output/"independent_reset_confidence.r32f",secondQuality,size_t(Width)*Height*4);
        SaveBytes(output/"independent_reset_motion.rg16f",secondValues,size_t(Width)*Height*4);
        firstLifetime.reset(); Require(!weakPipeline.expired(),"Second GPU token retains shared pipeline after first completed token release");
        secondLifetime.reset(); Require(weakPipeline.expired(),"Last completed dispatch token releases destroyed pipeline owner");
        report["independent_descriptor_gpu_case"]={{"passed",true},{"dispatches_in_same_command_buffer",2},
            {"distinct_descriptor_pools_and_sets",true},{"different_outputs_and_history_flags",true},
            {"pipeline_owner_destroyed_before_submit",true},{"pipeline_retained_by_tokens_until_gpu_completion",true},
            {"first_dispatch_valid_object_pixels",retainedValidPixels},{"second_dispatch_all_motion_and_confidence_zero",true},
            {"sentinel_outputs_overwritten_by_actual_gpu",true},{"last_token_released_pipeline",true}};
        report["motion_filter_dispatches_including_lifetime_case"]=InputFrames+2;
        report["passed"]=verified==InputFrames-1; report["synthetic_estimated_motion_gpu_interpolation_verified"]=report["passed"];
        stage="complete";
    }
    catch(const std::exception &error) { report["error"]=error.what(); std::cerr<<stage<<": "<<error.what()<<std::endl; }
    report["stage"]=stage;
    std::ofstream file(output/"estimated_interpolation.json"); file<<report.dump(2)<<'\n';
    return file && report["passed"].get<bool>()?0:1;
}
