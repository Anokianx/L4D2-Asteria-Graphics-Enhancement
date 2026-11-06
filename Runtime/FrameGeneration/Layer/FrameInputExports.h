#pragma once
#include "FrameInputBridge.h"
#include <cstring>

template<typename T> static bool AsteriaCopyInputV1(const T *source, T &copy)
{
    if (!source) return false;
    uint32_t header[2]{};
    std::memcpy(header, source, sizeof(header));
    if (header[0] != sizeof(T) || header[1] != ASTERIA_FG_INPUT_API_V1) return false;
    std::memcpy(&copy, source, sizeof(T));
    return copy.struct_size == sizeof(T) && copy.api_version == ASTERIA_FG_INPUT_API_V1;
}

extern "C" int32_t ASTERIA_FG_CALL AsteriaRegisterFrameInputProviderV1(const AsteriaFrameInputRegistrationV1 *info, uint64_t *id)
{
    AsteriaFrameInputRegistrationV1 copy{};
    if (!id || !AsteriaCopyInputV1(info, copy)) return ASTERIA_FG_INPUT_INVALID_ARGUMENT;
    try { return asteria_fg_runtime::input_bridge::Register(copy, *id); }
    catch (...) { return ASTERIA_FG_INPUT_INTERNAL_ERROR; }
}

extern "C" int32_t ASTERIA_FG_CALL AsteriaUnregisterFrameInputProviderV1(uint64_t id)
{
    try { return asteria_fg_runtime::input_bridge::Unregister(id); }
    catch (...) { return ASTERIA_FG_INPUT_INTERNAL_ERROR; }
}

extern "C" int32_t ASTERIA_FG_CALL AsteriaQueryFrameInputTicketV1(uint64_t id, uint64_t device, uint64_t swapchain, AsteriaFrameInputTicketV1 *ticket)
{
    AsteriaFrameInputTicketV1 copy{};
    if (!AsteriaCopyInputV1(ticket, copy))
        return ASTERIA_FG_INPUT_INVALID_ARGUMENT;
    try
    {
        const auto result = asteria_fg_runtime::input_bridge::Query(id, device, swapchain, copy);
        if (result == ASTERIA_FG_INPUT_OK) *ticket = copy;
        return result;
    }
    catch (...) { return ASTERIA_FG_INPUT_INTERNAL_ERROR; }
}

extern "C" int32_t ASTERIA_FG_CALL AsteriaSubmitFrameInputsV1(const AsteriaFrameInputSubmissionV1 *input)
{
    AsteriaFrameInputSubmissionV1 copy{};
    if (!AsteriaCopyInputV1(input, copy)) return ASTERIA_FG_INPUT_INVALID_ARGUMENT;
    try { return asteria_fg_runtime::input_bridge::Submit(copy); }
    catch (...) { return ASTERIA_FG_INPUT_INTERNAL_ERROR; }
}

