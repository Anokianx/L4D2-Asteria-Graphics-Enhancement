#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace asteria_estimated_motion
{
struct Parameters
{
    uint32_t width=0, height=0, blockSize=8, historyValid=0;
    float forwardBackwardLimit=2.5f, photometricLimit=0.12f, depthLimit=0.01f, minimumTexture=0.002f;
    float minimumConfidence=0.25f;
    uint32_t reversedDepth=0, forwardHistoryFrames=0, reverseHistoryFrames=0;
};
static_assert(sizeof(Parameters)==48);

struct ImageBinding
{
    VkImageView view=VK_NULL_HANDLE;
    VkImageLayout layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};

struct PipelineState
{
    PipelineState()=default;
    PipelineState(const PipelineState &)=delete;
    PipelineState &operator=(const PipelineState &)=delete;
    VkDevice device=VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptors=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;
    VkSampler sampler=VK_NULL_HANDLE;
    ~PipelineState()
    {
        if(!device) return;
        if(pipeline) vkDestroyPipeline(device,pipeline,nullptr);
        if(layout) vkDestroyPipelineLayout(device,layout,nullptr);
        if(descriptors) vkDestroyDescriptorSetLayout(device,descriptors,nullptr);
        if(sampler) vkDestroySampler(device,sampler,nullptr);
    }
};

struct DispatchLifetime
{
    const std::shared_ptr<const PipelineState> state;
    VkDescriptorPool pool=VK_NULL_HANDLE;
    VkDescriptorSet set=VK_NULL_HANDLE;
    explicit DispatchLifetime(std::shared_ptr<const PipelineState> owner) : state(std::move(owner)) {}
    DispatchLifetime(const DispatchLifetime &)=delete;
    DispatchLifetime &operator=(const DispatchLifetime &)=delete;
    ~DispatchLifetime() { if(pool && state) vkDestroyDescriptorPool(state->device,pool,nullptr); }
};

class BidirectionalFlowMotion
{
    std::shared_ptr<const PipelineState> state;
public:
    inline static constexpr uint32_t MaximumDispatchDimension=65535u*8u;
    inline static constexpr const char *LifetimeContract="Caller retains dispatch token, device and bound images until GPU completion; no implicit wait";
    BidirectionalFlowMotion()=default;
    BidirectionalFlowMotion(const BidirectionalFlowMotion &)=delete;
    BidirectionalFlowMotion &operator=(const BidirectionalFlowMotion &)=delete;
    ~BidirectionalFlowMotion() { Destroy(); }

    void Destroy() { std::atomic_store_explicit(&state,std::shared_ptr<const PipelineState>{},std::memory_order_release); }

    bool Initialize(VkDevice owner,const std::vector<uint32_t> &code)
    {
        if(std::atomic_load_explicit(&state,std::memory_order_acquire) || !owner || code.size()<5 || code[0]!=0x07230203) return false;
        auto created=std::make_shared<PipelineState>(); created->device=owner;
        std::array<VkDescriptorSetLayoutBinding,12> bindings{};
        for(uint32_t i=0;i<bindings.size();++i)
            bindings[i]={i,i<10 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        descriptorInfo.bindingCount=uint32_t(bindings.size()); descriptorInfo.pBindings=bindings.data();
        if(vkCreateDescriptorSetLayout(owner,&descriptorInfo,nullptr,&created->descriptors)!=VK_SUCCESS) return false;
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Parameters)};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount=1; layoutInfo.pSetLayouts=&created->descriptors;
        layoutInfo.pushConstantRangeCount=1; layoutInfo.pPushConstantRanges=&push;
        if(vkCreatePipelineLayout(owner,&layoutInfo,nullptr,&created->layout)!=VK_SUCCESS) return false;
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize=code.size()*sizeof(uint32_t); moduleInfo.pCode=code.data();
        VkShaderModule module=VK_NULL_HANDLE;
        if(vkCreateShaderModule(owner,&moduleInfo,nullptr,&module)!=VK_SUCCESS) return false;
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.layout=created->layout;
        compute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
        const auto result=vkCreateComputePipelines(owner,VK_NULL_HANDLE,1,&compute,nullptr,&created->pipeline);
        vkDestroyShaderModule(owner,module,nullptr);
        if(result!=VK_SUCCESS) return false;
        VkSamplerCreateInfo sample{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sample.magFilter=sample.minFilter=VK_FILTER_NEAREST;
        sample.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sample.addressModeU=sample.addressModeV=sample.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if(vkCreateSampler(owner,&sample,nullptr,&created->sampler)!=VK_SUCCESS) return false;
        std::shared_ptr<const PipelineState> expected, published=std::move(created);
        return std::atomic_compare_exchange_strong_explicit(&state,&expected,published,std::memory_order_acq_rel,std::memory_order_acquire);
    }

    std::shared_ptr<DispatchLifetime> Record(VkCommandBuffer command,const std::array<ImageBinding,10> &inputs,VkImageView motion,VkImageView confidence,const Parameters &parameters)
    {
        const auto pipeline=std::atomic_load_explicit(&state,std::memory_order_acquire);
        if(!pipeline || !command || !motion || !confidence || motion==confidence || !parameters.width || !parameters.height ||
            parameters.width>MaximumDispatchDimension || parameters.height>MaximumDispatchDimension ||
            parameters.blockSize!=8 || !std::isfinite(parameters.forwardBackwardLimit) || parameters.forwardBackwardLimit<=0 ||
            !std::isfinite(parameters.photometricLimit) || parameters.photometricLimit<=0 ||
            !std::isfinite(parameters.depthLimit) || parameters.depthLimit<0 ||
            !std::isfinite(parameters.minimumTexture) || parameters.minimumTexture<0 ||
            !std::isfinite(parameters.minimumConfidence) || parameters.minimumConfidence<0 || parameters.minimumConfidence>1) return {};
        std::array<VkDescriptorImageInfo,12> images{};
        std::array<VkWriteDescriptorSet,12> writes{};
        for(uint32_t i=0;i<inputs.size();++i)
        {
            if(!inputs[i].view || inputs[i].view==motion || inputs[i].view==confidence ||
                (inputs[i].layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && inputs[i].layout!=VK_IMAGE_LAYOUT_GENERAL)) return {};
            images[i]={pipeline->sampler,inputs[i].view,inputs[i].layout};
        }
        auto lifetime=std::make_shared<DispatchLifetime>(pipeline);
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,10},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets=1; poolInfo.poolSizeCount=2; poolInfo.pPoolSizes=sizes;
        if(vkCreateDescriptorPool(pipeline->device,&poolInfo,nullptr,&lifetime->pool)!=VK_SUCCESS) return {};
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool=lifetime->pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&pipeline->descriptors;
        if(vkAllocateDescriptorSets(pipeline->device,&allocate,&lifetime->set)!=VK_SUCCESS) return {};
        images[10]={VK_NULL_HANDLE,motion,VK_IMAGE_LAYOUT_GENERAL};
        images[11]={VK_NULL_HANDLE,confidence,VK_IMAGE_LAYOUT_GENERAL};
        for(uint32_t i=0;i<writes.size();++i)
        {
            writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet=lifetime->set; writes[i].dstBinding=i; writes[i].descriptorCount=1;
            writes[i].descriptorType=i<10 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo=&images[i];
        }
        vkUpdateDescriptorSets(pipeline->device,uint32_t(writes.size()),writes.data(),0,nullptr);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline->pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline->layout,0,1,&lifetime->set,0,nullptr);
        vkCmdPushConstants(command,pipeline->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(parameters),&parameters);
        vkCmdDispatch(command,1+(parameters.width-1)/8,1+(parameters.height-1)/8,1);
        return lifetime;
    }
};
}
