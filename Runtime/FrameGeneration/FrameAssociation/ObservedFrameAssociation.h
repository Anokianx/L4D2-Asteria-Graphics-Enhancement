#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace asteria_frame_association
{
inline constexpr const char *Author = "Anokianx";
using Matrix = std::array<float, 16>;
using Vector = std::array<double, 3>;

enum class HandleDomain : uint32_t { Unknown, ReShadeResource, VulkanImage };
enum class PassKind : uint32_t { SceneCandidate, Sky, Shadow, Viewmodel, Reflection, UI };
enum class Status : uint32_t
{
    NoDraws, AcceptedObservation, ExcludedPass, InvalidIdentity, UnverifiedSource,
    InvalidDimensions, UnsupportedViewport, InvalidMatrix, InvalidOrder,
    CoverageLoss, CapacityLoss, AmbiguousViews, InsufficientDraws, ProjectionPolicy,
    UnresolvedSky, WarmingUp, AssociatedObservation, IdentityChanged,
    CameraDiscontinuity, NativeFrameConflict, CommandReset, ResourceRetired,
    SceneChanged, InvalidPresent, InvalidPolicy, ExplicitReset,
    PresentTicketMissing, PresentTicketMismatch, PresentTicketStale, PresentTicketDuplicate,
    PresentTicketResize, PresentTicketForeign, PresentTicketCapacity
};

inline const char *Name(Status value)
{
    switch (value)
    {
    case Status::NoDraws: return "no_draws";
    case Status::AcceptedObservation: return "accepted_observation";
    case Status::ExcludedPass: return "excluded_pass";
    case Status::InvalidIdentity: return "invalid_identity";
    case Status::UnverifiedSource: return "unverified_source";
    case Status::InvalidDimensions: return "invalid_dimensions";
    case Status::UnsupportedViewport: return "unsupported_viewport";
    case Status::InvalidMatrix: return "invalid_matrix";
    case Status::InvalidOrder: return "invalid_observer_order";
    case Status::CoverageLoss: return "coverage_loss";
    case Status::CapacityLoss: return "capacity_loss";
    case Status::AmbiguousViews: return "ambiguous_views";
    case Status::InsufficientDraws: return "insufficient_draws";
    case Status::ProjectionPolicy: return "projection_outside_policy";
    case Status::UnresolvedSky: return "sky_relationship_excluded";
    case Status::WarmingUp: return "warming_up";
    case Status::AssociatedObservation: return "associated_observation";
    case Status::IdentityChanged: return "target_identity_changed";
    case Status::CameraDiscontinuity: return "camera_discontinuity";
    case Status::NativeFrameConflict: return "native_frame_identity_conflict";
    case Status::CommandReset: return "unclosed_command_reset";
    case Status::ResourceRetired: return "resource_retired";
    case Status::SceneChanged: return "scene_changed";
    case Status::InvalidPresent: return "invalid_present_observation";
    case Status::InvalidPolicy: return "invalid_policy";
    case Status::PresentTicketMissing: return "present_ticket_missing";
    case Status::PresentTicketMismatch: return "present_ticket_mismatch";
    case Status::PresentTicketStale: return "present_ticket_stale";
    case Status::PresentTicketDuplicate: return "present_ticket_duplicate";
    case Status::PresentTicketResize: return "present_ticket_resize";
    case Status::PresentTicketForeign: return "present_ticket_foreign";
    case Status::PresentTicketCapacity: return "present_ticket_capacity";
    default: return "explicit_reset";
    }
}

struct ResourceIdentity
{
    uint64_t device = 0, handle = 0, generation = 0;
    HandleDomain domain = HandleDomain::Unknown;
    uint32_t width = 0, height = 0, samples = 0, format = 0;
    bool operator==(const ResourceIdentity &other) const
    {
        return std::tie(device, handle, generation, domain, width, height, samples, format) ==
               std::tie(other.device, other.handle, other.generation, other.domain,
                        other.width, other.height, other.samples, other.format);
    }
    bool operator<(const ResourceIdentity &other) const
    {
        return std::tie(device, handle, generation, domain, width, height, samples, format) <
               std::tie(other.device, other.handle, other.generation, other.domain,
                        other.width, other.height, other.samples, other.format);
    }
};

struct DrawIdentity
{
    uint64_t command = 0, commandGeneration = 0, ordinal = 0, recordingSequence = 0;
    uint64_t recordingTickNanoseconds = 0;
};

struct PresentTicketEvidence
{
    uint64_t device = 0, owner = 0, command = 0, swapchain = 0, swapchainGeneration = 0;
    uint64_t imageIndex = 0, acquireSerial = 0, submitSerial = 0;
    bool verified = false;
};

inline bool PresentTicketHasFields(const PresentTicketEvidence &ticket)
{
    return ticket.device || ticket.owner || ticket.command || ticket.swapchain || ticket.swapchainGeneration ||
           ticket.imageIndex || ticket.acquireSerial || ticket.submitSerial;
}

inline bool ValidPresentTicket(const PresentTicketEvidence &ticket, uint64_t device)
{
    return ticket.verified && ticket.device == device && ticket.owner && ticket.swapchain && ticket.swapchainGeneration &&
           ticket.imageIndex < 64 && ticket.acquireSerial && ticket.submitSerial >= ticket.acquireSerial;
}

inline bool SamePresentTicketCore(const PresentTicketEvidence &a, const PresentTicketEvidence &b)
{
    return a.verified && b.verified && a.device == b.device && a.owner == b.owner && a.swapchain == b.swapchain &&
           a.swapchainGeneration == b.swapchainGeneration && a.imageIndex == b.imageIndex &&
           a.acquireSerial == b.acquireSerial && a.submitSerial == b.submitSerial;
}

using PresentTicketKey = std::tuple<uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t>;

inline PresentTicketKey MakePresentTicketKey(const PresentTicketEvidence &ticket)
{
    return {ticket.device, ticket.owner, ticket.command, ticket.swapchain, ticket.swapchainGeneration,
            ticket.imageIndex, ticket.acquireSerial, ticket.submitSerial};
}

struct DrawObservation
{
    uint64_t device = 0, scene = 0, observerIntervalId = 0;
    uint64_t nativeEngineFrameId = 0;
    bool nativeEngineFrameIdAvailable = false;
    DrawIdentity draw;
    ResourceIdentity depth, color;
    uint32_t viewportX = 0, viewportY = 0, viewportWidth = 0, viewportHeight = 0;
    uint64_t pipeline = 0, pipelineGeneration = 0;
    std::string nativeShaderSha256;
    Matrix worldToClip{};
    PassKind pass = PassKind::SceneCandidate;
    bool nativeBindingVerified = false, stableMatrixCopy = false;
    bool directPrimaryRecording = false, observedOutsideEffects = false;
    bool depthAttachment = false, colorAttachment = false;
    PresentTicketEvidence presentTicket;
};

struct PresentObservation
{
    uint64_t device = 0, scene = 0;
    uint64_t closedObserverIntervalId = 0, presentObserverId = 0;
    uint64_t tickNanoseconds = 0;
    uint32_t width = 0, height = 0;
    bool recordingCoverageComplete = false, sceneCut = false;
    PresentTicketEvidence presentTicket;
};

struct SkyReference
{
    Vector origin{};
    double scale = 0;
};

struct Policy
{
    uint32_t stableIntervals = 3, minimumDraws = 3;
    size_t maxObservations = 1024, maxGroups = 32;
    double minimumMainNear = 4, maximumMainNear = 32;
    double maximumOriginStep = 128, minimumBasisCosine = 0.5;
    double maximumFovStepRadians = 0.15;
    uint64_t maximumObservationAgeNanoseconds = 1000000000ull;
    std::vector<SkyReference> skyReferences;
};

struct Camera
{
    Matrix worldToClip{};
    std::array<double, 16> worldToView{}, projection{};
    Vector origin{}, right{}, up{}, backward{};
    double nearPlane = 0, farPlane = 0, verticalFovRadians = 0, aspect = 0;
};

inline double Dot(const Vector &a, const Vector &b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline Vector Cross(const Vector &a, const Vector &b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

inline bool RecoverNativePerspective(const Matrix &matrix, Camera &result)
{
    result = {};
    for (float value : matrix) if (!std::isfinite(value)) return false;
    const Vector rowX{matrix[0], matrix[1], matrix[2]}, rowY{matrix[4], matrix[5], matrix[6]};
    const Vector rowZ{matrix[8], matrix[9], matrix[10]}, rowW{matrix[12], matrix[13], matrix[14]};
    const double sx = std::sqrt(Dot(rowX, rowX)), sy = std::sqrt(Dot(rowY, rowY));
    if (sx <= 1e-7 || sy <= 1e-7 || std::abs(Dot(rowW, rowW) - 1) > 1e-4) return false;
    for (size_t index = 0; index < 3; ++index)
    {
        result.right[index] = rowX[index] / sx;
        result.up[index] = rowY[index] / sy;
        result.backward[index] = -rowW[index];
    }
    if (std::abs(Dot(result.right, result.up)) > 1e-4 || std::abs(Dot(result.right, result.backward)) > 1e-4 ||
        std::abs(Dot(result.up, result.backward)) > 1e-4 ||
        std::abs(Dot(result.right, Cross(result.up, result.backward)) - 1) > 1e-4) return false;
    const double a = Dot(rowZ, result.backward) / Dot(result.backward, result.backward);
    const double tx = matrix[3] / sx, ty = matrix[7] / sy, tz = -matrix[15];
    const double b = matrix[11] - a * tz;
    if (!(a < -1 && b < 0)) return false;
    result.nearPlane = b / a;
    result.farPlane = b / (a + 1);
    if (!std::isfinite(result.nearPlane) || !std::isfinite(result.farPlane) ||
        !(result.nearPlane > 0 && result.farPlane > result.nearPlane)) return false;
    for (size_t index = 0; index < 3; ++index)
    {
        result.worldToView[index] = result.right[index];
        result.worldToView[4 + index] = result.up[index];
        result.worldToView[8 + index] = result.backward[index];
        result.origin[index] = -(result.right[index] * tx + result.up[index] * ty + result.backward[index] * tz);
    }
    result.worldToView[3] = tx; result.worldToView[7] = ty; result.worldToView[11] = tz;
    result.worldToView[15] = 1;
    result.projection[0] = sx; result.projection[5] = sy;
    result.projection[10] = a; result.projection[11] = b; result.projection[14] = -1;
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
        {
            double rebuilt = 0;
            for (size_t k = 0; k < 4; ++k) rebuilt += result.projection[row * 4 + k] * result.worldToView[k * 4 + column];
            const double value = matrix[row * 4 + column];
            if (!std::isfinite(rebuilt) || std::abs(value - rebuilt) > 1e-5 + 2e-6 * std::max(std::abs(value), std::abs(rebuilt))) return false;
        }
    result.worldToClip = matrix;
    result.verticalFovRadians = 2 * std::atan(1 / sy);
    result.aspect = sy / sx;
    return true;
}

struct Snapshot
{
    ResourceIdentity depth, color;
    Camera camera;
    uint64_t device = 0, scene = 0, observerIntervalId = 0, presentObserverId = 0;
    uint64_t nativeEngineFrameId = 0;
    bool nativeEngineFrameIdAvailable = false;
    DrawIdentity firstDraw, lastDraw;
    uint64_t firstPipeline = 0, firstPipelineGeneration = 0, lastPipeline = 0, lastPipelineGeneration = 0;
    std::string firstNativeShaderSha256, lastNativeShaderSha256;
    uint64_t observedDraws = 0, presentTickNanoseconds = 0;
    PresentTicketEvidence presentTicket;
};

struct Association
{
    Status status = Status::NoDraws;
    Snapshot previous, current;
    bool observedAssociationAvailable = false, resetHistory = true;
    bool observerIntervalOnly = true, gpuFrameAligned = false;
    bool mainViewVerified = false, nativeCameraReady = false, gameFgReady = false;
    uint32_t stableIntervals = 0;
    uint64_t acceptedDraws = 0, excludedDraws = 0, rejectedDraws = 0, skyRelationsExcluded = 0;
    bool presentTicketVerified = false;
    PresentTicketEvidence presentTicket;
};

class Collector
{
    struct Group
    {
        Snapshot snapshot;
        bool nativeFrameMissing = false;
    };
    Policy policy;
    uint64_t owner = 0, scene = 0, interval = 0, lastSequence = 0, lastTick = 0;
    bool intervalStarted = false, previousAvailable = false;
    bool policyValid = false;
    uint32_t stable = 0;
    Status poison = Status::NoDraws;
    Snapshot previous;
    std::vector<Group> groups;
    std::set<std::string> nativeProfiles;
    std::set<std::tuple<uint64_t, uint64_t, uint64_t>> drawIdentities;
    std::map<uint64_t, uint64_t> commandGenerations;
    PresentTicketEvidence intervalTicket;
    bool intervalTicketSet = false, intervalUnticketed = false;
    std::set<uint64_t> intervalTicketCommands;
    PresentTicketEvidence lastTicket;
    bool lastTicketAvailable = false;
    std::set<PresentTicketKey> closedTickets;
    uint64_t accepted = 0, excluded = 0, rejected = 0;
    mutable std::mutex mutex;

    static bool ValidResource(const ResourceIdentity &resource, uint64_t device)
    {
        return resource.device == device && resource.handle && resource.generation && resource.width && resource.height &&
               resource.samples && resource.samples <= 64 && !(resource.samples & (resource.samples - 1)) && resource.format &&
               (resource.domain == HandleDomain::ReShadeResource || resource.domain == HandleDomain::VulkanImage);
    }

    static bool SameCamera(const Camera &a, const Camera &b)
    {
        return a.worldToClip == b.worldToClip;
    }

    static bool SameTarget(const Snapshot &a, const Snapshot &b)
    {
        return a.device == b.device && a.scene == b.scene && a.depth == b.depth && a.color == b.color;
    }

    bool Discontinuous(const Snapshot &a, const Snapshot &b) const
    {
        Vector delta{};
        for (size_t index = 0; index < 3; ++index) delta[index] = a.camera.origin[index] - b.camera.origin[index];
        return std::sqrt(Dot(delta, delta)) > policy.maximumOriginStep ||
               Dot(a.camera.backward, b.camera.backward) < policy.minimumBasisCosine ||
               Dot(a.camera.up, b.camera.up) < policy.minimumBasisCosine ||
               std::abs(a.camera.verticalFovRadians - b.camera.verticalFovRadians) > policy.maximumFovStepRadians;
    }

    bool SkyRelation(const Camera &sky, const Camera &main) const
    {
        if (std::abs(sky.nearPlane - 2) > 0.01 || std::abs(sky.verticalFovRadians - main.verticalFovRadians) > 1e-4 ||
            Dot(sky.backward, main.backward) < 0.999999 || Dot(sky.up, main.up) < 0.999999) return false;
        for (const auto &reference : policy.skyReferences)
        {
            bool match = std::isfinite(reference.scale) && reference.scale > 1;
            for (size_t index = 0; index < 3; ++index)
                match &= std::isfinite(reference.origin[index]) &&
                         std::abs(sky.origin[index] - main.origin[index] / reference.scale - reference.origin[index]) <= 0.02;
            if (match) return true;
        }
        return false;
    }

    Status Reject(Status reason, bool affectsInterval = true)
    {
        ++rejected;
        if (affectsInterval && poison == Status::NoDraws) poison = reason;
        return reason;
    }

    void ClearInterval()
    {
        intervalStarted = false; groups.clear(); drawIdentities.clear(); commandGenerations.clear();
        intervalTicket = {}; intervalTicketSet = intervalUnticketed = false; intervalTicketCommands.clear();
        poison = Status::NoDraws; accepted = excluded = rejected = 0;
    }

    Association Fail(Association result, Status reason)
    {
        result.status = reason; result.resetHistory = true;
        previousAvailable = false; previous = {}; stable = 0;
        ClearInterval();
        return result;
    }

public:
    explicit Collector(uint64_t device, Policy inputPolicy = {}) : policy(std::move(inputPolicy)), owner(device)
    {
        policy.stableIntervals = std::max(2u, policy.stableIntervals);
        policy.minimumDraws = std::max(1u, policy.minimumDraws);
        policy.maxObservations = std::clamp(policy.maxObservations, size_t(1), size_t(4096));
        policy.maxGroups = std::clamp(policy.maxGroups, size_t(1), size_t(64));
        policyValid = std::isfinite(policy.minimumMainNear) && std::isfinite(policy.maximumMainNear) &&
                      policy.minimumMainNear > 0 && policy.maximumMainNear >= policy.minimumMainNear &&
                      std::isfinite(policy.maximumOriginStep) && policy.maximumOriginStep > 0 &&
                      std::isfinite(policy.minimumBasisCosine) && policy.minimumBasisCosine >= -1 && policy.minimumBasisCosine <= 1 &&
                      std::isfinite(policy.maximumFovStepRadians) && policy.maximumFovStepRadians > 0 &&
                      policy.maximumObservationAgeNanoseconds > 0;
    }

    bool RegisterNativeProfile(const std::string &sha256)
    {
        if (sha256.size() != 64 || !std::all_of(sha256.begin(), sha256.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return false;
        const std::lock_guard<std::mutex> lock(mutex);
        if (nativeProfiles.size() >= 256) return false;
        nativeProfiles.insert(sha256);
        return true;
    }

    Status ObserveDraw(const DrawObservation &draw)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!policyValid) return Reject(Status::InvalidPolicy);
        if (!owner || draw.device != owner || !draw.scene || !draw.draw.command || !draw.draw.commandGeneration ||
            !draw.draw.ordinal || !draw.draw.recordingSequence || !draw.draw.recordingTickNanoseconds)
            return Reject(Status::InvalidIdentity);
        if (!intervalStarted) { intervalStarted = true; interval = draw.observerIntervalId; scene = draw.scene; }
        if (draw.observerIntervalId != interval || draw.scene != scene || draw.draw.recordingSequence <= lastSequence ||
            draw.draw.recordingTickNanoseconds < lastTick) return Reject(Status::InvalidOrder);
        lastSequence = draw.draw.recordingSequence; lastTick = draw.draw.recordingTickNanoseconds;
        if (draw.pass == PassKind::Sky || draw.pass == PassKind::Shadow || draw.pass == PassKind::Viewmodel ||
            draw.pass == PassKind::Reflection || draw.pass == PassKind::UI) { ++excluded; return Status::ExcludedPass; }
        if (draw.pass != PassKind::SceneCandidate) return Reject(Status::UnverifiedSource);
        if (!draw.pipeline || !draw.pipelineGeneration || !ValidResource(draw.depth, owner) || !ValidResource(draw.color, owner))
            return Reject(Status::InvalidIdentity);
        if (!draw.nativeBindingVerified || !draw.stableMatrixCopy || !draw.directPrimaryRecording || !draw.observedOutsideEffects ||
            !nativeProfiles.count(draw.nativeShaderSha256)) return Reject(Status::UnverifiedSource);
        if (!draw.depthAttachment || !draw.colorAttachment || draw.color.domain != draw.depth.domain ||
            draw.color.width != draw.depth.width || draw.color.height != draw.depth.height || draw.color.samples != draw.depth.samples)
            return Reject(Status::InvalidDimensions);
        if (draw.viewportX || draw.viewportY || draw.viewportWidth != draw.color.width || draw.viewportHeight != draw.color.height)
            return Reject(Status::UnsupportedViewport);
        if (draw.presentTicket.verified || PresentTicketHasFields(draw.presentTicket))
        {
            if (!ValidPresentTicket(draw.presentTicket, owner)) return Reject(Status::PresentTicketForeign);
            if (draw.presentTicket.command && draw.presentTicket.command != draw.draw.command)
                return Reject(Status::PresentTicketMismatch);
            if (intervalUnticketed) return Reject(Status::PresentTicketMissing);
            if (!intervalTicketSet) intervalTicket = draw.presentTicket, intervalTicketSet = true;
            else if (!SamePresentTicketCore(intervalTicket, draw.presentTicket)) return Reject(Status::PresentTicketMismatch);
            if (draw.presentTicket.command && !intervalTicketCommands.count(draw.presentTicket.command))
            {
                if (intervalTicketCommands.size() >= 64) return Reject(Status::PresentTicketCapacity);
                intervalTicketCommands.insert(draw.presentTicket.command);
            }
        }
        else
        {
            if (intervalTicketSet) return Reject(Status::PresentTicketMissing);
            intervalUnticketed = true;
        }
        const auto command = commandGenerations.find(draw.draw.command);
        if (command != commandGenerations.end() && command->second != draw.draw.commandGeneration) return Reject(Status::CommandReset);
        commandGenerations[draw.draw.command] = draw.draw.commandGeneration;
        if (drawIdentities.size() >= policy.maxObservations) return Reject(Status::CapacityLoss);
        if (!drawIdentities.emplace(draw.draw.command, draw.draw.commandGeneration, draw.draw.ordinal).second) return Reject(Status::InvalidIdentity);
        Camera camera;
        if (!RecoverNativePerspective(draw.worldToClip, camera)) return Reject(Status::InvalidMatrix);
        if (std::abs(camera.aspect - double(draw.color.width) / draw.color.height) > 0.005) return Reject(Status::InvalidDimensions);
        auto group = std::find_if(groups.begin(), groups.end(), [&](const Group &entry) {
            return entry.snapshot.depth == draw.depth && entry.snapshot.color == draw.color && SameCamera(entry.snapshot.camera, camera);
        });
        if (group == groups.end())
        {
            if (groups.size() >= policy.maxGroups) return Reject(Status::CapacityLoss);
            Group entry;
            entry.snapshot.device = owner; entry.snapshot.scene = scene; entry.snapshot.observerIntervalId = interval;
            entry.snapshot.depth = draw.depth; entry.snapshot.color = draw.color; entry.snapshot.camera = camera;
            entry.snapshot.nativeEngineFrameIdAvailable = draw.nativeEngineFrameIdAvailable;
            entry.snapshot.nativeEngineFrameId = draw.nativeEngineFrameIdAvailable ? draw.nativeEngineFrameId : 0;
            entry.nativeFrameMissing = !draw.nativeEngineFrameIdAvailable;
            entry.snapshot.firstDraw = draw.draw;
            entry.snapshot.firstPipeline = draw.pipeline; entry.snapshot.firstPipelineGeneration = draw.pipelineGeneration;
            entry.snapshot.firstNativeShaderSha256 = draw.nativeShaderSha256;
            entry.snapshot.presentTicket = draw.presentTicket;
            groups.push_back(std::move(entry)); group = groups.end() - 1;
        }
        if (group->snapshot.nativeEngineFrameIdAvailable && draw.nativeEngineFrameIdAvailable &&
            group->snapshot.nativeEngineFrameId != draw.nativeEngineFrameId) return Reject(Status::NativeFrameConflict);
        if (!draw.nativeEngineFrameIdAvailable) group->nativeFrameMissing = true;
        group->snapshot.nativeEngineFrameIdAvailable &= !group->nativeFrameMissing;
        if (!group->snapshot.nativeEngineFrameIdAvailable) group->snapshot.nativeEngineFrameId = 0;
        group->snapshot.lastDraw = draw.draw; ++group->snapshot.observedDraws; ++accepted;
        group->snapshot.lastPipeline = draw.pipeline; group->snapshot.lastPipelineGeneration = draw.pipelineGeneration;
        group->snapshot.lastNativeShaderSha256 = draw.nativeShaderSha256;
        return Status::AcceptedObservation;
    }

    Association OnPresent(const PresentObservation &present)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        Association result;
        result.acceptedDraws = accepted; result.excludedDraws = excluded; result.rejectedDraws = rejected;
        if (!policyValid) return Fail(result, Status::InvalidPolicy);
        if (present.device != owner || !present.scene || !present.width || !present.height || !present.tickNanoseconds ||
            present.closedObserverIntervalId == UINT64_MAX || present.presentObserverId != present.closedObserverIntervalId + 1)
            return Fail(result, Status::InvalidPresent);
        if (present.sceneCut || (previousAvailable && present.scene != previous.scene)) return Fail(result, Status::SceneChanged);
        if (previousAvailable && (present.closedObserverIntervalId <= previous.observerIntervalId ||
            present.tickNanoseconds <= previous.presentTickNanoseconds)) return Fail(result, Status::InvalidOrder);
        if (!present.recordingCoverageComplete) return Fail(result, Status::CoverageLoss);
        if (poison != Status::NoDraws) return Fail(result, poison);
        if (!intervalStarted || groups.empty()) return Fail(result, Status::NoDraws);
        const bool hasPresentTicket = present.presentTicket.verified || PresentTicketHasFields(present.presentTicket);
        if (hasPresentTicket)
        {
            if (!ValidPresentTicket(present.presentTicket, owner)) return Fail(result, Status::PresentTicketForeign);
            if (!intervalTicketSet || intervalUnticketed) return Fail(result, Status::PresentTicketMissing);
            if (!SamePresentTicketCore(intervalTicket, present.presentTicket)) return Fail(result, Status::PresentTicketMismatch);
            if (present.presentTicket.command && !intervalTicketCommands.count(present.presentTicket.command))
                return Fail(result, Status::PresentTicketMismatch);
            const auto key = MakePresentTicketKey(present.presentTicket);
            if (closedTickets.count(key)) return Fail(result, Status::PresentTicketDuplicate);
            if (lastTicketAvailable)
            {
                if (present.presentTicket.swapchain != lastTicket.swapchain) return Fail(result, Status::PresentTicketForeign);
                if (present.presentTicket.swapchainGeneration != lastTicket.swapchainGeneration)
                    return Fail(result, present.presentTicket.swapchainGeneration > lastTicket.swapchainGeneration ?
                        Status::PresentTicketResize : Status::PresentTicketStale);
                if (present.presentTicket.acquireSerial <= lastTicket.acquireSerial ||
                    present.presentTicket.submitSerial <= lastTicket.submitSerial) return Fail(result, Status::PresentTicketStale);
            }
            if (closedTickets.size() >= 256) return Fail(result, Status::PresentTicketCapacity);
            closedTickets.insert(key);
            lastTicket = present.presentTicket; lastTicketAvailable = true;
        }
        else if (intervalTicketSet) return Fail(result, Status::PresentTicketMissing);
        if (interval != present.closedObserverIntervalId || scene != present.scene || present.tickNanoseconds < lastTick ||
            present.tickNanoseconds - lastTick > policy.maximumObservationAgeNanoseconds)
            return Fail(result, Status::InvalidOrder);
        std::vector<size_t> candidates;
        bool projectionRejected = false;
        for (size_t index = 0; index < groups.size(); ++index)
        {
            auto &candidate = groups[index].snapshot;
            if (present.tickNanoseconds < candidate.firstDraw.recordingTickNanoseconds ||
                present.tickNanoseconds - candidate.firstDraw.recordingTickNanoseconds > policy.maximumObservationAgeNanoseconds)
                return Fail(result, Status::InvalidOrder);
            if (candidate.color.width != present.width || candidate.color.height != present.height) continue;
            bool sky = false;
            for (size_t other = 0; other < groups.size(); ++other)
                if (index != other && SameTarget(candidate, groups[other].snapshot) &&
                    SkyRelation(candidate.camera, groups[other].snapshot.camera)) { sky = true; break; }
            if (sky) { ++result.skyRelationsExcluded; continue; }
            if (candidate.camera.nearPlane < policy.minimumMainNear || candidate.camera.nearPlane > policy.maximumMainNear)
            { projectionRejected = true; continue; }
            candidates.push_back(index);
        }
        if (candidates.empty()) return Fail(result, projectionRejected ? Status::ProjectionPolicy : Status::UnresolvedSky);
        if (candidates.size() != 1) return Fail(result, Status::AmbiguousViews);
        auto current = groups[candidates[0]].snapshot;
        if (current.observedDraws < policy.minimumDraws) return Fail(result, Status::InsufficientDraws);
        current.presentObserverId = present.presentObserverId; current.presentTickNanoseconds = present.tickNanoseconds;
        current.presentTicket = present.presentTicket;
        result.current = current;
        result.presentTicketVerified = hasPresentTicket;
        result.presentTicket = present.presentTicket;
        const bool sameTarget = previousAvailable && SameTarget(previous, current);
        const bool consecutive = previousAvailable && previous.observerIntervalId != UINT64_MAX &&
                                 current.observerIntervalId == previous.observerIntervalId + 1 &&
                                 current.presentTickNanoseconds > previous.presentTickNanoseconds;
        const bool cameraCut = sameTarget && consecutive && Discontinuous(previous, current);
        if (sameTarget && consecutive && !cameraCut) ++stable;
        else stable = 1;
        result.stableIntervals = stable;
        result.status = previousAvailable && !sameTarget ? Status::IdentityChanged :
                        cameraCut ? Status::CameraDiscontinuity : Status::WarmingUp;
        if (sameTarget && consecutive && !cameraCut && stable >= policy.stableIntervals)
        {
            result.previous = previous;
            result.observedAssociationAvailable = true;
            result.resetHistory = false;
            result.status = Status::AssociatedObservation;
        }
        previous = current; previousAvailable = true;
        ClearInterval();
        return result;
    }

    void MarkCoverageLoss()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        poison = Status::CoverageLoss;
    }

    void CommandReset(uint64_t command, uint64_t generation)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto found = commandGenerations.find(command);
        if (found != commandGenerations.end() && found->second == generation) poison = Status::CommandReset;
    }

    void RetireResource(const ResourceIdentity &resource)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        bool used = previousAvailable && (previous.depth == resource || previous.color == resource);
        for (const auto &group : groups) used |= group.snapshot.depth == resource || group.snapshot.color == resource;
        if (used) { poison = Status::ResourceRetired; previousAvailable = false; previous = {}; stable = 0; }
    }

    void Reset()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        ClearInterval(); previousAvailable = false; previous = {}; stable = 0;
        lastSequence = lastTick = 0; lastTicket = {}; lastTicketAvailable = false; closedTickets.clear(); poison = Status::ExplicitReset;
    }
};
}
