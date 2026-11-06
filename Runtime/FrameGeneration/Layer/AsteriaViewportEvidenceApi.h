#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

enum AsteriaViewportEvidenceStatus : uint32_t
{
    ASTERIA_VIEWPORT_EVIDENCE_OK = 0,
    ASTERIA_VIEWPORT_EVIDENCE_BAD_ABI = 1,
    ASTERIA_VIEWPORT_EVIDENCE_UNKNOWN_DEVICE = 2,
    ASTERIA_VIEWPORT_EVIDENCE_UNKNOWN_COMMAND = 3,
    ASTERIA_VIEWPORT_EVIDENCE_NOT_RECORDING = 4,
    ASTERIA_VIEWPORT_EVIDENCE_SECONDARY_UNSUPPORTED = 5,
    ASTERIA_VIEWPORT_EVIDENCE_RESET_OR_UNOBSERVED = 6,
    ASTERIA_VIEWPORT_EVIDENCE_MULTIVIEWPORT = 7,
    ASTERIA_VIEWPORT_EVIDENCE_NONFINITE = 8,
    ASTERIA_VIEWPORT_EVIDENCE_RETIRED = 9,
    ASTERIA_VIEWPORT_EVIDENCE_UNSUPPORTED_COMMAND_STATE = 10,
    ASTERIA_VIEWPORT_EVIDENCE_INVALID_DIMENSIONS = 11,
    ASTERIA_VIEWPORT_EVIDENCE_INVALID_DEPTH_RANGE = 12
};

enum AsteriaViewportEvidenceSource : uint32_t
{
    ASTERIA_VIEWPORT_SOURCE_NONE = 0,
    ASTERIA_VIEWPORT_SOURCE_LEGACY = 1,
    ASTERIA_VIEWPORT_SOURCE_CORE_WITH_COUNT = 2,
    ASTERIA_VIEWPORT_SOURCE_EXT_WITH_COUNT = 3
};

enum AsteriaViewportEvidenceFlags : uint32_t
{
    ASTERIA_VIEWPORT_EVIDENCE_OBSERVED_ONLY = 1u << 0
};

#pragma pack(push, 8)
struct AsteriaNativeViewportEvidenceV1
{
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t status;
    uint32_t flags;
    uint32_t source;
    uint32_t viewport_count;
    uint64_t device_generation;
    uint64_t command_generation;
    uint64_t recording_generation;
    uint64_t set_sequence;
    float viewport[6];
};
#pragma pack(pop)

static_assert(sizeof(AsteriaNativeViewportEvidenceV1) == 80, "viewport evidence ABI size");
using AsteriaQueryNativeViewportV1Fn = BOOL (__stdcall *)(uint64_t, uint64_t, uint32_t, AsteriaNativeViewportEvidenceV1 *);

extern "C" BOOL __stdcall AsteriaQueryNativeViewportV1(
    uint64_t device_native, uint64_t command_buffer_native, uint32_t output_size,
    AsteriaNativeViewportEvidenceV1 *output);
