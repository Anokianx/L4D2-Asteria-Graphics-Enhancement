#pragma once
#include "AsteriaDescriptorEvidenceApi.h"
#include "AsteriaDrawViewportEvidenceApi.h"
#include "AsteriaDrawViewportDiagnosticApi.h"
#include "GraphicsViewportState.h"
#include "AsteriaViewportEvidenceApi.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <type_traits>
#include <vector>

namespace asteria_descriptor_evidence
{
constexpr uint32_t DrawViewportAbi(asteria::graphics_viewport::Status value)
{
    using T=asteria::graphics_viewport::Status;
    switch (value)
    {
    case T::Ok:return ASTERIA_DRAW_VIEWPORT_OK;
    case T::UnknownDevice:return ASTERIA_DRAW_VIEWPORT_UNKNOWN_DEVICE;
    case T::UnknownPipeline:return ASTERIA_DRAW_VIEWPORT_UNKNOWN_PIPELINE;
    case T::UnknownCommand:return ASTERIA_DRAW_VIEWPORT_UNKNOWN_COMMAND;
    case T::NotRecording:return ASTERIA_DRAW_VIEWPORT_NOT_RECORDING;
    case T::SecondaryUnsupported:return ASTERIA_DRAW_VIEWPORT_SECONDARY_UNSUPPORTED;
    case T::UnsupportedState:return ASTERIA_DRAW_VIEWPORT_UNSUPPORTED_STATE;
    case T::Retired:return ASTERIA_DRAW_VIEWPORT_RETIRED;
    case T::MultiViewport:return ASTERIA_DRAW_VIEWPORT_MULTI_VIEWPORT;
    case T::InvalidViewport:return ASTERIA_DRAW_VIEWPORT_INVALID_VIEWPORT;
    case T::ModeMismatch:return ASTERIA_DRAW_VIEWPORT_MODE_MISMATCH;
    case T::UnsupportedCommand:return ASTERIA_DRAW_VIEWPORT_UNSUPPORTED_COMMAND;
    case T::CapacityExceeded:return ASTERIA_DRAW_VIEWPORT_CAPACITY_EXCEEDED;
    }
    return ASTERIA_DRAW_VIEWPORT_UNSUPPORTED_STATE;
}
constexpr uint32_t DrawViewportAbi(asteria::graphics_viewport::Rejection value)
{
    using T=asteria::graphics_viewport::Rejection;
    switch (value)
    {
    case T::None:return ASTERIA_DRAW_VIEWPORT_REJECTION_NONE;
    case T::NullPipeline:return ASTERIA_DRAW_VIEWPORT_REJECTION_NULL_PIPELINE;
    case T::PipelineLibraryUnsupported:return ASTERIA_DRAW_VIEWPORT_REJECTION_PIPELINE_LIBRARY;
    case T::UnsupportedCreateFlags:return ASTERIA_DRAW_VIEWPORT_REJECTION_CREATE_FLAGS;
    case T::UnknownPipelineChain:return ASTERIA_DRAW_VIEWPORT_REJECTION_PIPELINE_CHAIN;
    case T::IncompletePipeline:return ASTERIA_DRAW_VIEWPORT_REJECTION_INCOMPLETE_PIPELINE;
    case T::UnsupportedViewportState:return ASTERIA_DRAW_VIEWPORT_REJECTION_VIEWPORT_STATE;
    case T::ConflictingViewportModes:return ASTERIA_DRAW_VIEWPORT_REJECTION_CONFLICTING_MODES;
    case T::UnsupportedScissorState:return ASTERIA_DRAW_VIEWPORT_REJECTION_SCISSOR_STATE;
    case T::MultiViewportUnsupported:return ASTERIA_DRAW_VIEWPORT_REJECTION_MULTI_VIEWPORT;
    case T::InvalidStaticViewport:return ASTERIA_DRAW_VIEWPORT_REJECTION_STATIC_VIEWPORT;
    case T::CapacityExceeded:return ASTERIA_DRAW_VIEWPORT_REJECTION_CAPACITY;
    case T::UnknownPipelineBinding:return ASTERIA_DRAW_VIEWPORT_REJECTION_UNKNOWN_BINDING;
    }
    return ASTERIA_DRAW_VIEWPORT_REJECTION_VIEWPORT_STATE;
}
constexpr uint32_t DrawViewportAbi(asteria::graphics_viewport::Mode value)
{
    using T=asteria::graphics_viewport::Mode;
    switch (value)
    {
    case T::Unknown:return ASTERIA_DRAW_VIEWPORT_UNKNOWN;
    case T::Static:return ASTERIA_DRAW_VIEWPORT_STATIC;
    case T::LegacyDynamic:return ASTERIA_DRAW_VIEWPORT_LEGACY_DYNAMIC;
    case T::DynamicWithCount:return ASTERIA_DRAW_VIEWPORT_DYNAMIC_WITH_COUNT;
    }
    return ASTERIA_DRAW_VIEWPORT_UNKNOWN;
}
constexpr uint32_t DrawViewportAbi(asteria::graphics_viewport::Source value)
{
    using T=asteria::graphics_viewport::Source;
    switch (value)
    {
    case T::None:return ASTERIA_DRAW_VIEWPORT_SOURCE_NONE;
    case T::Legacy:return ASTERIA_DRAW_VIEWPORT_SOURCE_LEGACY;
    case T::CoreWithCount:return ASTERIA_DRAW_VIEWPORT_SOURCE_CORE_WITH_COUNT;
    case T::ExtWithCount:return ASTERIA_DRAW_VIEWPORT_SOURCE_EXT_WITH_COUNT;
    }
    return ASTERIA_DRAW_VIEWPORT_SOURCE_NONE;
}
constexpr uint32_t DrawPoisonAbi(asteria::graphics_viewport::PoisonEvent value)
{
    using T=asteria::graphics_viewport::PoisonEvent;
    switch(value)
    {
    case T::None:return ASTERIA_VIEWPORT_POISON_NONE;
    case T::UnknownPipelineBinding:return ASTERIA_VIEWPORT_POISON_UNKNOWN_PIPELINE;
    case T::LibraryBinding:return ASTERIA_VIEWPORT_POISON_LIBRARY_BINDING;
    case T::RejectedPipelineBinding:return ASTERIA_VIEWPORT_POISON_REJECTED_PIPELINE;
    case T::UnsupportedCommand:return ASTERIA_VIEWPORT_POISON_UNSUPPORTED_COMMAND;
    case T::SetWhileStatic:return ASTERIA_VIEWPORT_POISON_SET_WHILE_STATIC;
    case T::InvalidViewportSetting:return ASTERIA_VIEWPORT_POISON_INVALID_VIEWPORT;
    }
    return ASTERIA_VIEWPORT_POISON_UNSUPPORTED_COMMAND;
}
template<typename T> static uint64_t Handle(T value)
{
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(value);
    else return static_cast<uint64_t>(value);
}

class Tracker
{
    struct Binding { uint32_t index, count; VkDescriptorType type; VkShaderStageFlags stages; };
    struct Layout { uint64_t generation = 0; bool supported = false; std::vector<Binding> bindings; };
    struct PipelineLayout { uint64_t generation = 0; bool supported = false; std::vector<std::pair<uint64_t,uint64_t>> sets; std::vector<VkPushConstantRange> pushes; };
    struct Buffer { uint64_t generation = 0, size = 0; };
    struct Value { uint64_t buffer = 0, generation = 0, offset = 0, range = 0; bool known = false; };
    struct Set { uint64_t generation = 0, pool = 0, pool_generation = 0, layout = 0, layout_generation = 0; bool supported = false; std::map<std::pair<uint32_t,uint32_t>,Value> values; };
    struct Pool { uint64_t generation = 0; bool supported = false; };
    struct Pipeline { uint64_t layout = 0, layout_generation = 0; };
    struct Bound { uint64_t set = 0, set_generation = 0, layout = 0, layout_generation = 0, sequence = 0; uint32_t dynamic_count = 0; bool dynamic_valid = false; std::map<std::pair<uint32_t,uint32_t>,std::pair<uint32_t,uint32_t>> offsets; };
    struct Command { uint64_t generation = 0, recording_generation = 0, pool = 0, pool_generation = 0, pipeline = 0, pipeline_layout = 0, pipeline_layout_generation = 0, viewport_sequence = 0; uint32_t viewport_source = ASTERIA_VIEWPORT_SOURCE_NONE, viewport_count = 0; AsteriaViewportEvidenceStatus viewport_status = ASTERIA_VIEWPORT_EVIDENCE_RESET_OR_UNOBSERVED; std::array<float,6> viewport{}; bool recording = false, secondary = false, unsupported = false; std::map<uint32_t,Bound> bound; };
    struct Template { uint64_t layout = 0, layout_generation = 0; bool supported = false; std::vector<VkDescriptorUpdateTemplateEntry> entries; };
    struct Device
    {
        uint64_t instance = 0, generation = 0, cap_hits = 0, unsupported_events = 0, sequence = 0;
        bool poisoned = false;
        std::unordered_map<uint64_t,Layout> layouts;
        std::unordered_map<uint64_t,PipelineLayout> pipeline_layouts;
        std::unordered_map<uint64_t,Buffer> buffers;
        std::unordered_map<uint64_t,Pool> pools, command_pools;
        std::unordered_map<uint64_t,Set> sets;
        std::unordered_map<uint64_t,Pipeline> pipelines;
        std::unordered_map<uint64_t,Command> commands;
        std::unordered_map<uint64_t,std::pair<uint64_t,uint64_t>> retired_commands;
        std::unordered_map<uint64_t,Template> templates;
    };
    mutable std::mutex mutex;
    std::unordered_map<uint64_t,Device> devices;
    uint64_t generation = 0, global_cap_hits = 0;
    asteria::graphics_viewport::Tracker graphics_viewport;

    template<typename T> bool Reserve(Device &d, const T &items, uint64_t key, size_t cap)
    {
        if (items.count(key) || items.size() < cap) return true;
        ++d.cap_hits; d.poisoned = true; return false;
    }
    static bool BufferType(VkDescriptorType type)
    {
        return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    }
    static bool Dynamic(VkDescriptorType type)
    {
        return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    }
    static const Layout *GetLayout(const Device &d, uint64_t id, uint64_t gen)
    {
        const auto it = d.layouts.find(id);
        return it != d.layouts.end() && it->second.generation == gen ? &it->second : nullptr;
    }
    static bool SameLayout(const Device &d, uint64_t a, uint64_t ag, uint64_t b, uint64_t bg)
    {
        const auto x = GetLayout(d,a,ag), y = GetLayout(d,b,bg);
        if (!x || !y || !x->supported || !y->supported || x->bindings.size() != y->bindings.size()) return false;
        for (size_t i = 0; i < x->bindings.size(); ++i)
        {
            const auto &u=x->bindings[i], &v=y->bindings[i];
            if (u.index!=v.index || u.count!=v.count || u.type!=v.type || u.stages!=v.stages) return false;
        }
        return true;
    }
    static bool Compatible(const Device &d, uint64_t a, uint64_t ag, uint64_t b, uint64_t bg, uint32_t set)
    {
        const auto ai=d.pipeline_layouts.find(a), bi=d.pipeline_layouts.find(b);
        if (ai==d.pipeline_layouts.end() || bi==d.pipeline_layouts.end()) return false;
        const auto &x=ai->second, &y=bi->second;
        if (x.generation!=ag || y.generation!=bg || !x.supported || !y.supported || x.sets.size()<=set || y.sets.size()<=set || x.pushes.size()!=y.pushes.size()) return false;
        for (size_t i=0;i<x.pushes.size();++i)
            if (x.pushes[i].stageFlags!=y.pushes[i].stageFlags || x.pushes[i].offset!=y.pushes[i].offset || x.pushes[i].size!=y.pushes[i].size) return false;
        for (uint32_t i=0;i<=set;++i)
            if (!SameLayout(d,x.sets[i].first,x.sets[i].second,y.sets[i].first,y.sets[i].second)) return false;
        return true;
    }
    static const Binding *FindBinding(const Layout &layout, uint32_t binding)
    {
        for (const auto &value:layout.bindings) if (value.index==binding) return &value;
        return nullptr;
    }
    static bool Locations(const Layout &layout, uint32_t binding, uint32_t element, uint32_t count, VkDescriptorType type,
                          std::vector<std::pair<uint32_t,uint32_t>> &result)
    {
        if (count>4096) return false;
        const Binding *initial=FindBinding(layout,binding);
        if (!initial || element>=initial->count || initial->type!=type) return false;
        uint32_t current=binding, index=element;
        while (count)
        {
            const auto part=FindBinding(layout,current);
            if (!part || part->type!=type || part->stages!=initial->stages) return false;
            while (index<part->count && count) { result.emplace_back(current,index++); --count; }
            if (count && current==UINT32_MAX) return false;
            ++current; index=0;
        }
        return true;
    }
    static const Layout *SetLayout(const Device &d, const Set &set)
    {
        const auto pool=d.pools.find(set.pool);
        if (!set.supported || pool==d.pools.end() || pool->second.generation!=set.pool_generation || !pool->second.supported) return nullptr;
        const auto layout=GetLayout(d,set.layout,set.layout_generation);
        return layout && layout->supported ? layout : nullptr;
    }
    static void InvalidateSet(Device &d, uint64_t set)
    {
        auto it=d.sets.find(set); if (it!=d.sets.end()) { it->second.supported=false; it->second.values.clear(); }
        ++d.unsupported_events;
    }
    static Value MakeValue(const Device &d, const VkDescriptorBufferInfo &info)
    {
        const auto it=d.buffers.find(Handle(info.buffer));
        if (it==d.buffers.end()) return {};
        return Value{Handle(info.buffer),it->second.generation,info.offset,info.range,true};
    }
    static void Write(Device &d, const VkWriteDescriptorSet &write)
    {
        auto it=d.sets.find(Handle(write.dstSet));
        if (it==d.sets.end()) return;
        const auto layout=SetLayout(d,it->second);
        std::vector<std::pair<uint32_t,uint32_t>> locations;
        if (!layout || write.pNext || !Locations(*layout,write.dstBinding,write.dstArrayElement,write.descriptorCount,write.descriptorType,locations) || (BufferType(write.descriptorType) && !write.pBufferInfo))
        { InvalidateSet(d,Handle(write.dstSet)); return; }
        if (!BufferType(write.descriptorType)) return;
        for (size_t i=0;i<locations.size();++i) it->second.values[locations[i]]=MakeValue(d,write.pBufferInfo[i]);
    }
    static void Copy(Device &d, const VkCopyDescriptorSet &copy)
    {
        auto source=d.sets.find(Handle(copy.srcSet)), target=d.sets.find(Handle(copy.dstSet));
        if (target==d.sets.end()) return;
        const auto sl=source==d.sets.end()?nullptr:SetLayout(d,source->second), tl=SetLayout(d,target->second);
        const auto binding=sl?FindBinding(*sl,copy.srcBinding):nullptr;
        std::vector<std::pair<uint32_t,uint32_t>> from,to;
        if (!binding || !tl || copy.pNext || !Locations(*sl,copy.srcBinding,copy.srcArrayElement,copy.descriptorCount,binding->type,from) || !Locations(*tl,copy.dstBinding,copy.dstArrayElement,copy.descriptorCount,binding->type,to))
        { InvalidateSet(d,Handle(copy.dstSet)); return; }
        if (!BufferType(binding->type)) return;
        std::vector<Value> values; values.reserve(from.size());
        for (const auto &key:from) { const auto item=source->second.values.find(key); values.push_back(item==source->second.values.end()?Value{}:item->second); }
        for (size_t i=0;i<to.size();++i) target->second.values[to[i]]=values[i];
    }

public:
    struct Stats { uint64_t devices=0, layouts=0, sets=0, commands=0, cap_hits=0, unsupported_events=0; };
    void AddDevice(uint64_t id,uint64_t instance)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!id || !instance) return;
        if (!devices.count(id) && devices.size()>=8) { ++global_cap_hits; return; }
        graphics_viewport.RemoveDevice(reinterpret_cast<VkDevice>(uintptr_t(id)));
        devices.erase(id); Device d; d.instance=instance; d.generation=++generation; devices.emplace(id,std::move(d));
        graphics_viewport.AddDevice(reinterpret_cast<VkDevice>(uintptr_t(id)));
    }
    void RemoveDevice(uint64_t id) { const std::lock_guard<std::mutex> lock(mutex); graphics_viewport.RemoveDevice(reinterpret_cast<VkDevice>(uintptr_t(id))); devices.erase(id); }
    void RemoveInstance(uint64_t id)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (auto it=devices.begin();it!=devices.end();) if (it->second.instance==id) { graphics_viewport.RemoveDevice(reinterpret_cast<VkDevice>(uintptr_t(it->first))); it=devices.erase(it); } else ++it;
    }
    void Poison(uint64_t id)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto d=devices.find(id);
        if (d!=devices.end()) { ++d->second.cap_hits; d->second.poisoned=true; }
    }
    void AddBuffer(uint64_t id,uint64_t buffer,uint64_t size)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end() || !buffer) return;
        if (Reserve(it->second,it->second.buffers,buffer,32768)) it->second.buffers[buffer]={++generation,size};
    }
    void RemoveBuffer(uint64_t id,uint64_t buffer) { const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it!=devices.end()) it->second.buffers.erase(buffer); }
    void AddLayout(uint64_t id,uint64_t layout,const VkDescriptorSetLayoutCreateInfo &info)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end() || !layout) return; auto &d=it->second;
        if (!Reserve(d,d.layouts,layout,4096)) return;
        Layout value; value.generation=++generation;
        value.supported=!info.pNext && !info.flags && info.bindingCount<=128 && (!info.bindingCount || info.pBindings);
        uint64_t total=0;
        if (value.supported) for (uint32_t i=0;i<info.bindingCount;++i)
        {
            const auto &b=info.pBindings[i]; total+=b.descriptorCount;
            if (b.pImmutableSamplers || total>4096 || FindBinding(value,b.binding)) { value.supported=false; break; }
            value.bindings.push_back({b.binding,b.descriptorCount,b.descriptorType,b.stageFlags});
        }
        std::sort(value.bindings.begin(),value.bindings.end(),[](const Binding &a,const Binding &b){return a.index<b.index;});
        if (!value.supported) ++d.unsupported_events;
        d.layouts[layout]=std::move(value);
    }
    void RemoveLayout(uint64_t id,uint64_t layout) { const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it!=devices.end()) it->second.layouts.erase(layout); }
    void AddPipelineLayout(uint64_t id,uint64_t layout,const VkPipelineLayoutCreateInfo &info)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end() || !layout) return; auto &d=it->second;
        if (!Reserve(d,d.pipeline_layouts,layout,4096)) return;
        PipelineLayout value; value.generation=++generation; value.supported=!info.pNext && !info.flags && info.setLayoutCount<=32 && info.pushConstantRangeCount<=32;
        if (value.supported)
        {
            for (uint32_t i=0;i<info.setLayoutCount;++i) { auto l=d.layouts.find(Handle(info.pSetLayouts[i])); value.sets.emplace_back(Handle(info.pSetLayouts[i]),l==d.layouts.end()?0:l->second.generation); if (l==d.layouts.end() || !l->second.supported) value.supported=false; }
            if (info.pushConstantRangeCount) value.pushes.assign(info.pPushConstantRanges,info.pPushConstantRanges+info.pushConstantRangeCount);
        }
        if (!value.supported) ++d.unsupported_events;
        d.pipeline_layouts[layout]=std::move(value);
    }
    void RemovePipelineLayout(uint64_t id,uint64_t layout) { const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it!=devices.end()) it->second.pipeline_layouts.erase(layout); }
    void AddPool(uint64_t id,uint64_t pool,bool command,bool supported=true)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end() || !pool) return; auto &d=it->second;
        auto &items=command?d.command_pools:d.pools;
        if (Reserve(d,items,pool,4096)) items[pool]={++generation,supported};
    }
    void ResetPool(uint64_t id,uint64_t pool,bool command,bool remove)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end()) return; auto &d=it->second;
        if (command)
        {
            for (const auto &entry:d.commands) if (entry.second.pool==pool)
            {
                if (remove) graphics_viewport.RetireCommand(reinterpret_cast<VkDevice>(uintptr_t(id)),reinterpret_cast<VkCommandBuffer>(uintptr_t(entry.first)));
                else graphics_viewport.ResetCommand(reinterpret_cast<VkDevice>(uintptr_t(id)),reinterpret_cast<VkCommandBuffer>(uintptr_t(entry.first)));
            }
            for (auto c=d.commands.begin();c!=d.commands.end();) if (c->second.pool==pool) { if (remove) { if (d.retired_commands.size()>=32768) d.retired_commands.erase(d.retired_commands.begin()); d.retired_commands[c->first]={c->second.generation,c->second.recording_generation}; c=d.commands.erase(c); } else { c->second.recording=false; c->second.bound.clear(); c->second.pipeline=0; c->second.viewport_source=ASTERIA_VIEWPORT_SOURCE_NONE; c->second.viewport_count=0; c->second.viewport_sequence=0; c->second.viewport_status=ASTERIA_VIEWPORT_EVIDENCE_RESET_OR_UNOBSERVED; c->second.viewport={}; ++c->second.recording_generation; ++c; } } else ++c;
        }
        else for (auto s=d.sets.begin();s!=d.sets.end();) if (s->second.pool==pool) s=d.sets.erase(s); else ++s;
        if (remove) (command?d.command_pools:d.pools).erase(pool);
    }
    void AddSets(uint64_t id,const VkDescriptorSetAllocateInfo &info,const VkDescriptorSet *sets)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end()) return; auto &d=it->second;
        if (info.descriptorSetCount>32768) { ++d.cap_hits; d.poisoned=true; return; }
        const auto pool=d.pools.find(Handle(info.descriptorPool));
        for (uint32_t i=0;i<info.descriptorSetCount;++i)
        {
            const uint64_t key=Handle(sets[i]); if (!Reserve(d,d.sets,key,32768)) return;
            const auto layout=d.layouts.find(Handle(info.pSetLayouts[i])); Set value; value.generation=++generation;
            value.pool=Handle(info.descriptorPool); value.pool_generation=pool==d.pools.end()?0:pool->second.generation;
            value.layout=Handle(info.pSetLayouts[i]); value.layout_generation=layout==d.layouts.end()?0:layout->second.generation;
            value.supported=!info.pNext && pool!=d.pools.end() && pool->second.supported && layout!=d.layouts.end() && layout->second.supported;
            if (!value.supported) ++d.unsupported_events;
            d.sets[key]=std::move(value);
        }
    }
    void RemoveSets(uint64_t id,uint32_t count,const VkDescriptorSet *sets)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end()) return;
        if (count>32768) { ++it->second.cap_hits; it->second.poisoned=true; return; }
        for (uint32_t i=0;i<count;++i) it->second.sets.erase(Handle(sets[i]));
    }
    void Update(uint64_t id,uint32_t wc,const VkWriteDescriptorSet *writes,uint32_t cc,const VkCopyDescriptorSet *copies)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end()) return; auto &d=it->second;
        if (wc>4096 || cc>4096) { ++d.cap_hits; d.poisoned=true; return; }
        for (uint32_t i=0;i<wc;++i) Write(d,writes[i]);
        for (uint32_t i=0;i<cc;++i) Copy(d,copies[i]);
    }
    void AddTemplate(uint64_t id,uint64_t handle,const VkDescriptorUpdateTemplateCreateInfo &info)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end()) return; auto &d=it->second;
        if (!Reserve(d,d.templates,handle,4096)) return;
        Template value; value.layout=Handle(info.descriptorSetLayout); const auto l=d.layouts.find(value.layout); value.layout_generation=l==d.layouts.end()?0:l->second.generation;
        value.supported=!info.pNext && !info.flags && info.templateType==VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET && info.descriptorUpdateEntryCount<=128 && l!=d.layouts.end() && l->second.supported;
        if (value.supported) for (uint32_t i=0;i<info.descriptorUpdateEntryCount;++i)
        {
            const auto &entry=info.pDescriptorUpdateEntries[i];
            if (entry.descriptorCount>4096 || entry.offset>1048576 || entry.stride>1048576 || (entry.descriptorCount && uint64_t(entry.offset)+uint64_t(entry.descriptorCount-1)*entry.stride+sizeof(VkDescriptorBufferInfo)>1048576)) { value.supported=false; break; }
            value.entries.push_back(entry);
        }
        if (!value.supported) ++d.unsupported_events;
        d.templates[handle]=std::move(value);
    }
    void RemoveTemplate(uint64_t id,uint64_t handle) { const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it!=devices.end()) it->second.templates.erase(handle); }
    void UpdateTemplate(uint64_t id,uint64_t set,uint64_t handle,const void *data)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end()) return; auto &d=it->second;
        const auto t=d.templates.find(handle); auto s=d.sets.find(set);
        if (t==d.templates.end() || s==d.sets.end() || !t->second.supported || !data || !SameLayout(d,t->second.layout,t->second.layout_generation,s->second.layout,s->second.layout_generation)) { InvalidateSet(d,set); return; }
        for (const auto &entry:t->second.entries)
        {
            std::vector<VkDescriptorBufferInfo> infos;
            if (BufferType(entry.descriptorType)) for (uint32_t i=0;i<entry.descriptorCount;++i) { VkDescriptorBufferInfo info; std::memcpy(&info,static_cast<const unsigned char *>(data)+entry.offset+size_t(i)*entry.stride,sizeof(info)); infos.push_back(info); }
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=(VkDescriptorSet)set; write.dstBinding=entry.dstBinding; write.dstArrayElement=entry.dstArrayElement; write.descriptorCount=entry.descriptorCount; write.descriptorType=entry.descriptorType; write.pBufferInfo=infos.data(); Write(d,write);
        }
    }
    void AddPipeline(uint64_t id,uint64_t pipeline,uint64_t layout)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end() || !pipeline) return; auto &d=it->second;
        if (!Reserve(d,d.pipelines,pipeline,32768)) return;
        const auto l=d.pipeline_layouts.find(layout); d.pipelines[pipeline]={layout,l==d.pipeline_layouts.end()?0:l->second.generation};
    }
    void RemovePipeline(uint64_t id,uint64_t pipeline)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end()) return; it->second.pipelines.erase(pipeline);
        graphics_viewport.RetirePipeline(reinterpret_cast<VkDevice>(uintptr_t(id)),VkPipeline(pipeline));
        for (auto &c:it->second.commands) if (c.second.pipeline==pipeline) { c.second.pipeline=0; c.second.pipeline_layout=0; c.second.pipeline_layout_generation=0; }
    }
    void AddGraphicsPipeline(uint64_t id,uint64_t pipeline,const VkGraphicsPipelineCreateInfo &info)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end() || !pipeline) return; auto &d=it->second;
        if (!Reserve(d,d.pipelines,pipeline,32768)) return;
        const auto l=d.pipeline_layouts.find(Handle(info.layout));
        d.pipelines[pipeline]={Handle(info.layout),l==d.pipeline_layouts.end()?0:l->second.generation};
        graphics_viewport.ObservePipelineCreate(reinterpret_cast<VkDevice>(uintptr_t(id)),VkPipeline(pipeline),info);
    }
    uint64_t GraphicsPipelineMutationEpoch(uint64_t id)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return graphics_viewport.PipelineMutationEpoch(reinterpret_cast<VkDevice>(uintptr_t(id)));
    }
    bool QueryGraphicsPipelineObservation(uint64_t id,uint64_t pipeline,uint64_t& deviceGeneration,asteria::graphics_viewport::PipelineObservation& out)
    {
        const std::lock_guard<std::mutex> lock(mutex);const auto it=devices.find(id);deviceGeneration=it==devices.end()?0:it->second.generation;
        return it!=devices.end()&&graphics_viewport.QueryPipelineObservation(reinterpret_cast<VkDevice>(uintptr_t(id)),VkPipeline(pipeline),out);
    }
    bool QueryDrawViewportDiagnostic(uint64_t id,uint64_t command,AsteriaDrawViewportDiagnosticV1& out)
    {
        const std::lock_guard<std::mutex> lock(mutex);out={};out.struct_size=sizeof(out);out.api_version=1;out.flags=ASTERIA_DRAW_DIAGNOSTIC_OBSERVER_ONLY;
        out.payload_hint_index=UINT32_MAX;const auto di=devices.find(id);
        if(di==devices.end()){out.status=ASTERIA_DRAW_VIEWPORT_UNKNOWN_DEVICE;return false;}
        out.device_generation=di->second.generation;const auto ci=di->second.commands.find(command);
        if(ci==di->second.commands.end()){out.status=ASTERIA_DRAW_VIEWPORT_UNKNOWN_COMMAND;return false;}
        out.command_generation=ci->second.generation;out.recording_generation=ci->second.recording_generation;
        if(ci->second.recording)out.flags|=ASTERIA_DRAW_DIAGNOSTIC_RECORDING;
        asteria::graphics_viewport::CommandDiagnostic value{};
        if(!graphics_viewport.QueryCommandDiagnostic(reinterpret_cast<VkDevice>(uintptr_t(id)),reinterpret_cast<VkCommandBuffer>(uintptr_t(command)),value))
        {out.status=ASTERIA_DRAW_VIEWPORT_UNSUPPORTED_STATE;return false;}
        out.pipeline=value.pipeline;out.pipeline_generation=value.generation;
        asteria::graphics_viewport::PipelineObservation pipeline{};
        if(graphics_viewport.QueryPipelineObservation(reinterpret_cast<VkDevice>(uintptr_t(id)),VkPipeline(value.pipeline),pipeline)&&pipeline.generation==value.generation)
        {out.current_creation_rejection=DrawViewportAbi(pipeline.rejection);out.payload_hint_index=pipeline.detail_index;out.flags|=ASTERIA_DRAW_DIAGNOSTIC_CURRENT_CREATION_OBSERVED;}
        const auto copy=[](const asteria::graphics_viewport::PoisonObservation& source,AsteriaViewportPoisonV1& target)
        {
            target.pipeline=source.pipeline;target.pipeline_generation=source.generation;target.sequence=source.sequence;
            target.event=DrawPoisonAbi(source.event);target.rejection_reason=DrawViewportAbi(source.rejection);
            target.payload_hint_index=source.detail_index;
        };
        copy(value.first,out.first_poison);copy(value.last,out.last_poison);out.status=ASTERIA_DRAW_VIEWPORT_OK;return true;
    }
    void AddGraphicsPipelinesAtEpoch(uint64_t id,uint32_t count,const VkGraphicsPipelineCreateInfo *infos,const VkPipeline *pipelines,uint64_t epoch,
        std::vector<asteria::graphics_viewport::PipelineObservation>* observations=nullptr,uint64_t* deviceGeneration=nullptr)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end()) return; auto &d=it->second;
        if (count>32768 || (!infos&&count) || (!pipelines&&count)) { ++d.cap_hits; d.poisoned=true; return; }
        for (uint32_t index=0;index<count;++index)
        {
            if (!pipelines[index]) continue;
            const auto pipeline=Handle(pipelines[index]);
            if (!Reserve(d,d.pipelines,pipeline,32768)) return;
            const auto layout=Handle(infos[index].layout); const auto l=d.pipeline_layouts.find(layout);
            d.pipelines[pipeline]={layout,l==d.pipeline_layouts.end()?0:l->second.generation};
        }
        if (!graphics_viewport.ObservePipelineCreatesAtEpoch(reinterpret_cast<VkDevice>(uintptr_t(id)),pipelines,infos,count,epoch)) d.poisoned=true;
        if(deviceGeneration)*deviceGeneration=d.generation;
        if(observations)
        {
            observations->resize(count);
            for(uint32_t index=0;index<count;++index)if(pipelines[index])
                graphics_viewport.QueryPipelineObservation(reinterpret_cast<VkDevice>(uintptr_t(id)),pipelines[index],(*observations)[index]);
        }
    }
    void AddCommands(uint64_t id,const VkCommandBufferAllocateInfo &info,const VkCommandBuffer *commands)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end()) return; auto &d=it->second;
        if (info.commandBufferCount>32768) { ++d.cap_hits; d.poisoned=true; return; }
        const auto pool=d.command_pools.find(Handle(info.commandPool));
        for (uint32_t i=0;i<info.commandBufferCount;++i)
        {
            const auto key=Handle(commands[i]); if (!Reserve(d,d.commands,key,32768)) return;
            d.retired_commands.erase(key);
            Command value; value.generation=++generation; value.pool=Handle(info.commandPool); value.pool_generation=pool==d.command_pools.end()?0:pool->second.generation;
            value.secondary=info.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY; value.unsupported=info.pNext || pool==d.command_pools.end() || !pool->second.supported;
            d.commands[key]=std::move(value);
            graphics_viewport.AddCommand(reinterpret_cast<VkDevice>(uintptr_t(id)),commands[i],info.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        }
    }
    void RemoveCommands(uint64_t id,uint32_t count,const VkCommandBuffer *commands)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto it=devices.find(id); if (it==devices.end()) return;
        if (count>32768) { ++it->second.cap_hits; it->second.poisoned=true; return; }
        for (uint32_t i=0;i<count;++i)
        {
            const uint64_t key=Handle(commands[i]);
            const auto command=it->second.commands.find(key);
            if (command!=it->second.commands.end())
            {
                if (it->second.retired_commands.size()>=32768) it->second.retired_commands.erase(it->second.retired_commands.begin());
                it->second.retired_commands[key]={command->second.generation,command->second.recording_generation};
                it->second.commands.erase(command);
                graphics_viewport.RetireCommand(reinterpret_cast<VkDevice>(uintptr_t(id)),commands[i]);
            }
        }
    }
    void Begin(uint64_t id,uint64_t command,bool recording)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto d=devices.find(id); if (d==devices.end()) return; auto it=d->second.commands.find(command); if (it==d->second.commands.end()) return;
        auto &c=it->second; c.recording=recording; c.bound.clear(); c.pipeline=0; c.pipeline_layout=0; c.pipeline_layout_generation=0; c.unsupported=false; c.viewport_source=ASTERIA_VIEWPORT_SOURCE_NONE; c.viewport_count=0; c.viewport_sequence=0; c.viewport_status=ASTERIA_VIEWPORT_EVIDENCE_RESET_OR_UNOBSERVED; c.viewport={}; ++c.recording_generation;
        if (recording) graphics_viewport.BeginCommand(reinterpret_cast<VkDevice>(uintptr_t(id)),reinterpret_cast<VkCommandBuffer>(uintptr_t(command)));
        else graphics_viewport.ResetCommand(reinterpret_cast<VkDevice>(uintptr_t(id)),reinterpret_cast<VkCommandBuffer>(uintptr_t(command)));
    }
    void End(uint64_t id,uint64_t command)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto d=devices.find(id); if (d==devices.end()) return; auto c=d->second.commands.find(command); if (c!=d->second.commands.end()) c->second.recording=false;
    }
    void SetViewport(uint64_t id,uint64_t command,uint32_t source,uint32_t first,uint32_t count,const float *values,bool readable)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto di=devices.find(id); if (di==devices.end()) return;
        auto ci=di->second.commands.find(command); if (ci==di->second.commands.end()) return; auto &c=ci->second;
        if (!c.recording) return;
        const auto native_device=reinterpret_cast<VkDevice>(uintptr_t(id));
        const auto native_command=reinterpret_cast<VkCommandBuffer>(uintptr_t(command));
        if (readable && values && count==1)
        {
            const VkViewport value{values[0],values[1],values[2],values[3],values[4],values[5]};
            if (source==ASTERIA_VIEWPORT_SOURCE_LEGACY) graphics_viewport.SetViewport(native_device,native_command,first,count,&value);
            else graphics_viewport.SetViewportWithCount(native_device,native_command,count,&value,static_cast<asteria::graphics_viewport::Source>(source));
        }
        else graphics_viewport.MarkUnsupported(native_device,native_command);
        c.viewport_sequence=++di->second.sequence; c.viewport_source=source; c.viewport_count=count; c.viewport={};
        if (c.secondary) { c.viewport_status=ASTERIA_VIEWPORT_EVIDENCE_SECONDARY_UNSUPPORTED; return; }
        if (c.unsupported) { c.viewport_status=ASTERIA_VIEWPORT_EVIDENCE_UNSUPPORTED_COMMAND_STATE; return; }
        if (first!=0) { c.viewport_status=ASTERIA_VIEWPORT_EVIDENCE_RESET_OR_UNOBSERVED; return; }
        if (count!=1) { c.viewport_status=ASTERIA_VIEWPORT_EVIDENCE_MULTIVIEWPORT; return; }
        if (!readable || !values) { c.viewport_status=ASTERIA_VIEWPORT_EVIDENCE_RESET_OR_UNOBSERVED; return; }
        for (size_t i=0;i<c.viewport.size();++i)
            if (!std::isfinite(values[i])) { c.viewport_status=ASTERIA_VIEWPORT_EVIDENCE_NONFINITE; return; }
        if (values[2]<=0.0f || values[3]==0.0f) { c.viewport_status=ASTERIA_VIEWPORT_EVIDENCE_INVALID_DIMENSIONS; return; }
        if (values[4]<0.0f || values[4]>1.0f || values[5]<0.0f || values[5]>1.0f)
        { c.viewport_status=ASTERIA_VIEWPORT_EVIDENCE_INVALID_DEPTH_RANGE; return; }
        std::copy_n(values,c.viewport.size(),c.viewport.begin());
        c.viewport_status=ASTERIA_VIEWPORT_EVIDENCE_OK;
    }
    bool QueryViewport(uint64_t id,uint64_t command,AsteriaNativeViewportEvidenceV1 &out) const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        out={}; out.struct_size=sizeof(out); out.api_version=1; out.flags=ASTERIA_VIEWPORT_EVIDENCE_OBSERVED_ONLY;
        const auto fail=[&](AsteriaViewportEvidenceStatus status){out.status=status;return false;};
        auto di=devices.find(id); if (di==devices.end()) return fail(ASTERIA_VIEWPORT_EVIDENCE_UNKNOWN_DEVICE); const auto &d=di->second;
        auto ci=d.commands.find(command);
        if (ci==d.commands.end())
        {
            const auto retired=d.retired_commands.find(command);
            if (retired!=d.retired_commands.end())
            {
                out.device_generation=d.generation; out.command_generation=retired->second.first;
                out.recording_generation=retired->second.second;
                return fail(ASTERIA_VIEWPORT_EVIDENCE_RETIRED);
            }
            return fail(ASTERIA_VIEWPORT_EVIDENCE_UNKNOWN_COMMAND);
        }
        const auto &c=ci->second; out.device_generation=d.generation; out.command_generation=c.generation;
        out.recording_generation=c.recording_generation; out.set_sequence=c.viewport_sequence; out.source=c.viewport_source;
        out.viewport_count=c.viewport_count; std::copy(c.viewport.begin(),c.viewport.end(),out.viewport);
        if (!c.recording) return fail(ASTERIA_VIEWPORT_EVIDENCE_NOT_RECORDING);
        if (c.secondary) return fail(ASTERIA_VIEWPORT_EVIDENCE_SECONDARY_UNSUPPORTED);
        if (c.unsupported) return fail(ASTERIA_VIEWPORT_EVIDENCE_UNSUPPORTED_COMMAND_STATE);
        if (d.poisoned || global_cap_hits) return fail(ASTERIA_VIEWPORT_EVIDENCE_UNSUPPORTED_COMMAND_STATE);
        if (c.viewport_status!=ASTERIA_VIEWPORT_EVIDENCE_OK) return fail(c.viewport_status);
        out.status=ASTERIA_VIEWPORT_EVIDENCE_OK; return true;
    }
    bool QueryDrawViewport(uint64_t id,uint64_t command,AsteriaDrawViewportEvidenceV1 &out)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        using Status=asteria::graphics_viewport::Status;
        out={}; out.struct_size=sizeof(out); out.api_version=1; out.flags=ASTERIA_DRAW_VIEWPORT_OBSERVER_ONLY;
        const auto fail=[&](Status status) { out.status=DrawViewportAbi(status); return false; };
        const auto di=devices.find(id); if (di==devices.end()) return fail(Status::UnknownDevice);
        const auto &device=di->second; out.device_generation=device.generation;
        const auto ci=device.commands.find(command); if (ci==device.commands.end()) return fail(Status::UnknownCommand);
        const auto &record=ci->second; out.command_generation=record.generation; out.recording_generation=record.recording_generation;
        if (!record.recording) return fail(Status::NotRecording);
        if (record.secondary) return fail(Status::SecondaryUnsupported);
        if (record.unsupported || device.poisoned || global_cap_hits) return fail(Status::UnsupportedCommand);
        asteria::graphics_viewport::Result result{};
        graphics_viewport.QueryDrawViewport(reinterpret_cast<VkDevice>(uintptr_t(id)),reinterpret_cast<VkCommandBuffer>(uintptr_t(command)),&result);
        out.status=DrawViewportAbi(result.status); out.mode=DrawViewportAbi(result.mode); out.source=DrawViewportAbi(result.source);
        out.rejection_reason=DrawViewportAbi(result.rejection);
        out.viewport_count=result.viewport_count; out.pipeline=result.pipeline; out.pipeline_generation=result.pipeline_generation;
        out.bind_sequence=result.bind_sequence; out.setting_sequence=result.setting_sequence;
        std::copy_n(result.viewport,6,out.viewport);
        if (result.pipeline && result.pipeline!=record.pipeline) return fail(Status::UnknownPipeline);
        if (!result.contract_supported || result.status!=Status::Ok) return false;
        out.flags|=ASTERIA_DRAW_VIEWPORT_RECORDING_CONTRACT_SUPPORTED;
        if (result.setting_after_required_boundary) out.flags|=ASTERIA_DRAW_VIEWPORT_DYNAMIC_SET_AFTER_BOUNDARY;
        return true;
    }
    void Unsupported(uint64_t id,uint64_t command)
    {
        const std::lock_guard<std::mutex> lock(mutex); auto d=devices.find(id); if (d==devices.end()) return; auto c=d->second.commands.find(command); if (c!=d->second.commands.end()) { c->second.unsupported=true; c->second.bound.clear(); ++d->second.unsupported_events; graphics_viewport.MarkUnsupported(reinterpret_cast<VkDevice>(uintptr_t(id)),reinterpret_cast<VkCommandBuffer>(uintptr_t(command))); }
    }
    void BindPipeline(uint64_t id,uint64_t command,VkPipelineBindPoint point,uint64_t pipeline)
    {
        if (point!=VK_PIPELINE_BIND_POINT_GRAPHICS) return;
        const std::lock_guard<std::mutex> lock(mutex); auto di=devices.find(id); if (di==devices.end()) return; auto &d=di->second; auto c=d.commands.find(command); if (c==d.commands.end()) return;
        auto p=d.pipelines.find(pipeline); c->second.pipeline=pipeline; c->second.pipeline_layout=p==d.pipelines.end()?0:p->second.layout; c->second.pipeline_layout_generation=p==d.pipelines.end()?0:p->second.layout_generation;
        graphics_viewport.BindGraphicsPipeline(reinterpret_cast<VkDevice>(uintptr_t(id)),reinterpret_cast<VkCommandBuffer>(uintptr_t(command)),VkPipeline(pipeline));
    }
    void Bind(uint64_t id,uint64_t command,VkPipelineBindPoint point,uint64_t layout,uint32_t first,uint32_t count,const VkDescriptorSet *sets,uint32_t dynamic_count,const uint32_t *dynamic)
    {
        if (point!=VK_PIPELINE_BIND_POINT_GRAPHICS) return;
        const std::lock_guard<std::mutex> lock(mutex); auto di=devices.find(id); if (di==devices.end()) return; auto &d=di->second; auto ci=d.commands.find(command); if (ci==d.commands.end()) return; auto &c=ci->second;
        if (first>32 || count>32-first || dynamic_count>4096) { ++d.cap_hits; d.poisoned=true; return; }
        auto li=d.pipeline_layouts.find(layout);
        if (li==d.pipeline_layouts.end() || !li->second.supported || first+count>li->second.sets.size() || (dynamic_count && !dynamic)) { c.bound.clear(); ++d.unsupported_events; return; }
        const auto &pl=li->second;
        for (auto b=c.bound.begin();b!=c.bound.end();) if (!Compatible(d,b->second.layout,b->second.layout_generation,layout,pl.generation,b->first)) b=c.bound.erase(b); else ++b;
        uint32_t consumed=0; const uint64_t sequence=++d.sequence;
        std::vector<std::pair<uint32_t,Bound>> updates;
        for (uint32_t i=0;i<count;++i)
        {
            Bound value; value.set=Handle(sets[i]); const auto s=d.sets.find(value.set); value.set_generation=s==d.sets.end()?0:s->second.generation;
            value.layout=layout; value.layout_generation=pl.generation; value.sequence=sequence; value.dynamic_count=dynamic_count; value.dynamic_valid=true;
            const auto expected=GetLayout(d,pl.sets[first+i].first,pl.sets[first+i].second);
            if (!expected || !expected->supported) value.dynamic_valid=false;
            else for (const auto &binding:expected->bindings) if (Dynamic(binding.type)) for (uint32_t element=0;element<binding.count;++element)
            {
                if (consumed>=dynamic_count) value.dynamic_valid=false;
                value.offsets[{binding.index,element}]={consumed,consumed<dynamic_count?dynamic[consumed]:0}; ++consumed;
            }
            updates.emplace_back(first+i,std::move(value));
        }
        for (auto &u:updates) { if (consumed!=dynamic_count) u.second.dynamic_valid=false; c.bound[u.first]=std::move(u.second); }
    }
    bool Query(uint64_t id,uint64_t command,uint32_t set_index,uint32_t binding,uint32_t element,AsteriaBoundBufferDescriptorV1 &out) const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        out={}; out.struct_size=sizeof(out); out.api_version=1; out.dynamic_offset_index=UINT32_MAX;
        const auto fail=[&](AsteriaDescriptorEvidenceStatus status){out.status=status;return false;};
        auto di=devices.find(id); if (di==devices.end()) return fail(ASTERIA_DESCRIPTOR_UNKNOWN_DEVICE); const auto &d=di->second;
        out.device_generation=d.generation; out.cap_hits=d.cap_hits; out.unsupported_events=d.unsupported_events;
        if (d.poisoned || global_cap_hits) return fail(ASTERIA_DESCRIPTOR_CAP_EXCEEDED);
        auto ci=d.commands.find(command); if (ci==d.commands.end()) return fail(ASTERIA_DESCRIPTOR_UNKNOWN_COMMAND); const auto &c=ci->second;
        out.command_generation=c.generation; out.recording_generation=c.recording_generation;
        if (!c.recording) return fail(ASTERIA_DESCRIPTOR_NOT_RECORDING);
        if (c.secondary) return fail(ASTERIA_DESCRIPTOR_SECONDARY_UNSUPPORTED);
        const auto cp=d.command_pools.find(c.pool);
        if (c.unsupported || cp==d.command_pools.end() || cp->second.generation!=c.pool_generation || !cp->second.supported) return fail(ASTERIA_DESCRIPTOR_UNSUPPORTED_COMMAND_STATE);
        if (!c.pipeline || !c.pipeline_layout) return fail(ASTERIA_DESCRIPTOR_UNKNOWN_PIPELINE);
        auto bi=c.bound.find(set_index); if (bi==c.bound.end()) return fail(ASTERIA_DESCRIPTOR_UNKNOWN_SET); const auto &b=bi->second;
        out.bind_sequence=b.sequence; out.descriptor_set=b.set; out.pipeline_layout=b.layout; out.dynamic_offset_count=b.dynamic_count;
        auto si=d.sets.find(b.set); if (si==d.sets.end() || si->second.generation!=b.set_generation) return fail(ASTERIA_DESCRIPTOR_UNKNOWN_SET); const auto &s=si->second;
        out.set_generation=s.generation; out.descriptor_set_layout=s.layout; out.layout_generation=s.layout_generation;
        const auto layout=SetLayout(d,s); if (!layout) return fail(ASTERIA_DESCRIPTOR_UNSUPPORTED_LAYOUT);
        const auto pl=d.pipeline_layouts.find(b.layout);
        if (pl==d.pipeline_layouts.end() || pl->second.generation!=b.layout_generation || pl->second.sets.size()<=set_index || !SameLayout(d,s.layout,s.layout_generation,pl->second.sets[set_index].first,pl->second.sets[set_index].second) || !Compatible(d,b.layout,b.layout_generation,c.pipeline_layout,c.pipeline_layout_generation,set_index)) return fail(ASTERIA_DESCRIPTOR_LAYOUT_INCOMPATIBLE);
        const auto bd=FindBinding(*layout,binding); if (!bd || element>=bd->count) return fail(ASTERIA_DESCRIPTOR_UNKNOWN_BINDING);
        out.descriptor_type=bd->type; out.stage_flags=bd->stages;
        if (!BufferType(bd->type)) return fail(ASTERIA_DESCRIPTOR_NOT_BUFFER);
        if (!b.dynamic_valid) return fail(ASTERIA_DESCRIPTOR_DYNAMIC_OFFSETS_INVALID);
        if (Dynamic(bd->type)) { const auto off=b.offsets.find({binding,element}); if (off==b.offsets.end()) return fail(ASTERIA_DESCRIPTOR_DYNAMIC_OFFSETS_INVALID); out.dynamic_offset_index=off->second.first; out.dynamic_offset=off->second.second; }
        const auto value=s.values.find({binding,element}); if (value==s.values.end() || !value->second.known) return fail(ASTERIA_DESCRIPTOR_UNINITIALIZED); const auto &v=value->second;
        out.buffer=v.buffer; out.buffer_offset=v.offset; out.range=v.range; out.buffer_generation=v.generation;
        const auto buffer=d.buffers.find(v.buffer); if (buffer==d.buffers.end() || buffer->second.generation!=v.generation) return fail(ASTERIA_DESCRIPTOR_BUFFER_RETIRED);
        out.buffer_size=buffer->second.size;
        if (v.offset>out.buffer_size || out.dynamic_offset>out.buffer_size-v.offset) return fail(ASTERIA_DESCRIPTOR_RANGE_INVALID);
        out.effective_offset=v.offset+out.dynamic_offset;
        const uint64_t range=v.range==VK_WHOLE_SIZE?out.buffer_size-v.offset:v.range;
        if (!range || range>out.buffer_size-out.effective_offset) return fail(ASTERIA_DESCRIPTOR_RANGE_INVALID);
        out.status=ASTERIA_DESCRIPTOR_OK; return true;
    }
    Stats GetStats() const
    {
        const std::lock_guard<std::mutex> lock(mutex); Stats result; result.devices=devices.size(); result.cap_hits=global_cap_hits;
        for (const auto &d:devices) { result.layouts+=d.second.layouts.size(); result.sets+=d.second.sets.size(); result.commands+=d.second.commands.size(); result.cap_hits+=d.second.cap_hits; result.unsupported_events+=d.second.unsupported_events; }
        return result;
    }
};
}
