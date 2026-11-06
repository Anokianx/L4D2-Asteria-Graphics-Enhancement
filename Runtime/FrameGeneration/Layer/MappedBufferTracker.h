#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace asteria_mapped_buffer
{
static bool CopyStableBytes(const void *source, uint32_t size, void *destination) noexcept
{
    unsigned char first[4096];
    unsigned char second[4096];
    __try
    {
        std::memcpy(first, source, size);
        MemoryBarrier();
        std::memcpy(second, source, size);
        if (std::memcmp(first, second, size) != 0) return false;
        std::memcpy(destination, second, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

class Tracker
{
    struct Memory
    {
        uint64_t size = 0;
        uint64_t map_offset = 0;
        uint64_t map_size = 0;
        uintptr_t pointer = 0;
    };
    struct Buffer
    {
        uint64_t size = 0;
        uint64_t usage = 0;
        uint64_t memory = 0;
        uint64_t memory_offset = 0;
    };
    struct Device
    {
        uint64_t instance = 0;
        std::unordered_map<uint64_t, Memory> memories;
        std::unordered_map<uint64_t, Buffer> buffers;
    };
    mutable std::mutex mutex;
    std::unordered_map<uint64_t, Device> devices;

    static bool Interval(uint64_t offset, uint64_t size, uint64_t total)
    {
        return size != 0 && offset <= total && size <= total - offset;
    }
    static bool PointerInterval(uintptr_t address, uint64_t size)
    {
        return address && size && size - 1 <= std::numeric_limits<uintptr_t>::max() - address;
    }
    static bool Intersects(uintptr_t first, uint64_t first_size, uintptr_t second, uint64_t second_size)
    {
        if (!PointerInterval(first, first_size) || !PointerInterval(second, second_size)) return false;
        return first <= second + second_size - 1 && second <= first + first_size - 1;
    }
    static void RemoveMemoryBindings(Device &device, uint64_t memory)
    {
        for (auto &entry : device.buffers)
            if (entry.second.memory == memory)
            {
                entry.second.memory = 0;
                entry.second.memory_offset = 0;
            }
    }

public:
    struct Stats
    {
        uint64_t devices = 0;
        uint64_t memories = 0;
        uint64_t buffers = 0;
        uint64_t mapped_memories = 0;
    };

    void AddDevice(uint64_t device, uint64_t instance)
    {
        if (!device || device > std::numeric_limits<uintptr_t>::max() || !instance) return;
        const std::lock_guard<std::mutex> lock(mutex);
        devices.erase(device);
        devices.emplace(device, Device{instance, {}, {}});
    }

    void RemoveDevice(uint64_t device)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        devices.erase(device);
    }

    void RemoveInstance(uint64_t instance)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (auto entry = devices.begin(); entry != devices.end();)
            if (entry->second.instance == instance) entry = devices.erase(entry); else ++entry;
    }

    void AddMemory(uint64_t device, uint64_t memory, uint64_t size)
    {
        if (!memory || !size) return;
        const std::lock_guard<std::mutex> lock(mutex);
        const auto owner = devices.find(device);
        if (owner == devices.end()) return;
        RemoveMemoryBindings(owner->second, memory);
        owner->second.memories.erase(memory);
        owner->second.memories.emplace(memory, Memory{size, 0, 0, 0});
    }

    void RemoveMemory(uint64_t device, uint64_t memory)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto owner = devices.find(device);
        if (owner == devices.end()) return;
        RemoveMemoryBindings(owner->second, memory);
        owner->second.memories.erase(memory);
    }

    bool Map(uint64_t device, uint64_t memory, uint64_t offset, uint64_t size, void *pointer)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto owner = devices.find(device);
        if (owner == devices.end()) return false;
        const auto entry = owner->second.memories.find(memory);
        if (entry == owner->second.memories.end()) return false;
        auto &value = entry->second;
        value.map_offset = 0;
        value.map_size = 0;
        value.pointer = 0;
        if (offset > value.size) return false;
        if (size == UINT64_MAX) size = value.size - offset;
        const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
        if (!Interval(offset, size, value.size) || !PointerInterval(address, size)) return false;
        value.map_offset = offset;
        value.map_size = size;
        value.pointer = address;
        return true;
    }

    void Unmap(uint64_t device, uint64_t memory)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto owner = devices.find(device);
        if (owner == devices.end()) return;
        const auto entry = owner->second.memories.find(memory);
        if (entry == owner->second.memories.end()) return;
        entry->second.pointer = 0;
        entry->second.map_offset = 0;
        entry->second.map_size = 0;
    }

    void AddBuffer(uint64_t device, uint64_t buffer, uint64_t size, uint64_t usage)
    {
        if (!buffer || !size) return;
        const std::lock_guard<std::mutex> lock(mutex);
        const auto owner = devices.find(device);
        if (owner == devices.end()) return;
        owner->second.buffers.erase(buffer);
        owner->second.buffers.emplace(buffer, Buffer{size, usage, 0, 0});
    }

    void RemoveBuffer(uint64_t device, uint64_t buffer)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto owner = devices.find(device);
        if (owner != devices.end()) owner->second.buffers.erase(buffer);
    }

    bool Bind(uint64_t device, uint64_t buffer, uint64_t memory, uint64_t offset)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto owner = devices.find(device);
        if (owner == devices.end()) return false;
        const auto target = owner->second.buffers.find(buffer);
        if (target == owner->second.buffers.end()) return false;
        const auto allocation = owner->second.memories.find(memory);
        if (allocation == owner->second.memories.end() || !Interval(offset, target->second.size, allocation->second.size))
        {
            target->second.memory = 0;
            target->second.memory_offset = 0;
            return false;
        }
        target->second.memory = memory;
        target->second.memory_offset = offset;
        return true;
    }

    bool Copy(uint64_t device, uint64_t buffer, uint64_t offset, uint32_t size, void *destination) const
    {
        if (!device || device > std::numeric_limits<uintptr_t>::max() || !buffer || !size || size > 4096) return false;
        const uintptr_t target = reinterpret_cast<uintptr_t>(destination);
        if (!PointerInterval(target, size)) return false;
        const std::lock_guard<std::mutex> lock(mutex);
        const auto owner = devices.find(device);
        if (owner == devices.end()) return false;
        const auto value = owner->second.buffers.find(buffer);
        if (value == owner->second.buffers.end() || !value->second.memory || !Interval(offset, size, value->second.size)) return false;
        const auto memory = owner->second.memories.find(value->second.memory);
        if (memory == owner->second.memories.end()) return false;
        const auto &allocation = memory->second;
        if (!allocation.pointer || offset > UINT64_MAX - value->second.memory_offset) return false;
        const uint64_t absolute = value->second.memory_offset + offset;
        if (!Interval(absolute, size, allocation.size) || absolute < allocation.map_offset) return false;
        const uint64_t relative = absolute - allocation.map_offset;
        if (!Interval(relative, size, allocation.map_size) || relative > std::numeric_limits<uintptr_t>::max() - allocation.pointer) return false;
        const uintptr_t source = allocation.pointer + static_cast<uintptr_t>(relative);
        if (!PointerInterval(source, size)) return false;
        for (const auto &device_entry : devices)
            for (const auto &allocation_entry : device_entry.second.memories)
                if (Intersects(target, size, allocation_entry.second.pointer, allocation_entry.second.map_size)) return false;
        return CopyStableBytes(reinterpret_cast<const void *>(source), size, destination);
    }

    Stats GetStats() const
    {
        const std::lock_guard<std::mutex> lock(mutex);
        Stats result;
        result.devices = devices.size();
        for (const auto &owner : devices)
        {
            result.memories += owner.second.memories.size();
            result.buffers += owner.second.buffers.size();
            for (const auto &memory : owner.second.memories) result.mapped_memories += memory.second.pointer != 0;
        }
        return result;
    }
};
}
