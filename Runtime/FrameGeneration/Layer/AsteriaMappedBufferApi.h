#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

using AsteriaCopyMappedBufferBytesFn = BOOL (__stdcall *)(uint64_t, uint64_t, uint64_t, uint32_t, void *);

extern "C" BOOL __stdcall AsteriaCopyMappedBufferBytes(
    uint64_t device_native, uint64_t buffer_native, uint64_t buffer_offset,
    uint32_t byte_count, void *destination);
