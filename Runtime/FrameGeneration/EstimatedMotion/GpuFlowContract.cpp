#define ASTERIA_INTERPOLATION_PROBE_LIBRARY
#include "../Port/InterpolationProbe.cpp"
#include "BidirectionalFlowMotion.h"
#include <array>
#include <limits>

using asteria_estimated_motion::BidirectionalFlowMotion;
using asteria_estimated_motion::ImageBinding;
using asteria_estimated_motion::Parameters;

static void Require(bool condition,const char *message)
{
    if(!condition) throw std::runtime_error(message);
}

static std::vector<uint32_t> LoadCode(const std::filesystem::path &path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) throw std::runtime_error("Cannot read estimated motion shader");
    const auto size=file.tellg();
    Require(size>0 && size%4==0,"SPIR-V size invalid");
    std::vector<uint32_t> code(size_t(size)/4);
    file.seekg(0); file.read(reinterpret_cast<char *>(code.data()),size);
    Require(bool(file),"Shader read incomplete");
    return code;
}

struct Views
{
    VkDevice device;
    std::vector<VkImageView> handles;
    ~Views() { for(auto view:handles) vkDestroyImageView(device,view,nullptr); }
    VkImageView Create(const Image &image)
    {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image=image.handle; info.viewType=VK_IMAGE_VIEW_TYPE_2D;
        info.format=NativeFormat(image.description.format);
        info.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView view=VK_NULL_HANDLE;
        Check(vkCreateImageView(device,&info,nullptr,&view),"vkCreateImageView");
        handles.push_back(view); return view;
    }
};

struct FlowContexts
{
    VkDevice device=VK_NULL_HANDLE;
    std::unique_ptr<FfxOpticalflowContext> forward=std::make_unique<FfxOpticalflowContext>();
    std::unique_ptr<FfxOpticalflowContext> reverse=std::make_unique<FfxOpticalflowContext>();
    bool forwardCreated=false, reverseCreated=false;
    ~FlowContexts()
    {
        if(device) vkDeviceWaitIdle(device);
        if(forwardCreated) ffxOpticalflowContextDestroy(forward.get());
        if(reverseCreated) ffxOpticalflowContextDestroy(reverse.get());
    }
};

struct SceneInput
{
    std::vector<uint8_t> color;
    std::vector<float> depth,mask;
};

static uint32_t Hash(uint32_t x,uint32_t y)
{
    uint32_t h=x*0x9e3779b9u+y*0x85ebca6bu+17;
    h^=h>>16; h*=0x7feb352du; h^=h>>15; return h;
}

static SceneInput MakeScene(int shiftX,int shiftY,bool hud=false)
{
    SceneInput scene;
    scene.color.resize(Width*Height*4); scene.depth.assign(Width*Height,0.8f); scene.mask.assign(Width*Height,0);
    for(uint32_t y=0;y<Height;++y)
        for(uint32_t x=0;x<Width;++x)
        {
            const auto index=size_t(y)*Width+x;
            const int localX=int(x)-220-shiftX,localY=int(y)-108-shiftY;
            const bool object=localX>=0 && localX<112 && localY>=0 && localY<136;
            const uint32_t noise=Hash(object ? uint32_t(localX) : x,object ? uint32_t(localY) : y);
            scene.color[index*4]=object ? uint8_t(180+noise%70) : uint8_t(15+noise%50);
            scene.color[index*4+1]=object ? uint8_t(70+(noise>>8)%70) : uint8_t(25+(noise>>8)%50);
            scene.color[index*4+2]=object ? uint8_t(15+(noise>>16)%40) : uint8_t(60+(noise>>16)%50);
            scene.color[index*4+3]=255;
            if(object) scene.depth[index]=0.4f;
            if(hud && x>=260 && x<288 && y>=150 && y<172) scene.mask[index]=1;
        }
    return scene;
}

static float FromHalf(uint16_t half)
{
    const int exponent=(half>>10)&31;
    const float fraction=float(half&1023)/1024;
    float value=exponent==0 ? std::ldexp(fraction,-14) : exponent==31 ?
        std::numeric_limits<float>::infinity() : std::ldexp(1+fraction,exponent-15);
    return half&0x8000 ? -value : value;
}

static float ReadFloat(const std::vector<uint8_t> &bytes,size_t pixel)
{
    float result; std::memcpy(&result,bytes.data()+pixel*4,4); return result;
}

static std::array<float,2> ReadMotion(const std::vector<uint8_t> &bytes,size_t pixel)
{
    uint16_t result[2]; std::memcpy(result,bytes.data()+pixel*4,4);
    return {FromHalf(result[0])*Width,FromHalf(result[1])*Height};
}

static void DispatchFlow(Gpu &gpu,FfxOpticalflowContext *context,Image &color,Image &vectors,Image &scd,bool reset)
{
    gpu.Begin();
    FfxOpticalflowDispatchDescription desc{};
    desc.commandList=ffxGetCommandListVK(gpu.command);
    desc.color=gpu.Resource(color,L"Motion estimation color");
    desc.opticalFlowVector=gpu.Resource(vectors,L"Bidirectional optical flow",FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    desc.opticalFlowSCD=gpu.Resource(scd,L"Optical flow scene change",FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    desc.reset=reset; desc.backbufferTransferFunction=FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    desc.minMaxLuminance={0,1};
    CheckFfx(ffxOpticalflowContextDispatch(context,&desc),"ffxOpticalflowContextDispatch");
    gpu.Submit();
}

#ifndef ASTERIA_FLOW_PROBE_LIBRARY
int main(int argc,char **argv)
{
    const std::filesystem::path root=argc>1 ? argv[1] : ".";
    std::filesystem::create_directories(root/"results");
    json report={{"author","Anokianx"},{"passed",false},{"process_bits",sizeof(void *)*8},
        {"motion_source","Estimated"},{"synthetic_scene",true},{"optical_flow_vectors_manufactured",false},
        {"real_game_inputs",false},{"game_frame_generation",false},{"game_started",false}};
    try
    {
        Gpu gpu; gpu.Initialize(); gpu.images.reserve(24);
        report["gpu"]=gpu.properties.deviceName;
        VkDeviceContext deviceContext{gpu.device,gpu.physical,vkGetDeviceProcAddr};
        std::vector<uint8_t> scratch(ffxGetScratchMemorySizeVK(gpu.physical,2));
        FfxInterface backend{};
        CheckFfx(ffxGetInterfaceVK(&backend,ffxGetDeviceVK(&deviceContext),scratch.data(),scratch.size(),2),"ffxGetInterfaceVK");
        Views views{gpu.device};
        FlowContexts contexts; contexts.device=gpu.device;
        FfxOpticalflowContextDescription of{}; of.resolution={Width,Height}; of.backendInterface=backend;
        CheckFfx(ffxOpticalflowContextCreate(contexts.forward.get(),&of),"create forward optical flow"); contexts.forwardCreated=true;
        CheckFfx(ffxOpticalflowContextCreate(contexts.reverse.get(),&of),"create reverse optical flow"); contexts.reverseCreated=true;
        FfxOpticalflowSharedResourceDescriptions shared{};
        CheckFfx(ffxOpticalflowGetSharedResourceDescriptions(contexts.forward.get(),&shared),"flow descriptions");
        auto &previous=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R8G8B8A8_UNORM));
        auto &current=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R8G8B8A8_UNORM));
        auto &oldDepth=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R32_FLOAT));
        auto &newDepth=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R32_FLOAT));
        auto &oldMask=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R32_FLOAT));
        auto &newMask=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R32_FLOAT));
        auto &forward=gpu.CreateImage(shared.opticalFlowVector.resourceDescription);
        auto &reverse=gpu.CreateImage(shared.opticalFlowVector.resourceDescription);
        auto &forwardScd=gpu.CreateImage(shared.opticalFlowSCD.resourceDescription);
        auto &reverseScd=gpu.CreateImage(shared.opticalFlowSCD.resourceDescription);
        auto &motion=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R16G16_FLOAT));
        auto &confidence=gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R32_FLOAT));
        auto &upload=gpu.CreateBuffer(); auto &readback=gpu.CreateBuffer();
        const std::array<ImageBinding,10> bindings={{{views.Create(current),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {views.Create(previous),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{views.Create(newDepth),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {views.Create(oldDepth),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{views.Create(forward),VK_IMAGE_LAYOUT_GENERAL},
            {views.Create(reverse),VK_IMAGE_LAYOUT_GENERAL},{views.Create(newMask),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {views.Create(oldMask),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{views.Create(forwardScd),VK_IMAGE_LAYOUT_GENERAL},
            {views.Create(reverseScd),VK_IMAGE_LAYOUT_GENERAL}}};
        const auto motionView=views.Create(motion),confidenceView=views.Create(confidence);
        BidirectionalFlowMotion filter;
        Require(filter.Initialize(gpu.device,LoadCode(root/"FilterFlow.spv")),"Cannot initialize flow filter");
        gpu.Begin();
        for(auto image:{&forward,&reverse,&forwardScd,&reverseScd,&motion,&confidence}) gpu.Barrier(*image,VK_IMAGE_LAYOUT_GENERAL);
        gpu.Submit();
        report["cases"]=json::array();
        const std::array<std::array<int,2>,4> shifts={{{0,0},{12,0},{0,8},{-8,6}}};
        uint32_t verified=0;
        uint32_t rejectionCases=0;
        for(const auto &shift:shifts)
        {
            const auto old=MakeScene(0,0),now=MakeScene(shift[0],shift[1]);
            gpu.Upload(previous,upload,old.color.data(),old.color.size()); gpu.Upload(current,upload,now.color.data(),now.color.size());
            gpu.Upload(oldDepth,upload,old.depth.data(),old.depth.size()*4); gpu.Upload(newDepth,upload,now.depth.data(),now.depth.size()*4);
            gpu.Upload(oldMask,upload,old.mask.data(),old.mask.size()*4); gpu.Upload(newMask,upload,now.mask.data(),now.mask.size()*4);
            DispatchFlow(gpu,contexts.forward.get(),previous,forward,forwardScd,true);
            for(uint32_t warmup=1;warmup<6;++warmup) DispatchFlow(gpu,contexts.forward.get(),previous,forward,forwardScd,false);
            DispatchFlow(gpu,contexts.forward.get(),current,forward,forwardScd,false);
            DispatchFlow(gpu,contexts.reverse.get(),current,reverse,reverseScd,true);
            for(uint32_t warmup=1;warmup<6;++warmup) DispatchFlow(gpu,contexts.reverse.get(),current,reverse,reverseScd,false);
            DispatchFlow(gpu,contexts.reverse.get(),previous,reverse,reverseScd,false);
            const auto rawForward=gpu.Readback(forward,readback),rawReverse=gpu.Readback(reverse,readback);
            json rawHistogram=json::object();
            for(int by=16;by<28;++by)
                for(int bx=30;bx<39;++bx)
                {
                    int16_t v[2]; std::memcpy(v,rawForward.data()+size_t(by*forward.description.width+bx)*4,4);
                    const auto key=std::to_string(v[0])+","+std::to_string(v[1]);
                    rawHistogram[key]=rawHistogram.value(key,0)+1;
                }
            std::cout<<"Raw forward interior: "<<rawHistogram.dump()<<std::endl;
            std::ofstream(root/"results/raw_forward.rg16i",std::ios::binary).write(reinterpret_cast<const char *>(rawForward.data()),size_t(forward.description.width)*forward.description.height*4);
            std::ofstream(root/"results/raw_reverse.rg16i",std::ios::binary).write(reinterpret_cast<const char *>(rawReverse.data()),size_t(reverse.description.width)*reverse.description.height*4);
            Parameters parameters; parameters.width=Width; parameters.height=Height; parameters.historyValid=1;
            parameters.forwardHistoryFrames=parameters.reverseHistoryFrames=6;
            parameters.photometricLimit=0.06f;
            gpu.Begin();
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(gpu.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            auto dispatchLifetime=filter.Record(gpu.command,bindings,motionView,confidenceView,parameters);
            Require(bool(dispatchLifetime),"Flow filter record failed"); gpu.Submit(); dispatchLifetime.reset();
            const auto values=gpu.Readback(motion,readback),quality=gpu.Readback(confidence,readback);
            size_t accepted=0,total=0; double error=0;
            for(int y=128+shift[1];y<224+shift[1];++y)
                for(int x=240+shift[0];x<312+shift[0];++x)
                {
                    ++total; const auto index=size_t(y)*Width+x;
                    if(ReadFloat(quality,index)>=parameters.minimumConfidence)
                    {
                        const auto value=ReadMotion(values,index); ++accepted;
                        error+=std::hypot(value[0]+shift[0],value[1]+shift[1]);
                    }
                }
            const double coverage=double(accepted)/total,meanError=accepted ? error/accepted : 1e9;
            const bool passed=coverage>0.30 && meanError<1.25;
            const auto scd=gpu.Readback(forwardScd,readback); uint32_t bits; std::memcpy(&bits,scd.data()+4,4);
            json item={{"object_delta_pixels",shift},{"expected_motion_pixels",{-shift[0],-shift[1]}},
                {"accepted_pixels",accepted},{"tested_pixels",total},{"coverage",coverage},{"mean_endpoint_error_pixels",meanError},
                {"forward_scd_history",bits},{"raw_forward_histogram",rawHistogram},{"passed",passed}};
            report["cases"].push_back(item); std::cout<<item.dump()<<std::endl;
            Require(passed,"Estimated flow direction/accuracy/coverage failed"); ++verified;
            std::ofstream(root/"results"/("motion_"+std::to_string(verified)+".rg16f"),std::ios::binary).write(reinterpret_cast<const char *>(values.data()),values.size());
            std::ofstream(root/"results"/("confidence_"+std::to_string(verified)+".r32f"),std::ios::binary).write(reinterpret_cast<const char *>(quality.data()),quality.size());
            auto readFiltered=[&](const Parameters &settings) {
                gpu.Begin(); gpu.Barrier(motion,VK_IMAGE_LAYOUT_GENERAL); gpu.Barrier(confidence,VK_IMAGE_LAYOUT_GENERAL);
                auto lifetime=filter.Record(gpu.command,bindings,motionView,confidenceView,settings);
                Require(bool(lifetime),"Rejection filter record failed");
                gpu.Submit(); lifetime.reset(); return gpu.Readback(confidence,readback);
            };
            auto rejectAll=[&](const Parameters &settings,const char *message) {
                const auto rejected=readFiltered(settings);
                for(size_t i=0;i<Width*Height;++i) Require(ReadFloat(rejected,i)==0,message);
                ++rejectionCases;
            };
            auto invalid=parameters; invalid.historyValid=0;
            rejectAll(invalid,"History reset left valid motion");
            invalid=parameters; invalid.forwardHistoryFrames=5;
            rejectAll(invalid,"Forward warmup zero vectors accepted");
            invalid=parameters; invalid.reverseHistoryFrames=5;
            rejectAll(invalid,"Reverse warmup zero vectors accepted");
            invalid=parameters; invalid.width=Width+1;
            rejectAll(invalid,"Inconsistent input/output dimensions accepted");
            const auto hud=MakeScene(shift[0],shift[1],true);
            gpu.Upload(newMask,upload,hud.mask.data(),hud.mask.size()*4);
            const auto masked=readFiltered(parameters);
            for(uint32_t y=150;y<172;++y)
                for(uint32_t x=260;x<288;++x) Require(ReadFloat(masked,size_t(y)*Width+x)==0,"Current HUD pixels accepted");
            ++rejectionCases;
            gpu.Upload(newMask,upload,now.mask.data(),now.mask.size()*4);
            const auto oldHud=MakeScene(0,0,true);
            gpu.Upload(oldMask,upload,oldHud.mask.data(),oldHud.mask.size()*4);
            const auto previousMasked=readFiltered(parameters);
            for(int y=152+shift[1];y<170+shift[1];++y)
                for(int x=262+shift[0];x<286+shift[0];++x)
                    Require(ReadFloat(previousMasked,size_t(y)*Width+x)==0,"Previous HUD reprojection accepted");
            ++rejectionCases;
            gpu.Upload(oldMask,upload,old.mask.data(),old.mask.size()*4);
            const std::vector<float> invalidMask(Width*Height,std::numeric_limits<float>::quiet_NaN());
            gpu.Upload(newMask,upload,invalidMask.data(),invalidMask.size()*4);
            rejectAll(parameters,"Nonfinite current HUD mask accepted");
            gpu.Upload(newMask,upload,now.mask.data(),now.mask.size()*4);
            gpu.Upload(oldMask,upload,invalidMask.data(),invalidMask.size()*4);
            rejectAll(parameters,"Nonfinite previous HUD mask accepted");
            gpu.Upload(oldMask,upload,old.mask.data(),old.mask.size()*4);
            for(float bad:{0.0f,1.0f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})
            {
                const std::vector<float> invalidDepth(Width*Height,bad);
                gpu.Upload(newDepth,upload,invalidDepth.data(),invalidDepth.size()*4);
                rejectAll(parameters,"Invalid or clear depth accepted");
            }
            gpu.Upload(newDepth,upload,now.depth.data(),now.depth.size()*4);
            const std::vector<float> occluder(Width*Height,0.99f);
            gpu.Upload(oldDepth,upload,occluder.data(),occluder.size()*4);
            rejectAll(parameters,"Inconsistent previous depth accepted");
            gpu.Upload(oldDepth,upload,old.depth.data(),old.depth.size()*4);
            std::vector<uint8_t> flat(Width*Height*4,128);
            gpu.Upload(current,upload,flat.data(),flat.size());
            rejectAll(parameters,"Ambiguous textureless image accepted");
            gpu.Upload(current,upload,now.color.data(),now.color.size());
            gpu.Begin(); gpu.Barrier(confidence,VK_IMAGE_LAYOUT_GENERAL); gpu.Barrier(motion,VK_IMAGE_LAYOUT_GENERAL); gpu.Submit();
        }
        std::vector<uint8_t> cutColor(Width*Height*4,255);
        gpu.Upload(current,upload,cutColor.data(),cutColor.size());
        DispatchFlow(gpu,contexts.forward.get(),current,forward,forwardScd,false);
        const auto cutScd=gpu.Readback(forwardScd,readback);
        uint32_t cutHistory; std::memcpy(&cutHistory,cutScd.data()+4,4);
        Require((cutHistory&15)!=0,"Actual optical-flow scene cut was not detected");
        Parameters cutParameters; cutParameters.width=Width; cutParameters.height=Height; cutParameters.historyValid=1;
        cutParameters.forwardHistoryFrames=7; cutParameters.reverseHistoryFrames=6;
        gpu.Begin(); gpu.Barrier(motion,VK_IMAGE_LAYOUT_GENERAL); gpu.Barrier(confidence,VK_IMAGE_LAYOUT_GENERAL);
        auto cutLifetime=filter.Record(gpu.command,bindings,motionView,confidenceView,cutParameters);
        Require(bool(cutLifetime),"Scene cut record failed"); gpu.Submit(); cutLifetime.reset();
        const auto cutConfidence=gpu.Readback(confidence,readback);
        for(size_t i=0;i<Width*Height;++i) Require(ReadFloat(cutConfidence,i)==0,"Scene cut retained valid estimates");
        uint32_t recoveryDispatches=0;
        for(uint32_t attempt=0;attempt<10;++attempt)
        {
            DispatchFlow(gpu,contexts.forward.get(),current,forward,forwardScd,false);
            const auto scd=gpu.Readback(forwardScd,readback); uint32_t bits; std::memcpy(&bits,scd.data()+4,4);
            ++recoveryDispatches;
            if((bits&15)==0) break;
        }
        Require(recoveryDispatches<10,"Scene cut history did not settle for a stable fixture");
        report["verified_motion_cases"]=verified;
        report["history_reset_rejection"]=true;
        report["sdk_warmup_static_fixture_frames_per_direction"]=6;
        report["rejection_cases"]=rejectionCases;
        report["warmup_hud_clear_nonfinite_occlusion_textureless_rejection"]=true;
        report["actual_scene_cut_history"]=cutHistory;
        report["scene_cut_rejection"]=true;
        report["scene_cut_static_fixture_recovery_dispatches"]=recoveryDispatches;
        report["uv_convention"]="previousUV-currentUV; top-left image origin; scale=(width,height)";
        report["shader_outputs"]={{"motion","R16G16_FLOAT"},{"confidence","R32_FLOAT"},{"width",Width},{"height",Height}};
        report["passed"]=verified==shifts.size();
    }
    catch(const std::exception &error) { report["error"]=error.what(); std::cerr<<error.what()<<std::endl; }
    std::ofstream file(root/"results/flow_contract.json"); file<<report.dump(2)<<'\n';
    return file && report["passed"].get<bool>() ? 0 : 1;
}
#endif
