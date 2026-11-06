#pragma once
#include <stdint.h>

#if defined(_WIN32)
#define ASTERIA_FG_CALL __stdcall
#else
#define ASTERIA_FG_CALL
#endif

#define ASTERIA_FG_INPUT_API_V1 1u
#define ASTERIA_FG_INPUT_OK 0
#define ASTERIA_FG_INPUT_INVALID_ARGUMENT -1
#define ASTERIA_FG_INPUT_UNAVAILABLE -2
#define ASTERIA_FG_INPUT_STALE_TICKET -3
#define ASTERIA_FG_INPUT_BUSY -4
#define ASTERIA_FG_INPUT_INTERNAL_ERROR -5
#define ASTERIA_FG_MOTION_ENGINE 1u
#define ASTERIA_FG_MOTION_ESTIMATED 2u
#define ASTERIA_FG_FORMAT_R32_FLOAT 1u
#define ASTERIA_FG_FORMAT_RG16_FLOAT 2u
#define ASTERIA_FG_FORMAT_RG32_FLOAT 3u
#define ASTERIA_FG_LAYOUT_SHADER_READ_ONLY 1u
#define ASTERIA_FG_DEPTH_SCENE 1u
#define ASTERIA_FG_GPU_COMPLETED 2u
#define ASTERIA_FG_HUD_ABSENT 4u
#define ASTERIA_FG_HISTORY_RESET 8u

#pragma pack(push, 8)
typedef struct AsteriaFrameInputRegistrationV1
{
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t max_live_leases;
    uint32_t reserved;
} AsteriaFrameInputRegistrationV1;

typedef struct AsteriaFrameInputTicketV1
{
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t registration;
    uint64_t device;
    uint64_t swapchain;
    uint64_t generation;
    uint64_t frame_id;
    uint32_t width;
    uint32_t height;
    uint32_t image_index;
    uint32_t reserved;
    uint64_t acquire_id;
} AsteriaFrameInputTicketV1;

typedef void (ASTERIA_FG_CALL *AsteriaFrameInputLeaseCallbackV1)(void *context);

typedef struct AsteriaFrameInputSubmissionV1
{
    uint32_t struct_size;
    uint32_t api_version;
    AsteriaFrameInputTicketV1 ticket;
    uint64_t depth_image;
    uint64_t motion_image;
    uint32_t depth_format;
    uint32_t motion_format;
    uint32_t image_layout;
    uint32_t motion_source;
    uint32_t flags;
    uint32_t reserved;
    float delta_milliseconds;
    float near_plane;
    float far_plane;
    float vertical_fov_radians;
    float meters_per_unit;
    float position[3];
    float up[3];
    float right[3];
    float forward[3];
    float motion_scale[2];
    void *lease_context;
    AsteriaFrameInputLeaseCallbackV1 retain_lease;
    AsteriaFrameInputLeaseCallbackV1 release_lease;
} AsteriaFrameInputSubmissionV1;
#pragma pack(pop)

typedef int32_t (ASTERIA_FG_CALL *PFN_AsteriaRegisterFrameInputProviderV1)(const AsteriaFrameInputRegistrationV1 *, uint64_t *);
typedef int32_t (ASTERIA_FG_CALL *PFN_AsteriaUnregisterFrameInputProviderV1)(uint64_t);
typedef int32_t (ASTERIA_FG_CALL *PFN_AsteriaQueryFrameInputTicketV1)(uint64_t, uint64_t, uint64_t, AsteriaFrameInputTicketV1 *);
typedef int32_t (ASTERIA_FG_CALL *PFN_AsteriaSubmitFrameInputsV1)(const AsteriaFrameInputSubmissionV1 *);
