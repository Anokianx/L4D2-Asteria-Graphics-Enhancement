#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

enum AsteriaDrawViewportDiagnosticFlags : uint32_t
{
    ASTERIA_DRAW_DIAGNOSTIC_RECORDING=1,
    ASTERIA_DRAW_DIAGNOSTIC_OBSERVER_ONLY=2,
    ASTERIA_DRAW_DIAGNOSTIC_CREATION_CACHE_ENABLED=4,
    ASTERIA_DRAW_DIAGNOSTIC_CURRENT_CREATION_OBSERVED=8
};
enum AsteriaViewportPoisonEvent : uint32_t
{
    ASTERIA_VIEWPORT_POISON_NONE=0,
    ASTERIA_VIEWPORT_POISON_UNKNOWN_PIPELINE=1,
    ASTERIA_VIEWPORT_POISON_LIBRARY_BINDING=2,
    ASTERIA_VIEWPORT_POISON_REJECTED_PIPELINE=3,
    ASTERIA_VIEWPORT_POISON_UNSUPPORTED_COMMAND=4,
    ASTERIA_VIEWPORT_POISON_SET_WHILE_STATIC=5,
    ASTERIA_VIEWPORT_POISON_INVALID_VIEWPORT=6
};
#pragma pack(push,8)
struct AsteriaViewportPoisonV1
{
    uint64_t pipeline,pipeline_generation,sequence;
    uint32_t event,rejection_reason,payload_hint_index,reserved;
};
struct AsteriaDrawViewportDiagnosticV1
{
    uint32_t struct_size,api_version,status,flags;
    uint64_t device_generation,command_generation,recording_generation;
    uint64_t pipeline,pipeline_generation;
    uint32_t current_creation_rejection,payload_hint_index;
    AsteriaViewportPoisonV1 first_poison,last_poison;
};
#pragma pack(pop)
static_assert(sizeof(AsteriaViewportPoisonV1)==40,"viewport poison diagnostic ABI size");
static_assert(sizeof(AsteriaDrawViewportDiagnosticV1)==144,"draw viewport diagnostic ABI size");
using AsteriaQueryDrawViewportDiagnosticV1Fn=BOOL (__stdcall *)(uint64_t,uint64_t,uint32_t,AsteriaDrawViewportDiagnosticV1 *);
extern "C" BOOL __stdcall AsteriaQueryDrawViewportDiagnosticV1(uint64_t device_native,uint64_t command_native,
    uint32_t output_size,AsteriaDrawViewportDiagnosticV1 *output);
