#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

enum AsteriaDrawViewportEvidenceFlags : uint32_t
{
    ASTERIA_DRAW_VIEWPORT_RECORDING_CONTRACT_SUPPORTED = 1u,
    ASTERIA_DRAW_VIEWPORT_OBSERVER_ONLY = 2u,
    ASTERIA_DRAW_VIEWPORT_DYNAMIC_SET_AFTER_BOUNDARY = 4u
};
enum AsteriaDrawViewportMode : uint32_t
{
    ASTERIA_DRAW_VIEWPORT_UNKNOWN=0,
    ASTERIA_DRAW_VIEWPORT_STATIC=1,
    ASTERIA_DRAW_VIEWPORT_LEGACY_DYNAMIC=2,
    ASTERIA_DRAW_VIEWPORT_DYNAMIC_WITH_COUNT=3
};
enum AsteriaDrawViewportSource : uint32_t
{
    ASTERIA_DRAW_VIEWPORT_SOURCE_NONE=0,
    ASTERIA_DRAW_VIEWPORT_SOURCE_LEGACY=1,
    ASTERIA_DRAW_VIEWPORT_SOURCE_CORE_WITH_COUNT=2,
    ASTERIA_DRAW_VIEWPORT_SOURCE_EXT_WITH_COUNT=3
};
enum AsteriaDrawViewportStatus : uint32_t
{
    ASTERIA_DRAW_VIEWPORT_OK=0,
    ASTERIA_DRAW_VIEWPORT_UNKNOWN_DEVICE=1,
    ASTERIA_DRAW_VIEWPORT_UNKNOWN_PIPELINE=2,
    ASTERIA_DRAW_VIEWPORT_UNKNOWN_COMMAND=3,
    ASTERIA_DRAW_VIEWPORT_NOT_RECORDING=4,
    ASTERIA_DRAW_VIEWPORT_SECONDARY_UNSUPPORTED=5,
    ASTERIA_DRAW_VIEWPORT_UNSUPPORTED_STATE=6,
    ASTERIA_DRAW_VIEWPORT_RETIRED=7,
    ASTERIA_DRAW_VIEWPORT_MULTI_VIEWPORT=8,
    ASTERIA_DRAW_VIEWPORT_INVALID_VIEWPORT=9,
    ASTERIA_DRAW_VIEWPORT_MODE_MISMATCH=10,
    ASTERIA_DRAW_VIEWPORT_UNSUPPORTED_COMMAND=11,
    ASTERIA_DRAW_VIEWPORT_CAPACITY_EXCEEDED=12
};
enum AsteriaDrawViewportRejection : uint32_t
{
    ASTERIA_DRAW_VIEWPORT_REJECTION_NONE=0,
    ASTERIA_DRAW_VIEWPORT_REJECTION_NULL_PIPELINE=1,
    ASTERIA_DRAW_VIEWPORT_REJECTION_PIPELINE_LIBRARY=2,
    ASTERIA_DRAW_VIEWPORT_REJECTION_CREATE_FLAGS=3,
    ASTERIA_DRAW_VIEWPORT_REJECTION_PIPELINE_CHAIN=4,
    ASTERIA_DRAW_VIEWPORT_REJECTION_INCOMPLETE_PIPELINE=5,
    ASTERIA_DRAW_VIEWPORT_REJECTION_VIEWPORT_STATE=6,
    ASTERIA_DRAW_VIEWPORT_REJECTION_CONFLICTING_MODES=7,
    ASTERIA_DRAW_VIEWPORT_REJECTION_SCISSOR_STATE=8,
    ASTERIA_DRAW_VIEWPORT_REJECTION_MULTI_VIEWPORT=9,
    ASTERIA_DRAW_VIEWPORT_REJECTION_STATIC_VIEWPORT=10,
    ASTERIA_DRAW_VIEWPORT_REJECTION_CAPACITY=11,
    ASTERIA_DRAW_VIEWPORT_REJECTION_UNKNOWN_BINDING=12
};

#pragma pack(push, 8)
struct AsteriaDrawViewportEvidenceV1
{
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t status;
    uint32_t flags;
    uint32_t mode;
    uint32_t source;
    uint32_t viewport_count;
    uint32_t rejection_reason;
    uint64_t device_generation;
    uint64_t command_generation;
    uint64_t recording_generation;
    uint64_t pipeline;
    uint64_t pipeline_generation;
    uint64_t bind_sequence;
    uint64_t setting_sequence;
    float viewport[6];
};
#pragma pack(pop)
static_assert(sizeof(AsteriaDrawViewportEvidenceV1) == 112, "draw viewport evidence ABI size");
using AsteriaQueryDrawViewportV1Fn = BOOL (__stdcall *)(uint64_t, uint64_t, uint32_t, AsteriaDrawViewportEvidenceV1 *);
extern "C" BOOL __stdcall AsteriaQueryDrawViewportV1(uint64_t device_native, uint64_t command_buffer_native,
    uint32_t output_size, AsteriaDrawViewportEvidenceV1 *output);
