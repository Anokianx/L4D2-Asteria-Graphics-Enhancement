#pragma once
#include "../Layer/FrameInputExports.h"
#include "../Layer/Tests/FrameInputProducerFixture.h"

class CompletedSceneSession final : public asteria_fg_runtime::FrameInputSession
{
    struct Owner
    {
        ProducerLeaseState state;
        std::atomic<uint32_t> references{1};
        std::shared_ptr<void> resources;
        std::shared_ptr<std::atomic<uint32_t>> releases;
    };
    static void ASTERIA_FG_CALL RetainOwner(void *context)
    {
        ++static_cast<Owner *>(context)->references;
    }
    static void ASTERIA_FG_CALL ReleaseOwner(void *context)
    {
        auto *owner = static_cast<Owner *>(context);
        if (--owner->references == 0) { ++*owner->releases; delete owner; }
    }
    uint64_t device = 0;
    const uint64_t swapchain = UINT64_C(0xC07A011ED0000001);
    uint64_t registration = 0, nextFrame = 0;
    bool published = false;
    HMODULE producer = nullptr;
    PFN_ProducerSubmit submit = nullptr;
    std::shared_ptr<asteria_fg_runtime::FrameInputSession> session;
    std::shared_ptr<std::atomic<uint32_t>> releases = std::make_shared<std::atomic<uint32_t>>(0);
    uint32_t submissions = 0;
public:
    explicit CompletedSceneSession(const std::filesystem::path &root)
    {
        producer = LoadLibraryW(std::filesystem::absolute(root / "../Layer/Tests/bin/AsteriaFrameInputProducerFixture.dll").c_str());
        if (!producer) throw std::runtime_error("Independent input producer DLL unavailable");
        try
        {
            submit = reinterpret_cast<PFN_ProducerSubmit>(GetProcAddress(producer, "ProducerSubmit"));
            if (!submit) throw std::runtime_error("Independent input producer C ABI unavailable");
            AsteriaFrameInputRegistrationV1 info{sizeof(info), ASTERIA_FG_INPUT_API_V1, 3};
            if (AsteriaRegisterFrameInputProviderV1(&info, &registration) != ASTERIA_FG_INPUT_OK)
                throw std::runtime_error("Input bridge registration failed");
        }
        catch (...)
        {
            if (registration) AsteriaUnregisterFrameInputProviderV1(registration);
            FreeLibrary(producer); producer = nullptr;
            throw;
        }
    }
    void AttachDevice(uint64_t owner)
    {
        if (!registration || device || !owner) throw std::runtime_error("Controlled input bridge device attachment invalid");
        device = owner;
        session = asteria_fg_runtime::CurrentFrameInputProvider()->Open(device, Width, Height);
        if (!session) throw std::runtime_error("Controlled input bridge session failed");
        session->BindSwapchain(swapchain);
    }
    ~CompletedSceneSession()
    {
        if (session) session->RetireAfterGpuIdle();
        if (registration) AsteriaUnregisterFrameInputProviderV1(registration);
        if (producer) FreeLibrary(producer);
    }
    bool Publish(std::shared_ptr<const asteria_fg_runtime::FrameInputs> input)
    {
        if (published || !input || !asteria_fg_runtime::ValidFrameInputs(*input, device, nextFrame, Width, Height)) return false;
        session->RecordAcquire(0);
        AsteriaFrameInputSubmissionV1 native{sizeof(native), ASTERIA_FG_INPUT_API_V1};
        native.ticket = {sizeof(native.ticket), ASTERIA_FG_INPUT_API_V1};
        if (AsteriaQueryFrameInputTicketV1(registration, device, swapchain, &native.ticket) != 0 || native.ticket.frame_id != nextFrame)
            return false;
        const auto gpuDevice = reinterpret_cast<VkDevice>(uintptr_t(device));
        native.depth_image = asteria_vk::Bits(asteria_vk::Decode<VkImage>(input->depth.resource, gpuDevice, asteria_vk::HandleKind::Image));
        native.motion_image = asteria_vk::Bits(asteria_vk::Decode<VkImage>(input->motion.resource, gpuDevice, asteria_vk::HandleKind::Image));
        native.depth_format = ASTERIA_FG_FORMAT_R32_FLOAT; native.motion_format = ASTERIA_FG_FORMAT_RG16_FLOAT;
        native.image_layout = ASTERIA_FG_LAYOUT_SHADER_READ_ONLY; native.motion_source = ASTERIA_FG_MOTION_ESTIMATED;
        native.flags = ASTERIA_FG_DEPTH_SCENE | ASTERIA_FG_GPU_COMPLETED | ASTERIA_FG_HUD_ABSENT;
        if (input->historyReset) native.flags |= ASTERIA_FG_HISTORY_RESET;
        native.delta_milliseconds = input->deltaMilliseconds; native.near_plane = input->nearPlane; native.far_plane = input->farPlane;
        native.vertical_fov_radians = input->verticalFovRadians; native.meters_per_unit = input->metersPerUnit;
        std::copy_n(input->position, 3, native.position); std::copy_n(input->up, 3, native.up);
        std::copy_n(input->right, 3, native.right); std::copy_n(input->forward, 3, native.forward);
        native.motion_scale[0] = input->motionScale.x; native.motion_scale[1] = input->motionScale.y;
        auto *lease = new Owner;
        lease->resources = input->lifetime; lease->releases = releases;
        lease->state.owner_context = lease;
        lease->state.retain_owner = &RetainOwner; lease->state.release_owner = &ReleaseOwner;
        const int32_t result = submit(&AsteriaSubmitFrameInputsV1, &native, &lease->state);
        ReleaseOwner(lease);
        published = result == ASTERIA_FG_INPUT_OK;
        if (published) ++submissions;
        return published;
    }
    std::shared_ptr<const asteria_fg_runtime::FrameInputs> Acquire(uint64_t frame) override
    {
        if (!published || frame != nextFrame) return nullptr;
        session->BeginPresent(0);
        auto input = session->Acquire(frame);
        if (input) { ++nextFrame; published = false; }
        return input;
    }
    void RetireAfterGpuIdle() override {}
    uint32_t ReleasedLeases() const { return releases->load(); }
    uint32_t SubmittedLeases() const { return submissions; }
};
