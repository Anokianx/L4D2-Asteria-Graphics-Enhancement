#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

enum AsteriaDescriptorEvidenceStatus : uint32_t
{
    ASTERIA_DESCRIPTOR_OK = 0,
    ASTERIA_DESCRIPTOR_BAD_ABI = 1,
    ASTERIA_DESCRIPTOR_UNKNOWN_DEVICE = 2,
    ASTERIA_DESCRIPTOR_CAP_EXCEEDED = 3,
    ASTERIA_DESCRIPTOR_UNKNOWN_COMMAND = 4,
    ASTERIA_DESCRIPTOR_NOT_RECORDING = 5,
    ASTERIA_DESCRIPTOR_SECONDARY_UNSUPPORTED = 6,
    ASTERIA_DESCRIPTOR_UNKNOWN_PIPELINE = 7,
    ASTERIA_DESCRIPTOR_UNKNOWN_SET = 8,
    ASTERIA_DESCRIPTOR_LAYOUT_INCOMPATIBLE = 9,
    ASTERIA_DESCRIPTOR_UNSUPPORTED_LAYOUT = 10,
    ASTERIA_DESCRIPTOR_UNKNOWN_BINDING = 11,
    ASTERIA_DESCRIPTOR_NOT_BUFFER = 12,
    ASTERIA_DESCRIPTOR_UNINITIALIZED = 13,
    ASTERIA_DESCRIPTOR_DYNAMIC_OFFSETS_INVALID = 14,
    ASTERIA_DESCRIPTOR_BUFFER_RETIRED = 15,
    ASTERIA_DESCRIPTOR_RANGE_INVALID = 16,
    ASTERIA_DESCRIPTOR_UNSUPPORTED_COMMAND_STATE = 17
};

#pragma pack(push, 8)
struct AsteriaBoundBufferDescriptorV1
{
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t status;
    uint32_t descriptor_type;
    uint32_t stage_flags;
    uint32_t dynamic_offset_count;
    uint32_t dynamic_offset_index;
    uint32_t reserved;
    uint64_t device_generation;
    uint64_t command_generation;
    uint64_t recording_generation;
    uint64_t set_generation;
    uint64_t layout_generation;
    uint64_t buffer_generation;
    uint64_t bind_sequence;
    uint64_t descriptor_set;
    uint64_t descriptor_set_layout;
    uint64_t pipeline_layout;
    uint64_t buffer;
    uint64_t buffer_offset;
    uint64_t range;
    uint64_t dynamic_offset;
    uint64_t effective_offset;
    uint64_t buffer_size;
    uint64_t cap_hits;
    uint64_t unsupported_events;
};
#pragma pack(pop)

static_assert(sizeof(AsteriaBoundBufferDescriptorV1) == 176, "descriptor evidence ABI size");
using AsteriaQueryBoundBufferDescriptorFn = BOOL (__stdcall *)(
    uint64_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, AsteriaBoundBufferDescriptorV1 *);

extern "C" BOOL __stdcall AsteriaQueryBoundBufferDescriptor(
    uint64_t device_native, uint64_t command_buffer_native, uint32_t set_index,
    uint32_t binding, uint32_t element, uint32_t output_size, AsteriaBoundBufferDescriptorV1 *output);
