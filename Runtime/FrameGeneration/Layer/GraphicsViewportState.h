#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <vulkan/vulkan.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <array>
#include <algorithm>

namespace asteria::graphics_viewport
{
enum class Mode : uint32_t { Unknown, Static, LegacyDynamic, DynamicWithCount };
enum class Source : uint32_t { None, Legacy, CoreWithCount, ExtWithCount };
enum class Status : uint32_t { Ok, UnknownDevice, UnknownPipeline, UnknownCommand, NotRecording, SecondaryUnsupported, UnsupportedState, Retired, MultiViewport, InvalidViewport, ModeMismatch, UnsupportedCommand, CapacityExceeded };
enum class Rejection : uint32_t { None, NullPipeline, PipelineLibraryUnsupported, UnsupportedCreateFlags, UnknownPipelineChain, IncompletePipeline, UnsupportedViewportState, ConflictingViewportModes, UnsupportedScissorState, MultiViewportUnsupported, InvalidStaticViewport, CapacityExceeded, UnknownPipelineBinding };
enum class PoisonEvent : uint32_t { None, UnknownPipelineBinding, LibraryBinding, RejectedPipelineBinding, UnsupportedCommand, SetWhileStatic, InvalidViewportSetting };
struct PipelineReference
{
    uint64_t pipeline=0,generation=0;
    uint32_t library_flags=0;
    Rejection rejection=Rejection::None;
    bool observed=false,valid=false,is_library=false;
};
struct PipelineObservation
{
    uint64_t pipeline=0,generation=0,epoch=0;
    Mode mode=Mode::Unknown;
    Rejection rejection=Rejection::None;
    const char *detail="none";
    uint32_t detail_index=UINT32_MAX,library_flags=0,dependency_count=0;
    std::array<PipelineReference,4> dependencies{};
    bool valid=false,is_library=false,epoch_stable=false,epoch_guard_required=false,dependencies_truncated=false;
};
struct PoisonObservation
{
    uint64_t pipeline=0,generation=0,sequence=0;
    PoisonEvent event=PoisonEvent::None;
    Rejection rejection=Rejection::None;
    uint32_t detail_index=UINT32_MAX;
};
struct CommandDiagnostic
{
    uint64_t pipeline=0,generation=0;
    PoisonObservation first{},last{};
};

struct Result
{
    Status status = Status::UnknownCommand;
    Rejection rejection = Rejection::None;
    Mode mode = Mode::Unknown;
    Source source = Source::None;
    bool contract_supported = false;
    bool observed_only = true;
    bool setting_after_required_boundary = false;
    uint32_t viewport_count = 0;
    uint64_t pipeline = 0;
    uint64_t pipeline_generation = 0;
    uint64_t command_generation = 0;
    uint64_t recording_generation = 0;
    uint64_t bind_sequence = 0;
    uint64_t setting_sequence = 0;
    float viewport[6]{};
};

class Tracker
{
    struct Pipeline { uint64_t key=0,generation=0,render_pass=0; Mode mode=Mode::Unknown; Rejection rejection=Rejection::None; uint32_t count=0,library_flags=0,render_subpass=0,render_view_mask=0; float viewport[6]{}; bool valid=false,is_library=false,viewport_contract=false,render_relevant=false,view_mask_relevant=false,uses_identifier=false; PipelineObservation observation{}; };
    struct Command { uint64_t key=0,generation=0,recording=0,sequence=0,recording_start=0,last_static_bind=0,bind_sequence=0,pipeline=0,pipeline_generation=0,setting_sequence=0; Mode mode=Mode::Unknown; Source source=Source::None; Rejection rejection=Rejection::None; uint32_t count=0; float viewport[6]{}; bool recording_live=false,secondary=false,unsupported=false,set_seen=false; PoisonObservation first_poison{},last_poison{}; };
    struct Device
    {
        uint64_t key=0,generation=0,sequence=0,pipeline_epoch=0;
        bool live=false,poisoned=false;
        std::unordered_map<uint64_t,Pipeline> pipelines;
        std::unordered_map<uint64_t,uint64_t> pipeline_generations;
        std::unordered_map<uint64_t,Command> commands;
        std::unordered_map<uint64_t,uint64_t> command_generations;
        std::unordered_map<uint64_t,uint64_t> recording_generations;
    };
    size_t device_capacity_,object_capacity_;
    std::unordered_map<uint64_t,Device> devices_;
    std::mutex mutex_;

    template<class T> static uint64_t Key(T v) { if constexpr (std::is_pointer_v<T>) return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(v)); else return static_cast<uint64_t>(v); }
    Device* FindDevice(uint64_t key) { auto i=devices_.find(key);return i!=devices_.end()&&i->second.live?&i->second:nullptr; }
    static bool ValidViewport(const VkViewport& v)
    {
        const float values[]={v.x,v.y,v.width,v.height,v.minDepth,v.maxDepth};
        for(float x:values)if(!std::isfinite(x))return false;
        return v.width>0.0f&&v.height!=0.0f&&v.minDepth>=0.0f&&v.minDepth<=1.0f&&v.maxDepth>=0.0f&&v.maxDepth<=1.0f&&v.minDepth!=v.maxDepth;
    }
    static void CopyViewport(float* out,const VkViewport& v){out[0]=v.x;out[1]=v.y;out[2]=v.width;out[3]=v.height;out[4]=v.minDepth;out[5]=v.maxDepth;}
    static void CopyViewport(VkViewport& out,const float* v){out.x=v[0];out.y=v[1];out.width=v[2];out.height=v[3];out.minDepth=v[4];out.maxDepth=v[5];}
    static bool HasState(const VkPipelineDynamicStateCreateInfo* ds,VkDynamicState state)
    {
        if(!ds)return false;
        for(uint32_t i=0;i<ds->dynamicStateCount;++i)if(ds->pDynamicStates[i]==state)return true;
        return false;
    }
    struct Chain
    {
        const VkGraphicsPipelineLibraryCreateInfoEXT* subsets=nullptr;
        const VkPipelineLibraryCreateInfoKHR* libraries=nullptr;
        const VkPipelineRenderingCreateInfo* rendering=nullptr;
    };
    static Rejection ReadChain(const void* next,Chain& out)
    {
        uint32_t seen=0;
        for(uint32_t n=0;next&&n<8;++n){const auto* h=static_cast<const VkBaseInStructure*>(next);uint32_t bit=0;switch(h->sType){case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT:bit=1;out.subsets=reinterpret_cast<const VkGraphicsPipelineLibraryCreateInfoEXT*>(h);break;case VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR:bit=2;out.libraries=reinterpret_cast<const VkPipelineLibraryCreateInfoKHR*>(h);break;case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO:bit=4;out.rendering=reinterpret_cast<const VkPipelineRenderingCreateInfo*>(h);break;default:return Rejection::UnknownPipelineChain;}if(seen&bit)return Rejection::UnknownPipelineChain;seen|=bit;next=h->pNext;}
        return next?Rejection::UnknownPipelineChain:Rejection::None;
    }
    static Rejection ValidateViewportState(const VkGraphicsPipelineCreateInfo& i,Mode& mode,uint32_t& count,float* viewport)
    {
        if(!i.pViewportState)return Rejection::IncompletePipeline;
        const auto& vs=*i.pViewportState;
        if(vs.sType!=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO||vs.pNext||vs.flags)return Rejection::IncompletePipeline;
        if(i.pDynamicState&&(i.pDynamicState->sType!=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO||i.pDynamicState->pNext||i.pDynamicState->flags||(i.pDynamicState->dynamicStateCount&&!i.pDynamicState->pDynamicStates)))return Rejection::IncompletePipeline;
        const bool legacy=HasState(i.pDynamicState,VK_DYNAMIC_STATE_VIEWPORT),withCount=HasState(i.pDynamicState,VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT);
        if(legacy&&withCount)return Rejection::ConflictingViewportModes;
        const bool scissor=HasState(i.pDynamicState,VK_DYNAMIC_STATE_SCISSOR),scissorWithCount=HasState(i.pDynamicState,VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT);
        if(scissor&&scissorWithCount)return Rejection::UnsupportedScissorState;
        if(scissorWithCount){if(vs.scissorCount!=0)return Rejection::UnsupportedScissorState;}
        else if(scissor){if(vs.scissorCount!=1)return Rejection::UnsupportedScissorState;}
        else if(vs.scissorCount==0||!vs.pScissors)return Rejection::UnsupportedScissorState;
        if(withCount){if(vs.viewportCount!=0)return Rejection::UnsupportedViewportState;mode=Mode::DynamicWithCount;count=0;return Rejection::None;}
        if(vs.viewportCount!=1)return Rejection::MultiViewportUnsupported;
        if(legacy){mode=Mode::LegacyDynamic;count=1;return Rejection::None;}
        if(!vs.pViewports||!ValidViewport(vs.pViewports[0]))return Rejection::InvalidStaticViewport;
        mode=Mode::Static;count=1;CopyViewport(viewport,vs.pViewports[0]);return Rejection::None;
    }
    static Rejection ValidatePresentPayload(const VkGraphicsPipelineCreateInfo& i)
    {
        if(i.stageCount&&!i.pStages)return Rejection::IncompletePipeline;
        for(uint32_t n=0;n<i.stageCount;++n)if(!ValidShaderStage(i.pStages[n],i.flags))return Rejection::IncompletePipeline;
        if(i.pVertexInputState&&(i.pVertexInputState->sType!=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO||i.pVertexInputState->pNext||i.pVertexInputState->flags))return Rejection::IncompletePipeline;
        if(i.pInputAssemblyState&&(i.pInputAssemblyState->sType!=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO||i.pInputAssemblyState->pNext||i.pInputAssemblyState->flags))return Rejection::IncompletePipeline;
        if(i.pRasterizationState&&!ValidRasterizationState(*i.pRasterizationState))return Rejection::IncompletePipeline;
        if(i.pTessellationState&&(i.pTessellationState->sType!=VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO||i.pTessellationState->pNext||i.pTessellationState->flags))return Rejection::IncompletePipeline;
        if(i.pMultisampleState&&(i.pMultisampleState->sType!=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO||i.pMultisampleState->pNext||i.pMultisampleState->flags))return Rejection::IncompletePipeline;
        if(i.pDepthStencilState&&(i.pDepthStencilState->sType!=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO||i.pDepthStencilState->pNext||i.pDepthStencilState->flags))return Rejection::IncompletePipeline;
        if(i.pColorBlendState&&(i.pColorBlendState->sType!=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO||i.pColorBlendState->pNext||i.pColorBlendState->flags))return Rejection::IncompletePipeline;
        if(i.pDynamicState&&(i.pDynamicState->sType!=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO||i.pDynamicState->pNext||i.pDynamicState->flags||(i.pDynamicState->dynamicStateCount&&!i.pDynamicState->pDynamicStates)))return Rejection::IncompletePipeline;
        if(i.pViewportState&&(i.pViewportState->sType!=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO||i.pViewportState->pNext||i.pViewportState->flags))return Rejection::IncompletePipeline;
        return Rejection::None;
    }
    static bool ValidShaderStage(const VkPipelineShaderStageCreateInfo& stage,VkPipelineCreateFlags flags=0)
    {
        if(stage.sType!=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO||stage.flags||!stage.pName)return false;
        if(!stage.pNext)return stage.module!=VK_NULL_HANDLE;
        if(stage.module!=VK_NULL_HANDLE)return false;
        const auto* header=static_cast<const VkBaseInStructure*>(stage.pNext);
        if(header->sType==VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT)
        {
            const auto& identifier=*static_cast<const VkPipelineShaderStageModuleIdentifierCreateInfoEXT*>(stage.pNext);
            return !identifier.pNext&&identifier.identifierSize&&identifier.identifierSize<=VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT&&
                identifier.pIdentifier&&(flags&VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT)!=0;
        }
        if(header->sType!=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)return false;
        const auto& module=*static_cast<const VkShaderModuleCreateInfo*>(stage.pNext);
        if(module.pNext||module.flags||!module.pCode||module.codeSize<20||
            module.codeSize%sizeof(uint32_t)||reinterpret_cast<uintptr_t>(module.pCode)%alignof(uint32_t))return false;
        uint32_t magic=0;std::memcpy(&magic,module.pCode,sizeof(magic));return magic==0x07230203u;
    }
    static bool HasIdentifierStage(const VkGraphicsPipelineCreateInfo& info)
    {
        if(!info.pStages)return false;
        for(uint32_t index=0;index<info.stageCount;++index)
            if(info.pStages[index].pNext&&static_cast<const VkBaseInStructure*>(info.pStages[index].pNext)->sType==VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT)return true;
        return false;
    }
    static bool ValidRasterizationState(const VkPipelineRasterizationStateCreateInfo& state)
    {
        if(state.sType!=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO||state.flags)return false;
        if(!state.pNext)return true;
        if(static_cast<const VkBaseInStructure*>(state.pNext)->sType!=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT)return false;
        const auto& clip=*static_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT*>(state.pNext);
        return !clip.pNext&&!clip.flags&&clip.depthClipEnable==VK_TRUE;
    }
    static Rejection ValidateLibrary(const VkGraphicsPipelineCreateInfo& i,const Chain& chain,Pipeline& p)
    {
        if(!chain.subsets||chain.libraries||i.flags&~(VK_PIPELINE_CREATE_LIBRARY_BIT_KHR|VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT)||!(i.flags&VK_PIPELINE_CREATE_LIBRARY_BIT_KHR))return Rejection::PipelineLibraryUnsupported;
        const uint32_t f=chain.subsets->flags;constexpr uint32_t all=VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT|VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT|VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT|VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
        if(chain.subsets->sType!=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT||!f||(f&~all))return Rejection::UnsupportedCreateFlags;
        if(i.basePipelineHandle!=VK_NULL_HANDLE||i.basePipelineIndex!=-1)return Rejection::UnsupportedCreateFlags;
        if(chain.rendering&&(chain.rendering->sType!=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO||(chain.rendering->colorAttachmentCount&&!chain.rendering->pColorAttachmentFormats)))return Rejection::UnknownPipelineChain;
        if(chain.rendering&&i.renderPass!=VK_NULL_HANDLE)return Rejection::IncompletePipeline;
        auto payload=ValidatePresentPayload(i);if(payload!=Rejection::None)return payload;
        if((f&VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT)&&(!i.stageCount||!i.pStages||!i.pRasterizationState))return Rejection::IncompletePipeline;
        p.is_library=true;p.library_flags=f;p.valid=true;p.uses_identifier=HasIdentifierStage(i);
        p.render_relevant=(f&(VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT|VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT|VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT))!=0;
        if(p.render_relevant)
        {
            p.render_pass=Key(i.renderPass);p.render_subpass=i.subpass;
            p.view_mask_relevant=(f&(VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT|VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT))!=0;
            p.render_view_mask=i.renderPass==VK_NULL_HANDLE&&chain.rendering?chain.rendering->viewMask:0;
        }
        if(f&VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT){p.rejection=ValidateViewportState(i,p.mode,p.count,p.viewport);p.viewport_contract=p.rejection==Rejection::None;if(p.rejection!=Rejection::None)p.valid=false;}
        return p.valid?Rejection::None:p.rejection;
    }
    Rejection ValidateLinkedPipeline(Device& d,const VkGraphicsPipelineCreateInfo& i,const Chain& chain,Mode& mode,uint32_t& count,float* viewport,Pipeline& record)
    {
        if(!chain.libraries||chain.libraries->sType!=VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR||!chain.libraries->libraryCount||!chain.libraries->pLibraries)return Rejection::IncompletePipeline;
        if((i.flags&~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT)!=0||i.basePipelineHandle!=VK_NULL_HANDLE||i.basePipelineIndex!=-1)return Rejection::UnsupportedCreateFlags;
        uint32_t direct=0;
        if(chain.subsets)return Rejection::UnsupportedCreateFlags;
        if(chain.rendering&&(chain.rendering->sType!=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO||(chain.rendering->colorAttachmentCount&&!chain.rendering->pColorAttachmentFormats)))return Rejection::UnknownPipelineChain;
        if(chain.rendering&&i.renderPass!=VK_NULL_HANDLE)return Rejection::IncompletePipeline;
        if(i.stageCount||i.pStages||i.pVertexInputState||i.pInputAssemblyState||i.pViewportState||i.pRasterizationState||i.pTessellationState||i.pMultisampleState||i.pDepthStencilState||i.pColorBlendState||i.pDynamicState)direct=1;
        if(direct)return Rejection::IncompletePipeline;
        constexpr uint32_t all=VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT|VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT|VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT|VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
        uint32_t aggregate=0;bool haveViewport=false,identifiers=false;const Pipeline* viewportSource=nullptr;const Pipeline* renderSource=nullptr;const Pipeline* viewSource=nullptr;
        if(chain.libraries->libraryCount>object_capacity_)return Rejection::CapacityExceeded;
        for(uint32_t n=0;n<chain.libraries->libraryCount;++n)
        {
            VkPipeline h=chain.libraries->pLibraries[n];if(h==VK_NULL_HANDLE)return Rejection::IncompletePipeline;const uint64_t key=Key(h);
            for(uint32_t j=0;j<n;++j)if(Key(chain.libraries->pLibraries[j])==key)return Rejection::IncompletePipeline;
            auto it=d.pipelines.find(key);if(it==d.pipelines.end()||!it->second.is_library||!it->second.valid)return Rejection::IncompletePipeline;
            const auto& lib=it->second;if(lib.library_flags&aggregate)return Rejection::IncompletePipeline;aggregate|=lib.library_flags;identifiers|=lib.uses_identifier;
            if(lib.render_relevant)
            {
                if(renderSource&&(lib.render_pass!=renderSource->render_pass||lib.render_subpass!=renderSource->render_subpass))return Rejection::IncompletePipeline;
                renderSource=&lib;
                if(lib.view_mask_relevant)
                {
                    if(viewSource&&lib.render_pass==0&&lib.render_view_mask!=viewSource->render_view_mask)return Rejection::IncompletePipeline;
                    viewSource=&lib;
                }
            }
            if(lib.library_flags&VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT){if(haveViewport||!lib.viewport_contract)return Rejection::IncompletePipeline;haveViewport=true;viewportSource=&lib;}
        }
        if(aggregate!=all||!haveViewport||!viewportSource||!renderSource||!viewSource)return Rejection::IncompletePipeline;
        if(identifiers&&(i.flags&VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT)==0)return Rejection::UnsupportedCreateFlags;
        if(i.renderPass!=VK_NULL_HANDLE&&Key(i.renderPass)!=renderSource->render_pass)return Rejection::IncompletePipeline;
        if(chain.rendering&&(renderSource->render_pass!=0||chain.rendering->viewMask!=viewSource->render_view_mask))return Rejection::IncompletePipeline;
        record.render_relevant=true;record.view_mask_relevant=true;record.render_pass=renderSource->render_pass;
        record.render_subpass=renderSource->render_subpass;record.render_view_mask=viewSource->render_view_mask;record.uses_identifier=identifiers;
        mode=viewportSource->mode;count=viewportSource->count;std::memcpy(viewport,viewportSource->viewport,sizeof(float)*6);return Rejection::None;
    }
    Rejection ValidatePipeline(Device& d,const VkGraphicsPipelineCreateInfo& i,Mode& mode,uint32_t& count,float* viewport,Pipeline& record)
    {
        Chain chain{};auto cr=ReadChain(i.pNext,chain);if(cr!=Rejection::None)return cr;
        if(i.flags&VK_PIPELINE_CREATE_LIBRARY_BIT_KHR)return ValidateLibrary(i,chain,record);
        if(chain.libraries)return ValidateLinkedPipeline(d,i,chain,mode,count,viewport,record);
        if(chain.subsets)return Rejection::UnsupportedCreateFlags;
        const bool rendering=chain.rendering!=nullptr;
        if(rendering&&(chain.rendering->sType!=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO||(chain.rendering->colorAttachmentCount&&!chain.rendering->pColorAttachmentFormats)))return Rejection::UnknownPipelineChain;
        if((i.flags!=0&&(i.flags!=VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT||!HasIdentifierStage(i)))||i.basePipelineHandle!=VK_NULL_HANDLE||i.basePipelineIndex!=-1)return Rejection::UnsupportedCreateFlags;
        if((rendering&&i.renderPass!=VK_NULL_HANDLE)||(!rendering&&i.renderPass==VK_NULL_HANDLE)||!i.stageCount||!i.pStages||!i.pVertexInputState||!i.pInputAssemblyState||!i.pViewportState||!i.pRasterizationState||!i.pMultisampleState||!i.pColorBlendState||i.layout==VK_NULL_HANDLE)return Rejection::IncompletePipeline;
        for(uint32_t n=0;n<i.stageCount;++n)if(!ValidShaderStage(i.pStages[n],i.flags))return Rejection::IncompletePipeline;
        const auto& vi=*i.pVertexInputState;const auto& ia=*i.pInputAssemblyState;const auto& vs=*i.pViewportState;const auto& rs=*i.pRasterizationState;const auto& ms=*i.pMultisampleState;const auto& cb=*i.pColorBlendState;
        if(vi.sType!=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO||vi.pNext||vi.flags||ia.sType!=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO||ia.pNext||ia.flags||vs.sType!=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO||vs.pNext||vs.flags||!ValidRasterizationState(rs)||ms.sType!=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO||ms.pNext||ms.flags||cb.sType!=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO||cb.pNext||cb.flags)return Rejection::IncompletePipeline;
        if(i.pTessellationState&&(i.pTessellationState->sType!=VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO||i.pTessellationState->pNext||i.pTessellationState->flags))return Rejection::IncompletePipeline;
        if(i.pDepthStencilState&&(i.pDepthStencilState->sType!=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO||i.pDepthStencilState->pNext||i.pDepthStencilState->flags))return Rejection::IncompletePipeline;
        return ValidateViewportState(i,mode,count,viewport);
    }
    void Poison(Device& d){d.poisoned=true;}
    static void Bump(Device& d,Command& c){c.sequence=++d.sequence;}
    static void ObservePoison(Command& c,PoisonEvent event,Rejection reason,uint32_t index=UINT32_MAX,uint64_t sourcePipeline=0)
    {
        const PoisonObservation value{sourcePipeline?sourcePipeline:c.pipeline,sourcePipeline?0:c.pipeline_generation,c.sequence,event,reason,index};
        if(c.first_poison.event==PoisonEvent::None)c.first_poison=value;
        c.last_poison=value;c.unsupported=true;
    }
    static void CaptureReferences(Device& d,const VkGraphicsPipelineCreateInfo& info,PipelineObservation& out)
    {
        Chain chain{};if(ReadChain(info.pNext,chain)!=Rejection::None||!chain.libraries)return;
        out.dependency_count=chain.libraries->libraryCount;out.dependencies_truncated=out.dependency_count>out.dependencies.size();
        if(!chain.libraries->pLibraries)return;
        for(uint32_t n=0;n<(std::min)(out.dependency_count,uint32_t(out.dependencies.size()));++n)
        {
            auto& ref=out.dependencies[n];ref.pipeline=Key(chain.libraries->pLibraries[n]);
            const auto it=d.pipelines.find(ref.pipeline);if(it==d.pipelines.end())continue;
            const auto& p=it->second;ref.generation=p.generation;ref.library_flags=p.library_flags;ref.rejection=p.rejection;
            ref.observed=true;ref.valid=p.valid;ref.is_library=p.is_library;
        }
    }
    bool ObservePipelineCreateLocked(Device& d,uint64_t pk,VkPipeline pipeline,const VkGraphicsPipelineCreateInfo& info,bool epoch_valid)
    {
        if(pipeline==VK_NULL_HANDLE||d.poisoned)return false;
        auto gi=d.pipeline_generations.find(pk);if(gi==d.pipeline_generations.end()&&d.pipeline_generations.size()>=object_capacity_){Poison(d);return false;}
        uint64_t generation=++d.pipeline_generations[pk];Pipeline p{};p.key=pk;p.generation=generation;
        p.observation.pipeline=pk;p.observation.generation=generation;p.observation.epoch=d.pipeline_epoch;p.observation.epoch_stable=epoch_valid;
        Chain dependencyChain{};ReadChain(info.pNext,dependencyChain);
        p.observation.epoch_guard_required=dependencyChain.libraries&&dependencyChain.libraries->libraryCount!=0;
        CaptureReferences(d,info,p.observation);
        const bool epoch_rejected=!epoch_valid&&p.observation.epoch_guard_required;
        p.rejection=epoch_rejected?Rejection::IncompletePipeline:ValidatePipeline(d,info,p.mode,p.count,p.viewport,p);
        if(!p.is_library){p.valid=p.rejection==Rejection::None;p.viewport_contract=p.valid;}
        p.observation.mode=p.mode;p.observation.rejection=p.rejection;p.observation.valid=p.valid;p.observation.is_library=p.is_library;
        p.observation.library_flags=p.library_flags;
        if(epoch_rejected)p.observation.detail="native_creation_epoch_changed";
        else if(p.rejection==Rejection::None)p.observation.detail="accepted";
        else if(p.rejection==Rejection::IncompletePipeline)
        {
            p.observation.detail="required_state_or_library_incomplete";
            for(uint32_t n=0;info.pStages&&n<(std::min)(info.stageCount,uint32_t(8));++n)
            {
                const auto& s=info.pStages[n];if(ValidShaderStage(s,info.flags))continue;const char* detail=nullptr;
                if(s.sType!=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO)detail="shader_stage_type";
                else if(s.pNext)detail="shader_stage_pnext";
                else if(s.flags)detail="shader_stage_flags";
                else if(!s.module)detail="shader_stage_module_missing";
                else if(!s.pName)detail="shader_stage_name_missing";
                if(detail){p.observation.detail=detail;p.observation.detail_index=n;break;}
            }
            if(p.observation.detail_index==UINT32_MAX&&info.pRasterizationState&&info.pRasterizationState->pNext&&!ValidRasterizationState(*info.pRasterizationState))p.observation.detail="rasterization_state_pnext";
        }
        d.pipelines[pk]=p;++d.pipeline_epoch;return true;
    }
public:
    explicit Tracker(size_t deviceCapacity=8,size_t objectCapacity=32768):device_capacity_(deviceCapacity),object_capacity_(objectCapacity){devices_.reserve(deviceCapacity);}
    bool AddDevice(VkDevice device)
    {
        std::lock_guard<std::mutex> g(mutex_);const uint64_t k=Key(device);auto i=devices_.find(k);
        if(i!=devices_.end()){if(i->second.live)return false;i->second.live=true;i->second.poisoned=false;++i->second.generation;++i->second.pipeline_epoch;i->second.sequence=0;i->second.pipelines.clear();i->second.commands.clear();return true;}
        if(devices_.size()>=device_capacity_)return false;Device d{};d.key=k;d.generation=1;d.pipeline_epoch=1;d.live=true;d.pipelines.reserve(object_capacity_);d.pipeline_generations.reserve(object_capacity_);d.commands.reserve(object_capacity_);d.command_generations.reserve(object_capacity_);d.recording_generations.reserve(object_capacity_);devices_.emplace(k,std::move(d));return true;
    }
    bool RemoveDevice(VkDevice device)
    {
        std::lock_guard<std::mutex>g(mutex_);auto*d=FindDevice(Key(device));if(!d)return false;d->live=false;d->poisoned=false;++d->pipeline_epoch;d->pipelines.clear();d->commands.clear();return true;
    }
    bool ObservePipelineCreate(VkDevice device,VkPipeline pipeline,const VkGraphicsPipelineCreateInfo& info)
    {
        std::lock_guard<std::mutex>g(mutex_);auto*d=FindDevice(Key(device));if(!d)return false;return ObservePipelineCreateLocked(*d,Key(pipeline),pipeline,info,true);
    }
    uint64_t PipelineMutationEpoch(VkDevice device){std::lock_guard<std::mutex>g(mutex_);auto*d=FindDevice(Key(device));return d?d->pipeline_epoch:0;}
    bool QueryPipelineObservation(VkDevice device,VkPipeline pipeline,PipelineObservation& out)
    {
        std::lock_guard<std::mutex>g(mutex_);out={};auto*d=FindDevice(Key(device));if(!d)return false;
        const auto it=d->pipelines.find(Key(pipeline));if(it==d->pipelines.end())return false;out=it->second.observation;return true;
    }
    bool QueryCommandDiagnostic(VkDevice device,VkCommandBuffer command,CommandDiagnostic& out)
    {
        std::lock_guard<std::mutex>g(mutex_);out={};auto*d=FindDevice(Key(device));if(!d)return false;
        const auto it=d->commands.find(Key(command));if(it==d->commands.end())return false;const auto& c=it->second;
        out.pipeline=c.pipeline;out.generation=c.pipeline_generation;out.first=c.first_poison;out.last=c.last_poison;return true;
    }
    bool ObservePipelineCreateAtEpoch(VkDevice device,VkPipeline pipeline,const VkGraphicsPipelineCreateInfo& info,uint64_t expectedEpoch)
    {
        return ObservePipelineCreatesAtEpoch(device,&pipeline,&info,1,expectedEpoch);
    }
    bool ObservePipelineCreatesAtEpoch(VkDevice device,const VkPipeline* pipelines,const VkGraphicsPipelineCreateInfo* infos,uint32_t count,uint64_t expectedEpoch)
    {
        std::lock_guard<std::mutex>g(mutex_);auto*d=FindDevice(Key(device));if(!d||(!pipelines&&count)||(!infos&&count))return false;const bool stable=d->pipeline_epoch==expectedEpoch;bool result=true;for(uint32_t n=0;n<count;++n){if(pipelines[n]==VK_NULL_HANDLE)continue;for(uint32_t j=0;j<n;++j)if(pipelines[j]==pipelines[n]){Poison(*d);return false;}if(!ObservePipelineCreateLocked(*d,Key(pipelines[n]),pipelines[n],infos[n],stable))result=false;}return result;
    }
    bool RetirePipeline(VkDevice device,VkPipeline pipeline){std::lock_guard<std::mutex>g(mutex_);auto*d=FindDevice(Key(device));if(!d)return false;const bool retired=d->pipelines.erase(Key(pipeline))!=0;if(retired)++d->pipeline_epoch;return retired;}
    bool AddCommand(VkDevice device,VkCommandBuffer command,bool secondary)
    {
        std::lock_guard<std::mutex>g(mutex_);auto*d=FindDevice(Key(device));if(!d||d->poisoned)return false;const uint64_t ck=Key(command);auto gi=d->command_generations.find(ck);if(gi==d->command_generations.end()&&d->command_generations.size()>=object_capacity_){Poison(*d);return false;}uint64_t gen=++d->command_generations[ck];Command c{};c.key=ck;c.generation=gen;c.secondary=secondary;d->commands[ck]=c;return true;
    }
    bool BeginCommand(VkDevice device,VkCommandBuffer command)
    {
        std::lock_guard<std::mutex>g(mutex_);auto*d=FindDevice(Key(device));if(!d||d->poisoned)return false;auto i=d->commands.find(Key(command));if(i==d->commands.end()||i->second.secondary)return false;Command c{};c.key=i->first;c.generation=i->second.generation;c.secondary=i->second.secondary;c.recording=++d->recording_generations[c.key];c.recording_live=true;c.recording_start=++d->sequence;c.sequence=d->sequence;i->second=c;return true;
    }
    bool ResetCommand(VkDevice device,VkCommandBuffer command)
    {
        std::lock_guard<std::mutex>g(mutex_);auto*d=FindDevice(Key(device));if(!d||d->poisoned)return false;auto i=d->commands.find(Key(command));if(i==d->commands.end())return false;Command c{};c.key=i->first;c.generation=++d->command_generations[c.key];c.secondary=i->second.secondary;i->second=c;return true;
    }
    bool RetireCommand(VkDevice device,VkCommandBuffer command){std::lock_guard<std::mutex>g(mutex_);auto*d=FindDevice(Key(device));if(!d)return false;return d->commands.erase(Key(command))!=0;}
    bool BindGraphicsPipeline(VkDevice device,VkCommandBuffer command,VkPipeline pipeline)
    {
        std::lock_guard<std::mutex>g(mutex_);auto*d=FindDevice(Key(device));if(!d||d->poisoned)return false;
        auto ci=d->commands.find(Key(command));if(ci==d->commands.end()||!ci->second.recording_live||ci->second.secondary)return false;
        auto& c=ci->second;Bump(*d,c);auto pi=d->pipelines.find(Key(pipeline));
        if(pi==d->pipelines.end())
        {
            c.pipeline=0;c.pipeline_generation=0;c.mode=Mode::Unknown;c.set_seen=false;c.source=Source::None;c.count=0;c.setting_sequence=0;
            c.rejection=Rejection::UnknownPipelineBinding;ObservePoison(c,PoisonEvent::UnknownPipelineBinding,c.rejection,UINT32_MAX,Key(pipeline));return true;
        }
        auto& p=pi->second;c.pipeline=p.key;c.pipeline_generation=p.generation;c.mode=p.valid&&!p.is_library?p.mode:Mode::Unknown;c.bind_sequence=c.sequence;
        if(p.is_library||!p.valid)
        {
            c.set_seen=false;c.source=Source::None;c.count=0;c.setting_sequence=0;c.rejection=p.is_library?Rejection::PipelineLibraryUnsupported:p.rejection;
            ObservePoison(c,p.is_library?PoisonEvent::LibraryBinding:PoisonEvent::RejectedPipelineBinding,c.rejection,p.observation.detail_index);return true;
        }
        if(p.mode==Mode::Static){c.set_seen=false;c.source=Source::None;c.setting_sequence=0;c.count=0;c.last_static_bind=c.sequence;}
        else if(p.mode==Mode::LegacyDynamic&&c.source!=Source::Legacy){c.set_seen=false;c.source=Source::None;c.count=0;}
        return true;
    }
    bool SetViewport(VkDevice device,VkCommandBuffer command,uint32_t first,uint32_t count,const VkViewport* v){return Set(device,command,first,count,v,Source::Legacy);}
    bool SetViewportWithCount(VkDevice device,VkCommandBuffer command,uint32_t count,const VkViewport* v,Source source){if(source!=Source::CoreWithCount&&source!=Source::ExtWithCount)return false;return Set(device,command,0,count,v,source);}
    bool MarkUnsupported(VkDevice device,VkCommandBuffer command){std::lock_guard<std::mutex>g(mutex_);auto*d=FindDevice(Key(device));if(!d||d->poisoned)return false;auto i=d->commands.find(Key(command));if(i==d->commands.end())return false;Bump(*d,i->second);ObservePoison(i->second,PoisonEvent::UnsupportedCommand,Rejection::None);return true;}
    bool QueryDrawViewport(VkDevice device,VkCommandBuffer command,Result* out)
    {
        if(!out)return false;std::lock_guard<std::mutex>g(mutex_);*out={};auto*d=FindDevice(Key(device));if(!d){out->status=Status::UnknownDevice;return true;}if(d->poisoned){out->status=Status::CapacityExceeded;out->rejection=Rejection::CapacityExceeded;return true;}auto ci=d->commands.find(Key(command));if(ci==d->commands.end()){out->status=Status::UnknownCommand;return true;}const auto&c=ci->second;out->command_generation=c.generation;out->recording_generation=c.recording;if(c.secondary){out->status=Status::SecondaryUnsupported;return true;}if(!c.recording_live){out->status=Status::NotRecording;return true;}if(c.unsupported){out->status=Status::UnsupportedCommand;out->rejection=c.rejection;out->pipeline=c.pipeline;out->pipeline_generation=c.pipeline_generation;out->mode=c.mode;out->source=c.source;out->bind_sequence=c.bind_sequence;out->setting_sequence=c.setting_sequence;return true;}auto pi=d->pipelines.find(c.pipeline);if(pi==d->pipelines.end()||pi->second.generation!=c.pipeline_generation){out->status=Status::UnknownPipeline;return true;}const auto&p=pi->second;out->mode=p.mode;out->pipeline=p.key;out->pipeline_generation=p.generation;out->bind_sequence=c.bind_sequence;out->setting_sequence=c.setting_sequence;out->source=c.source;out->viewport_count=c.count;if(!p.valid){out->status=Status::UnsupportedState;out->rejection=p.rejection;return true;}if(p.mode==Mode::Static){out->source=Source::None;out->viewport_count=p.count;std::memcpy(out->viewport,p.viewport,sizeof(out->viewport));out->contract_supported=true;out->status=Status::Ok;return true;}out->setting_after_required_boundary=c.set_seen&&c.setting_sequence>(c.last_static_bind?c.last_static_bind:c.recording_start);if((p.mode==Mode::LegacyDynamic&&c.source!=Source::Legacy)||(p.mode==Mode::DynamicWithCount&&c.source!=Source::CoreWithCount&&c.source!=Source::ExtWithCount)){out->status=Status::ModeMismatch;return true;}if(!c.set_seen||!out->setting_after_required_boundary||c.count!=1){out->status=c.count>1?Status::MultiViewport:Status::UnsupportedState;return true;}std::memcpy(out->viewport,c.viewport,sizeof(out->viewport));out->contract_supported=true;out->status=Status::Ok;return true;
    }
private:
    bool Set(VkDevice device,VkCommandBuffer command,uint32_t first,uint32_t count,const VkViewport* v,Source source)
    {
        std::lock_guard<std::mutex>g(mutex_);auto*d=FindDevice(Key(device));if(!d||d->poisoned)return false;auto i=d->commands.find(Key(command));if(i==d->commands.end()||!i->second.recording_live||i->second.secondary)return false;auto&c=i->second;Bump(*d,c);
        if(c.mode==Mode::Static)ObservePoison(c,PoisonEvent::SetWhileStatic,Rejection::None);
        c.set_seen=true;c.source=source;c.setting_sequence=c.sequence;c.count=count;
        if(first!=0||count!=1||!v){ObservePoison(c,PoisonEvent::InvalidViewportSetting,Rejection::None);return true;}
        if(!ValidViewport(v[0])){ObservePoison(c,PoisonEvent::InvalidViewportSetting,Rejection::None);return true;}CopyViewport(c.viewport,v[0]);return true;
    }
};
}
