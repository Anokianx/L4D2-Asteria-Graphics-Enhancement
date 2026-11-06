#pragma once
#include "AsteriaFrameInputApi.h"
#include "FrameGenerationInputs.h"
#include "AsteriaVkHandles.h"
#include <windows.h>
#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace asteria_fg_runtime::input_bridge
{
class Session;
inline std::mutex ResourceMutex;
inline std::set<std::pair<uint64_t, uint64_t>> LiveImages;

struct Registration
{
    const uint64_t id;
    const uint32_t capacity;
    std::atomic<bool> active{true};
    std::mutex mutex;
    uint32_t leases = 0;
    std::vector<std::weak_ptr<Session>> sessions;
    Registration(uint64_t identity, uint32_t limit) : id(identity), capacity(limit) {}
    bool Reserve(uint64_t device, uint64_t depth, uint64_t motion)
    {
        std::scoped_lock lock(mutex, ResourceMutex);
        if (!active.load() || leases >= capacity || LiveImages.count({device, depth}) || LiveImages.count({device, motion})) return false;
        LiveImages.emplace(device, depth);
        try { LiveImages.emplace(device, motion); }
        catch (...) { LiveImages.erase({device, depth}); throw; }
        ++leases;
        return true;
    }
    void Release(uint64_t device, uint64_t depth, uint64_t motion)
    {
        std::scoped_lock lock(mutex, ResourceMutex);
        LiveImages.erase({device, depth}); LiveImages.erase({device, motion}); --leases;
    }
};

struct Lease
{
    std::shared_ptr<Registration> registration;
    uint64_t device = 0, depth = 0, motion = 0;
    void *depthBox = nullptr, *motionBox = nullptr;
    void *context = nullptr;
    AsteriaFrameInputLeaseCallbackV1 release = nullptr;
    HMODULE retainModule = nullptr, releaseModule = nullptr;
    bool reserved = false, retained = false;
    ~Lease()
    {
        if (depthBox) asteria_vk::Retire(depthBox);
        if (motionBox) asteria_vk::Retire(motionBox);
        if (retained) { try { release(context); } catch (...) {} }
        if (reserved) registration->Release(device, depth, motion);
        if (releaseModule) FreeLibrary(releaseModule);
        if (retainModule) FreeLibrary(retainModule);
    }
};

inline std::atomic<uint64_t> NextGeneration{0};

inline bool TicketMatches(const AsteriaFrameInputTicketV1 &a, const AsteriaFrameInputTicketV1 &b)
{
    return a.struct_size == sizeof(a) && a.api_version == ASTERIA_FG_INPUT_API_V1 &&
        a.registration == b.registration && a.device == b.device && a.swapchain == b.swapchain &&
        a.generation == b.generation && a.frame_id == b.frame_id && a.width == b.width && a.height == b.height &&
        !a.reserved && a.image_index == b.image_index && a.acquire_id == b.acquire_id;
}

class Session final : public FrameInputSession
{
    std::shared_ptr<Registration> registration;
    std::mutex mutex;
    AsteriaFrameInputTicketV1 ticket{};
    uint64_t nativeSwapchain = 0;
    bool closed = false, submitting = false;
    bool ambiguousAcquire = false, presentQualified = false, presentInProgress = false;
    uint64_t acquireSequence = 0;
    std::map<uint32_t, uint64_t> acquisitions;
    std::shared_ptr<const FrameInputs> pending;
public:
    Session(std::shared_ptr<Registration> owner, uint64_t device, uint32_t width, uint32_t height) : registration(std::move(owner))
    {
        ticket = {sizeof(ticket), ASTERIA_FG_INPUT_API_V1, registration->id, device, 0, ++NextGeneration, 0, width, height};
    }
    void BindSwapchain(uint64_t swapchain) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!ticket.swapchain && !closed) ticket.swapchain = swapchain;
    }
    void BindNativeSwapchain(uint64_t swapchain)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!nativeSwapchain && !closed) nativeSwapchain = swapchain;
    }
    void RecordAcquire(uint32_t index) override
    {
        std::shared_ptr<const FrameInputs> retired;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed) return;
            if (!acquisitions.empty()) { ambiguousAcquire = true; retired.swap(pending); }
            acquisitions[index] = ++acquireSequence;
            ticket.image_index = index; ticket.acquire_id = acquireSequence;
        }
    }
    void BeginPresent(uint32_t index) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        presentInProgress = true;
        const auto acquired = acquisitions.find(index);
        presentQualified = !ambiguousAcquire && acquired != acquisitions.end() && acquisitions.size() == 1 &&
            ticket.image_index == index && ticket.acquire_id == acquired->second;
        if (acquired != acquisitions.end()) acquisitions.erase(acquired);
        if (acquisitions.empty()) ambiguousAcquire = false;
    }
    int32_t Query(uint64_t device, uint64_t swapchain, AsteriaFrameInputTicketV1 &out)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed || !registration->active.load() || !swapchain || ticket.device != device ||
            (ticket.swapchain != swapchain && nativeSwapchain != swapchain) || presentInProgress || ambiguousAcquire || acquisitions.size() != 1)
            return ASTERIA_FG_INPUT_UNAVAILABLE;
        out = ticket;
        return ASTERIA_FG_INPUT_OK;
    }
    std::shared_ptr<const FrameInputs> Acquire(uint64_t frame) override
    {
        std::shared_ptr<const FrameInputs> result;
        bool qualified = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed || frame != ticket.frame_id) return nullptr;
            ++ticket.frame_id;
            presentInProgress = false;
            result.swap(pending);
            qualified = presentQualified; presentQualified = false;
        }
        if (!qualified || !registration->active.load()) result.reset();
        return result;
    }
    void RetireAfterGpuIdle() override
    {
        std::shared_ptr<const FrameInputs> retired;
        {
            std::lock_guard<std::mutex> lock(mutex);
            closed = true; retired.swap(pending);
        }
    }
    int32_t Submit(const AsteriaFrameInputSubmissionV1 &input)
    {
        if (input.struct_size != sizeof(input) || input.api_version != ASTERIA_FG_INPUT_API_V1 || input.reserved ||
            input.depth_format != ASTERIA_FG_FORMAT_R32_FLOAT ||
            (input.motion_format != ASTERIA_FG_FORMAT_RG16_FLOAT && input.motion_format != ASTERIA_FG_FORMAT_RG32_FLOAT) ||
            input.image_layout != ASTERIA_FG_LAYOUT_SHADER_READ_ONLY || !input.depth_image || !input.motion_image ||
            input.depth_image == input.motion_image || !input.retain_lease || !input.release_lease ||
            (input.flags & ~(ASTERIA_FG_DEPTH_SCENE | ASTERIA_FG_GPU_COMPLETED | ASTERIA_FG_HUD_ABSENT | ASTERIA_FG_HISTORY_RESET)) ||
            (input.flags & 7u) != 7u) return ASTERIA_FG_INPUT_INVALID_ARGUMENT;
        auto frame = std::make_shared<FrameInputs>();
        auto lease = std::make_shared<Lease>();
        lease->registration = registration; lease->device = input.ticket.device;
        lease->depth = input.depth_image; lease->motion = input.motion_image;
        lease->context = input.lease_context; lease->release = input.release_lease;
        frame->lifetime = lease;
        frame->device = input.ticket.device; frame->frameID = input.ticket.frame_id;
        frame->width = input.ticket.width; frame->height = input.ticket.height;
        frame->motionSource = MotionInputSource(input.motion_source);
        frame->sceneDepth = true; frame->synchronized = true; frame->hudCompositionVerified = true;
        frame->historyReset = (input.flags & ASTERIA_FG_HISTORY_RESET) != 0;
        frame->deltaMilliseconds = input.delta_milliseconds;
        frame->nearPlane = input.near_plane; frame->farPlane = input.far_plane;
        frame->verticalFovRadians = input.vertical_fov_radians; frame->metersPerUnit = input.meters_per_unit;
        std::copy_n(input.position, 3, frame->position); std::copy_n(input.up, 3, frame->up);
        std::copy_n(input.right, 3, frame->right); std::copy_n(input.forward, 3, frame->forward);
        frame->motionScale = {input.motion_scale[0], input.motion_scale[1]};
        for (FfxResource *resource : {&frame->depth, &frame->motion})
        {
            resource->resource = lease.get();
            resource->description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
            resource->description.width = frame->width; resource->description.height = frame->height;
            resource->description.depth = resource->description.mipCount = 1;
            resource->state = FFX_RESOURCE_STATE_COMPUTE_READ;
            resource->description.usage = FFX_RESOURCE_USAGE_READ_ONLY;
        }
        frame->depth.description.format = FFX_SURFACE_FORMAT_R32_FLOAT;
        frame->motion.description.format = input.motion_format == ASTERIA_FG_FORMAT_RG16_FLOAT ?
            FFX_SURFACE_FORMAT_R16G16_FLOAT : FFX_SURFACE_FORMAT_R32G32_FLOAT;
        if (input.ticket.device > (std::numeric_limits<uintptr_t>::max)() ||
            !ValidFrameInputs(*frame, input.ticket.device, input.ticket.frame_id, input.ticket.width, input.ticket.height))
            return ASTERIA_FG_INPUT_INVALID_ARGUMENT;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed || !registration->active.load() || presentInProgress || ambiguousAcquire || acquisitions.size() != 1) return ASTERIA_FG_INPUT_UNAVAILABLE;
            if (!TicketMatches(input.ticket, ticket)) return ASTERIA_FG_INPUT_STALE_TICKET;
            if (pending || submitting) return ASTERIA_FG_INPUT_BUSY;
            submitting = true;
        }
        struct SubmissionGuard
        {
            Session &owner;
            ~SubmissionGuard() { std::lock_guard<std::mutex> lock(owner.mutex); owner.submitting = false; }
        } guard{*this};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(input.retain_lease), &lease->retainModule) ||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(input.release_lease), &lease->releaseModule))
            return ASTERIA_FG_INPUT_INVALID_ARGUMENT;
        if (!registration->Reserve(lease->device, lease->depth, lease->motion)) return ASTERIA_FG_INPUT_BUSY;
        lease->reserved = true;
        lease->retained = true;
        input.retain_lease(input.lease_context);
        const auto device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(frame->device));
        lease->depthBox = asteria_vk::StoreBorrowed(device, asteria_vk::HandleKind::Image, input.depth_image);
        lease->motionBox = asteria_vk::StoreBorrowed(device, asteria_vk::HandleKind::Image, input.motion_image);
        frame->depth.resource = lease->depthBox; frame->motion.resource = lease->motionBox;
        if (!lease->depthBox || !lease->motionBox) return ASTERIA_FG_INPUT_INTERNAL_ERROR;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed || !registration->active.load() || presentInProgress || ambiguousAcquire || acquisitions.size() != 1) return ASTERIA_FG_INPUT_UNAVAILABLE;
            if (!TicketMatches(input.ticket, ticket)) return ASTERIA_FG_INPUT_STALE_TICKET;
            pending = std::move(frame);
        }
        return ASTERIA_FG_INPUT_OK;
    }
};

class Provider final : public FrameInputProvider
{
public:
    const std::shared_ptr<Registration> registration;
    explicit Provider(std::shared_ptr<Registration> owner) : registration(std::move(owner)) {}
    std::shared_ptr<FrameInputSession> Open(uint64_t device, uint32_t width, uint32_t height) override
    {
        if (!device || !width || !height || device > (std::numeric_limits<uintptr_t>::max)()) return nullptr;
        auto session = std::make_shared<Session>(registration, device, width, height);
        std::lock_guard<std::mutex> lock(registration->mutex);
        if (!registration->active.load()) return nullptr;
        auto &sessions = registration->sessions;
        sessions.erase(std::remove_if(sessions.begin(), sessions.end(), [](const auto &value) { return value.expired(); }), sessions.end());
        if (sessions.size() >= 16) return nullptr;
        sessions.emplace_back(session);
        return session;
    }
};

inline std::mutex RegistryMutex;
inline uint64_t RegistrationSequence = 0;
inline std::shared_ptr<Provider> Registered;

inline std::shared_ptr<Provider> Find(uint64_t id)
{
    std::lock_guard<std::mutex> lock(RegistryMutex);
    return Registered && Registered->registration->id == id ? Registered : nullptr;
}

inline int32_t Register(const AsteriaFrameInputRegistrationV1 &info, uint64_t &id)
{
    if (info.struct_size != sizeof(info) || info.api_version != ASTERIA_FG_INPUT_API_V1 ||
        info.reserved || info.max_live_leases < 2 || info.max_live_leases > 8) return ASTERIA_FG_INPUT_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(RegistryMutex);
    if (Registered || CurrentFrameInputProvider()) return ASTERIA_FG_INPUT_BUSY;
    auto provider = std::make_shared<Provider>(std::make_shared<Registration>(++RegistrationSequence, info.max_live_leases));
    Registered = provider;
    std::atomic_store_explicit(&InputProvider, std::shared_ptr<FrameInputProvider>(provider), std::memory_order_release);
    id = provider->registration->id;
    return ASTERIA_FG_INPUT_OK;
}

inline std::vector<std::shared_ptr<Session>> Sessions(const std::shared_ptr<Provider> &provider)
{
    std::vector<std::shared_ptr<Session>> result;
    std::lock_guard<std::mutex> lock(provider->registration->mutex);
    for (auto &weak : provider->registration->sessions) if (auto session = weak.lock()) result.push_back(std::move(session));
    return result;
}

inline int32_t Unregister(uint64_t id)
{
    std::shared_ptr<Provider> provider;
    {
        std::lock_guard<std::mutex> lock(RegistryMutex);
        if (!Registered || Registered->registration->id != id) return ASTERIA_FG_INPUT_UNAVAILABLE;
        provider.swap(Registered);
        provider->registration->active.store(false);
        std::atomic_store_explicit(&InputProvider, std::shared_ptr<FrameInputProvider>{}, std::memory_order_release);
    }
    for (auto &session : Sessions(provider)) session->RetireAfterGpuIdle();
    return ASTERIA_FG_INPUT_OK;
}

inline int32_t Query(uint64_t id, uint64_t device, uint64_t swapchain, AsteriaFrameInputTicketV1 &ticket)
{
    const auto provider = Find(id);
    if (!provider) return ASTERIA_FG_INPUT_UNAVAILABLE;
    for (auto &session : Sessions(provider))
        if (session->Query(device, swapchain, ticket) == ASTERIA_FG_INPUT_OK) return ASTERIA_FG_INPUT_OK;
    return ASTERIA_FG_INPUT_UNAVAILABLE;
}

inline int32_t Submit(const AsteriaFrameInputSubmissionV1 &input)
{
    const auto provider = Find(input.ticket.registration);
    if (!provider) return ASTERIA_FG_INPUT_UNAVAILABLE;
    for (auto &session : Sessions(provider))
    {
        AsteriaFrameInputTicketV1 ticket{};
        if (session->Query(input.ticket.device, input.ticket.swapchain, ticket) == ASTERIA_FG_INPUT_OK)
            return session->Submit(input);
    }
    return ASTERIA_FG_INPUT_UNAVAILABLE;
}
}
