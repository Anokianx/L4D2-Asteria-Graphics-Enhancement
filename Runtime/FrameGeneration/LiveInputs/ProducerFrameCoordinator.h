#pragma once
#include "../Layer/AsteriaFrameInputApi.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace asteria_live_inputs
{
enum class ProducerLayerOrder { Unknown, ReShadeBeforeFrameGeneration, FrameGenerationBeforeReShade };
enum class ProducerPhase { Unknown, TechniqueRecording, AfterReShadeSubmitBeforeInputConsume, InputConsumeStarted };
enum class CompletionKind { Unknown, Fence, TimelineSemaphore, QueueIdle };
enum class ConsumerAbortDisposition { Unknown, NativeSubmissionFailed, AbandonedBeforeNativeSubmission };
enum class ConsumerDrainKind { Unknown, QueueIdle, DeviceIdle };
enum class NativeDispatchRole { Unknown, Producer, Consumer };
inline constexpr int32_t NoConsumerNativeSubmissionResult = INT32_C(2147483647);
inline constexpr uint32_t UnknownQueueFamily = UINT32_MAX;
struct NativeWaitObservation
{
    uint64_t semaphore = 0, value = 0, stageMask = 0;
};
struct NativeSignalObservation
{
    uint64_t semaphore = 0, value = 0;
};
struct NativeSyncIdentity
{
    bool metadataPresent = false;
    uint64_t submitBatchId = 0, ownershipTransferEpoch = 0;
    uint32_t submitBatchIndex = 0, submitBatchCount = 0, queueFamilyIndex = UnknownQueueFamily;
};
inline bool SameNativeSyncIdentity(const NativeSyncIdentity &a, const NativeSyncIdentity &b)
{
    return a.metadataPresent == b.metadataPresent && a.submitBatchId == b.submitBatchId && a.submitBatchIndex == b.submitBatchIndex &&
        a.submitBatchCount == b.submitBatchCount && a.queueFamilyIndex == b.queueFamilyIndex && a.ownershipTransferEpoch == b.ownershipTransferEpoch;
}
enum class CoordinatorResult
{
    Accepted, RuntimeClosed, InvalidScope, StaleTicket, CapacityExceeded, AmbiguousAcquire, AcquisitionUnavailable,
    UnsupportedLayerOrder, WrongPhase, InvalidCapture, DuplicateCapture, DuplicateSubmission,
    SubmissionMismatch, NativeSubmissionFailed, ApplicationWaitsUnobserved, ApplicationWaitMissing,
    InvalidCompletionSignal, CompletionMismatch, NativeCompletionFailed, DuplicateCompletion,
    CaptureNotCompleted, SemanticsUnverified, PresentAlreadyStarted, DuplicatePublication,
    PublicationUnavailable, ConsumerNotStarted, ConsumerCompletionMismatch, NativeConsumerCompletionFailed,
    ConsumerSubmissionMismatch, NativeConsumerSubmissionFailed, DuplicateConsumerSubmission,
    ConsumerAbortUnavailable, ConsumerAbortMismatch, DuplicateConsumerAbort, ConsumerAttemptConflict,
    ConsumerAbortDrainMismatch, NativeConsumerDrainFailed,
    NativeWorkUnproven, NativeDeviceDrainMismatch, NativeDeviceDrainFailed, ConsumerSubmissionAfterRetirement, ProducerSubmissionAfterRetirement,
    AlreadyRetired, RecordingAlreadySubmitted, RecordingDiscardUnverified,
    NativeDispatchPending, NativeDispatchIntentMismatch, NativeDispatchResultMismatch, NativeDispatchTokenOccupied,
    InvalidNativeDispatchToken, NativeDispatchAlreadyResolved, NativeDispatchResultInProgress, NativeDispatchUnresolved,
    NativeSyncMetadataInvalid, NativeSyncMetadataMismatch
};
enum class ProducerFrameState { RecordedCapture, NativeSubmissionObserved, CompletedCapture, Published, ConsumerInFlight, Retired };
inline const char *ProducerFrameStateName(ProducerFrameState state)
{
    switch (state)
    {
    case ProducerFrameState::RecordedCapture: return "recorded_capture";
    case ProducerFrameState::NativeSubmissionObserved: return "native_submission_observed";
    case ProducerFrameState::CompletedCapture: return "completed_capture";
    case ProducerFrameState::Published: return "published";
    case ProducerFrameState::ConsumerInFlight: return "consumer_in_flight";
    case ProducerFrameState::Retired: return "retired";
    }
    return "unknown";
}

struct ProducerScope
{
    uint64_t registration = 0, device = 0, proxySwapchain = 0, generation = 0, graphicsQueue = 0;
    uint32_t width = 0, height = 0, maximumFrames = 4, graphicsQueueFamily = UnknownQueueFamily;
    uint64_t ownershipTransferEpoch = 0;
};
struct CaptureObservation
{
    AsteriaFrameInputTicketV1 ticket{};
    uint64_t queue = 0, command = 0, allocationGeneration = 0, recordingGeneration = 0;
    uint32_t outstandingAcquires = 0;
    ProducerLayerOrder layerOrder = ProducerLayerOrder::Unknown;
    ProducerPhase phase = ProducerPhase::Unknown;
};
struct NativeSubmissionObservation
{
    AsteriaFrameInputTicketV1 ticket{};
    uint64_t nativeDevice = 0, queue = 0, command = 0, allocationGeneration = 0, recordingGeneration = 0;
    uint64_t submitSerial = 0, applicationPresentObservationSerial = 0;
    int32_t nativeResult = -1;
    ProducerLayerOrder layerOrder = ProducerLayerOrder::Unknown;
    ProducerPhase phase = ProducerPhase::Unknown;
    bool applicationWaitSetObserved = false;
    std::vector<uint64_t> applicationWaitSemaphores, submittedWaitSemaphores;
    CompletionKind completionKind = CompletionKind::Unknown;
    uint64_t completionObject = 0, completionValue = 0;
    NativeSyncIdentity sync{};
    std::array<NativeWaitObservation, 64> applicationWaitDetails{};
    std::array<NativeWaitObservation, 64> submittedWaitDetails{};
    std::array<NativeSignalObservation, 64> signalDetails{};
    uint32_t applicationWaitDetailCount = 0, submittedWaitDetailCount = 0, signalDetailCount = 0;
};
struct NativeCompletionObservation
{
    AsteriaFrameInputTicketV1 ticket{};
    uint64_t nativeDevice = 0, queue = 0, command = 0, allocationGeneration = 0, recordingGeneration = 0;
    uint64_t submitSerial = 0, completionObservationSerial = 0;
    CompletionKind completionKind = CompletionKind::Unknown;
    uint64_t completionObject = 0, completedValue = 0;
    int32_t nativeResult = -1;
    NativeSyncIdentity sync{};
};
struct ProducerInputSemantics
{
    bool mainViewVerified = false, sceneDepthVerified = false, cameraVerified = false;
    bool hudAbsentVerified = false, motionVerified = false, matchingColorStreamVerified = false;
    bool sceneEpochVerified = false, pipelineStateVerified = false;
};
struct ConsumerCompletionObservation
{
    AsteriaFrameInputTicketV1 ticket{};
    uint64_t nativeDevice = 0, queue = 0, command = 0, allocationGeneration = 0, recordingGeneration = 0;
    uint64_t submitSerial = 0, completionObservationSerial = 0, leaseReleaseObservationSerial = 0;
    CompletionKind completionKind = CompletionKind::Unknown;
    uint64_t completionObject = 0, completedValue = 0;
    int32_t nativeResult = -1;
    NativeSyncIdentity sync{};
};
struct ConsumerSubmissionObservation
{
    AsteriaFrameInputTicketV1 ticket{};
    uint64_t nativeDevice = 0, queue = 0, command = 0, allocationGeneration = 0, recordingGeneration = 0, submitSerial = 0;
    CompletionKind completionKind = CompletionKind::Unknown;
    uint64_t completionObject = 0, completionValue = 0;
    int32_t nativeResult = -1;
    NativeSyncIdentity sync{};
    std::array<NativeSignalObservation, 64> signalDetails{};
    uint32_t signalDetailCount = 0;
};
struct ConsumerAbortObservation
{
    AsteriaFrameInputTicketV1 ticket{};
    uint64_t queue = 0, command = 0, allocationGeneration = 0, recordingGeneration = 0, attemptSerial = 0;
    ConsumerAbortDisposition disposition = ConsumerAbortDisposition::Unknown;
    ProducerPhase phase = ProducerPhase::Unknown;
    int32_t nativeResult = NoConsumerNativeSubmissionResult;
};
struct ConsumerAbortDrainObservation
{
    AsteriaFrameInputTicketV1 ticket{};
    uint64_t queue = 0, command = 0, allocationGeneration = 0, recordingGeneration = 0, attemptSerial = 0;
    ConsumerAbortDisposition disposition = ConsumerAbortDisposition::Unknown;
    ConsumerDrainKind drainKind = ConsumerDrainKind::Unknown;
    uint64_t drainObject = 0, drainObservationSerial = 0, leaseReleaseObservationSerial = 0;
    int32_t nativeDrainResult = -1;
};
struct NativeDeviceDrainObservation
{
    AsteriaFrameInputTicketV1 ticket{};
    uint64_t nativeDevice = 0, deviceIdleObservationSerial = 0, leaseReleaseObservationSerial = 0;
    int32_t nativeResult = -1;
};
struct NativeDispatchIntent
{
    AsteriaFrameInputTicketV1 ticket{};
    NativeDispatchRole role = NativeDispatchRole::Unknown;
    uint64_t nativeDevice = 0, queue = 0, command = 0, allocationGeneration = 0, recordingGeneration = 0, submitSerial = 0;
    CompletionKind completionKind = CompletionKind::Unknown;
    uint64_t completionObject = 0, completionValue = 0;
    NativeSyncIdentity sync{};
};
struct RecordingDiscardObservation
{
    uint64_t command = 0, allocationGeneration = 0, recordingGeneration = 0, observationSerial = 0;
    int32_t nativeResetResult = -1;
};
struct UnconsumedReleaseObservation
{
    AsteriaFrameInputTicketV1 ticket{};
    uint64_t leaseReleaseObservationSerial = 0;
    bool providerConfirmedNeverAcquired = false;
};
struct ProducerFrameSnapshot
{
    ProducerFrameState state = ProducerFrameState::RecordedCapture;
    CoordinatorResult lastResult = CoordinatorResult::Accepted;
    bool runtimeClosed = false, invalidated = false, presentBegun = false, nativeSubmissionObserved = false;
    bool submissionQualified = false, gpuCompletionObserved = false, completedCapture = false, captureReady = false;
    bool producerSubmissionQualified = false, published = false, consumerSubmissionObserved = false, consumerCompleted = false;
    bool consumerAbortObserved = false, consumerAbortDrained = false;
    bool producerNativeWorkHazard = false, consumerNativeWorkHazard = false, nativeDeviceDrainObserved = false;
    bool nativeDeviceDrainProofPossible = true, lateConsumerSuccessObserved = false, lateProducerSuccessObserved = false;
    bool successfulNativeWorkFactsOverflow = false;
    uint32_t successfulNativeWorkFacts = 0;
    uint32_t pendingNativeDispatches = 0, abandonedNativeDispatches = 0;
    bool ownerRetained = false, drainRequired = false, retired = false;
    uint64_t acquireId = 0, submitSerial = 0, submittedQueue = 0, latestNativeReturnSerial = 0;
};
using ProducerRetireCallback = void (*)(void *) noexcept;

inline bool SameProducerTicket(const AsteriaFrameInputTicketV1 &a, const AsteriaFrameInputTicketV1 &b)
{
    return a.struct_size == sizeof(a) && b.struct_size == sizeof(b) && a.api_version == ASTERIA_FG_INPUT_API_V1 &&
        b.api_version == ASTERIA_FG_INPUT_API_V1 && !a.reserved && !b.reserved && a.registration == b.registration &&
        a.device == b.device && a.swapchain == b.swapchain && a.generation == b.generation && a.frame_id == b.frame_id &&
        a.width == b.width && a.height == b.height && a.image_index == b.image_index && a.acquire_id == b.acquire_id;
}

class ProducerFrameCoordinator
{
    struct Gate
    {
        const ProducerScope scope;
        std::atomic<bool> closed{false}, ambiguous{false};
        std::mutex mutex;
        uint64_t lastAcquire = 0, lastFrame = 0, lastSubmitSerial = 0;
        bool hasCapture = false;
        explicit Gate(const ProducerScope &value) : scope(value) {}
    };
    struct Retention
    {
        std::shared_ptr<void> owner;
        ProducerRetireCallback callback = nullptr;
        void *context = nullptr;
        ~Retention() { if (callback) callback(context); }
    };
    struct Frame
    {
        enum class DispatchState { Empty, Pending, Resolving, Resolved, Abandoned };
        struct Dispatch
        {
            NativeDispatchIntent intent;
            uint64_t id = 0;
            DispatchState state = DispatchState::Empty;
        };
        struct SuccessfulWork
        {
            bool consumer = false;
            uint64_t nativeDevice = 0, queue = 0, command = 0, allocationGeneration = 0, recordingGeneration = 0, submitSerial = 0;
            CompletionKind completionKind = CompletionKind::Unknown;
            uint64_t completionObject = 0, completionValue = 0;
            NativeSyncIdentity sync{};
        };
        const std::shared_ptr<Gate> gate;
        const CaptureObservation capture;
        std::mutex mutex;
        std::shared_ptr<Frame> pendingLifetime;
        std::shared_ptr<Retention> retention;
        NativeSubmissionObservation submission;
        ConsumerSubmissionObservation consumerSubmission;
        ConsumerAbortObservation consumerAbort;
        CoordinatorResult last = CoordinatorResult::Accepted;
        bool invalidated = false, presentBegun = false, submitted = false, submissionQualified = false;
        bool gpuCompleted = false, published = false, consumerSubmitted = false, consumerCompleted = false, retired = false;
        bool consumerAbortObserved = false, consumerAbortDrained = false;
        bool producerNativeWorkHazard = false, consumerNativeWorkHazard = false, nativeDeviceDrainObserved = false;
        bool allNativeDevicesKnown = true, allNativeSerialsKnown = true, lateConsumerSuccessObserved = false, lateProducerSuccessObserved = false;
        bool successfulNativeWorkFactsOverflow = false;
        std::array<SuccessfulWork, 16> successfulWork{};
        uint32_t successfulWorkCount = 0, producerSuccessCount = 0, consumerSuccessCount = 0;
        uint64_t lastNativeSuccessSerial = 0, lastNativeAttemptSerial = 0;
        bool consumerAttemptObserved = false;
        uint64_t captureCompletionSerial = 0;
        std::array<Dispatch, 16> dispatches{};
        uint32_t pendingDispatches = 0, abandonedDispatches = 0;
        uint64_t nextDispatchId = 0, lastDispatchSerial = 0, lastNativeReturnSerial = 0;
        explicit Frame(std::shared_ptr<Gate> owner, const CaptureObservation &value) : gate(std::move(owner)), capture(value) {}
    };
    std::shared_ptr<Gate> gate;
    mutable std::mutex framesMutex;
    std::map<uint64_t, std::weak_ptr<Frame>> frames;

    static bool SignalValid(CompletionKind kind, uint64_t object, uint64_t value, uint64_t queue)
    {
        if (kind == CompletionKind::QueueIdle) return object == queue && object && value == 0;
        if (kind == CompletionKind::Fence) return object && value == 0;
        return kind == CompletionKind::TimelineSemaphore && object && value;
    }
    static bool WaitsValid(const NativeSubmissionObservation &value)
    {
        if (value.applicationWaitSemaphores.size() > 64 || value.submittedWaitSemaphores.size() > 64) return false;
        std::set<uint64_t> expected, actual;
        for (uint64_t wait : value.applicationWaitSemaphores) if (!wait || !expected.insert(wait).second) return false;
        for (uint64_t wait : value.submittedWaitSemaphores) if (!wait || !actual.insert(wait).second) return false;
        return std::includes(actual.begin(), actual.end(), expected.begin(), expected.end());
    }
    static bool SyncIdentityValid(const NativeSyncIdentity &value, const ProducerScope &scope)
    {
        if (!value.metadataPresent) return true;
        if (!value.submitBatchId || !value.submitBatchCount || value.submitBatchCount > 16 || value.submitBatchIndex >= value.submitBatchCount ||
            value.queueFamilyIndex == UnknownQueueFamily) return false;
        if (scope.graphicsQueueFamily != UnknownQueueFamily && value.queueFamilyIndex != scope.graphicsQueueFamily) return false;
        if (scope.ownershipTransferEpoch && value.ownershipTransferEpoch != scope.ownershipTransferEpoch) return false;
        return true;
    }
    static bool WaitDetailsValid(const std::array<NativeWaitObservation, 64> &details, uint32_t count)
    {
        if (count > details.size()) return false;
        std::set<uint64_t> seen;
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto &item = details[index];
            if (!item.semaphore || !item.stageMask || !seen.insert(item.semaphore).second) return false;
        }
        return true;
    }
    static bool SignalDetailsValid(const std::array<NativeSignalObservation, 64> &details, uint32_t count)
    {
        if (count > details.size()) return false;
        std::set<uint64_t> seen;
        for (uint32_t index = 0; index < count; ++index)
            if (!details[index].semaphore || !seen.insert(details[index].semaphore).second) return false;
        return true;
    }
    static bool WaitDetailsCover(const std::array<NativeWaitObservation, 64> &submitted, uint32_t submittedCount,
                                 const std::vector<uint64_t> &expected)
    {
        if (submittedCount > submitted.size() || expected.size() > 64) return false;
        for (uint64_t semaphore : expected)
        {
            bool found = false;
            for (uint32_t index = 0; index < submittedCount; ++index)
                if (submitted[index].semaphore == semaphore) { found = true; break; }
            if (!found) return false;
        }
        return true;
    }
    static bool StrictSyncValid(const NativeSubmissionObservation &value, const ProducerScope &scope)
    {
        if (!value.sync.metadataPresent) return true;
        if (!SyncIdentityValid(value.sync, scope) || value.applicationWaitDetailCount != value.applicationWaitSemaphores.size() ||
            value.submittedWaitDetailCount != value.submittedWaitSemaphores.size() || !WaitDetailsValid(value.applicationWaitDetails, value.applicationWaitDetailCount) ||
            !WaitDetailsValid(value.submittedWaitDetails, value.submittedWaitDetailCount) || !SignalDetailsValid(value.signalDetails, value.signalDetailCount) ||
            !WaitDetailsCover(value.submittedWaitDetails, value.submittedWaitDetailCount, value.applicationWaitSemaphores)) return false;
        for (uint32_t index = 0; index < value.applicationWaitDetailCount; ++index)
        {
            const auto &expected = value.applicationWaitDetails[index];
            bool found = false;
            for (uint32_t inner = 0; inner < value.submittedWaitDetailCount; ++inner)
                if (value.submittedWaitDetails[inner].semaphore == expected.semaphore && value.submittedWaitDetails[inner].value == expected.value &&
                    value.submittedWaitDetails[inner].stageMask == expected.stageMask) { found = true; break; }
            if (!found) return false;
        }
        return true;
    }
    static bool StrictConsumerSyncValid(const ConsumerSubmissionObservation &value, const ProducerScope &scope)
    {
        return !value.sync.metadataPresent || (SyncIdentityValid(value.sync, scope) && SignalDetailsValid(value.signalDetails, value.signalDetailCount));
    }
    static bool SemanticsValid(const ProducerInputSemantics &value)
    {
        return value.mainViewVerified && value.sceneDepthVerified && value.cameraVerified && value.hudAbsentVerified &&
            value.motionVerified && value.matchingColorStreamVerified && value.sceneEpochVerified && value.pipelineStateVerified;
    }
    template <typename Observation>
    static void RecordAttemptLocked(Frame &frame, const Observation &value, bool consumer)
    {
        frame.lastNativeAttemptSerial = (std::max)(frame.lastNativeAttemptSerial, value.submitSerial);
        frame.allNativeDevicesKnown &= value.nativeDevice == frame.gate->scope.device && value.nativeDevice != 0;
        frame.allNativeSerialsKnown &= value.submitSerial != 0;
        if (consumer) frame.consumerAttemptObserved = true;
        if (value.nativeResult != 0)
        {
            auto &hazard = consumer ? frame.consumerNativeWorkHazard : frame.producerNativeWorkHazard;
            hazard |= (consumer ? frame.consumerSubmitted : frame.submitted) || value.nativeDevice != frame.gate->scope.device ||
                value.queue != frame.gate->scope.graphicsQueue || !value.submitSerial;
        }
    }
    template <typename Observation>
    static bool RecordSuccessfulWorkLocked(Frame &frame, const Observation &value, bool consumer)
    {
        using Work = Frame::SuccessfulWork;
        const Work work{consumer, value.nativeDevice, value.queue, value.command, value.allocationGeneration,
            value.recordingGeneration, value.submitSerial, value.completionKind, value.completionObject, value.completionValue, value.sync};
        frame.allNativeDevicesKnown &= value.nativeDevice == frame.gate->scope.device && value.nativeDevice != 0;
        frame.allNativeSerialsKnown &= value.submitSerial != 0;
        frame.lastNativeSuccessSerial = (std::max)(frame.lastNativeSuccessSerial, value.submitSerial);
        for (uint32_t index = 0; index < frame.successfulWorkCount; ++index)
        {
            const auto &old = frame.successfulWork[index];
            if (old.consumer == work.consumer && old.nativeDevice == work.nativeDevice && old.queue == work.queue &&
                old.command == work.command && old.allocationGeneration == work.allocationGeneration &&
                old.recordingGeneration == work.recordingGeneration && old.submitSerial == work.submitSerial &&
                old.completionKind == work.completionKind && old.completionObject == work.completionObject && old.completionValue == work.completionValue &&
                SameNativeSyncIdentity(old.sync, work.sync))
                return false;
        }
        auto &count = consumer ? frame.consumerSuccessCount : frame.producerSuccessCount;
        auto &hazard = consumer ? frame.consumerNativeWorkHazard : frame.producerNativeWorkHazard;
        ++count;
        hazard |= count > 1 || !value.nativeDevice || value.nativeDevice != frame.gate->scope.device || !value.queue ||
            !value.command || !value.allocationGeneration || !value.recordingGeneration || !value.submitSerial ||
            (consumer && value.submitSerial <= frame.captureCompletionSerial) ||
            !SignalValid(value.completionKind, value.completionObject, value.completionValue, value.queue);
        if (frame.successfulWorkCount < frame.successfulWork.size()) frame.successfulWork[frame.successfulWorkCount++] = work;
        else { hazard = true; frame.successfulNativeWorkFactsOverflow = true; }
        return true;
    }
    static bool RetireLocked(Frame &frame, std::shared_ptr<Retention> &retired, std::shared_ptr<Frame> &unpin)
    {
        if (frame.pendingDispatches) return false;
        frame.retired = true;
        retired = std::move(frame.retention);
        unpin = std::move(frame.pendingLifetime);
        return true;
    }
public:
    class NativeDispatchToken;
    class FrameHandle
    {
        friend class ProducerFrameCoordinator;
        friend class NativeDispatchToken;
        std::shared_ptr<Frame> frame;
        explicit FrameHandle(std::shared_ptr<Frame> value) : frame(std::move(value)) {}
        CoordinatorResult ResultLocked(CoordinatorResult value) const { frame->last = value; return value; }
    public:
        FrameHandle() = default;
        explicit operator bool() const { return bool(frame); }
        CoordinatorResult BeginNativeDispatch(const NativeDispatchIntent &intent, NativeDispatchToken &output) const;
        ProducerFrameSnapshot Snapshot() const
        {
            ProducerFrameSnapshot value;
            if (!frame) { value.retired = true; value.lastResult = CoordinatorResult::InvalidCapture; return value; }
            std::lock_guard<std::mutex> lock(frame->mutex);
            value.lastResult = frame->last; value.runtimeClosed = frame->gate->closed.load(); value.invalidated = frame->invalidated;
            value.presentBegun = frame->presentBegun; value.nativeSubmissionObserved = frame->submitted;
            value.submissionQualified = frame->submissionQualified; value.gpuCompletionObserved = frame->gpuCompleted;
            value.completedCapture = frame->submitted && frame->submissionQualified && frame->gpuCompleted && !frame->producerNativeWorkHazard && !frame->pendingDispatches;
            value.captureReady = value.completedCapture && !frame->retired && !frame->invalidated && !frame->gate->closed.load() && !frame->consumerAttemptObserved;
            value.producerSubmissionQualified = value.captureReady && frame->published && !frame->presentBegun &&
                !frame->gate->ambiguous.load() && !frame->consumerSubmitted;
            value.published = frame->published;
            value.consumerSubmissionObserved = frame->consumerSubmitted;
            value.consumerAbortObserved = frame->consumerAbortObserved; value.consumerAbortDrained = frame->consumerAbortDrained;
            value.producerNativeWorkHazard = frame->producerNativeWorkHazard; value.consumerNativeWorkHazard = frame->consumerNativeWorkHazard;
            value.nativeDeviceDrainObserved = frame->nativeDeviceDrainObserved;
            value.nativeDeviceDrainProofPossible = frame->allNativeDevicesKnown && frame->allNativeSerialsKnown && !frame->pendingDispatches;
            value.lateConsumerSuccessObserved = frame->lateConsumerSuccessObserved; value.successfulNativeWorkFacts = frame->successfulWorkCount;
            value.lateProducerSuccessObserved = frame->lateProducerSuccessObserved;
            value.successfulNativeWorkFactsOverflow = frame->successfulNativeWorkFactsOverflow;
            value.pendingNativeDispatches = frame->pendingDispatches; value.abandonedNativeDispatches = frame->abandonedDispatches;
            value.consumerCompleted = frame->consumerCompleted; value.ownerRetained = bool(frame->retention);
            value.retired = frame->retired; value.drainRequired = !frame->retired;
            value.acquireId = frame->capture.ticket.acquire_id; value.submitSerial = frame->submission.submitSerial;
            value.submittedQueue = frame->submission.queue;
            value.latestNativeReturnSerial = frame->lastNativeReturnSerial;
            value.state = frame->retired ? ProducerFrameState::Retired : frame->published ?
                (frame->presentBegun ? ProducerFrameState::ConsumerInFlight : ProducerFrameState::Published) :
                value.completedCapture ? ProducerFrameState::CompletedCapture : frame->submitted ?
                ProducerFrameState::NativeSubmissionObserved : ProducerFrameState::RecordedCapture;
            return value;
        }
        CoordinatorResult ObserveNativeSubmission(const NativeSubmissionObservation &value, bool drainOnly = false) const
        {
            if (!frame) return CoordinatorResult::InvalidCapture;
            std::lock_guard<std::mutex> serialLock(frame->gate->mutex);
            std::lock_guard<std::mutex> lock(frame->mutex);
            if (frame->retired)
            {
                if (SameProducerTicket(value.ticket, frame->capture.ticket) && value.nativeResult == 0)
                {
                    RecordSuccessfulWorkLocked(*frame, value, false);
                    frame->lateProducerSuccessObserved = true; frame->producerNativeWorkHazard = true;
                    return ResultLocked(CoordinatorResult::ProducerSubmissionAfterRetirement);
                }
                return ResultLocked(CoordinatorResult::AlreadyRetired);
            }
            if (!SameProducerTicket(value.ticket, frame->capture.ticket)) return ResultLocked(CoordinatorResult::SubmissionMismatch);
            const uint64_t previousAttempt = frame->lastNativeAttemptSerial;
            RecordAttemptLocked(*frame, value, false);
            if (!StrictSyncValid(value, frame->gate->scope))
            {
                frame->producerNativeWorkHazard = true;
                return ResultLocked(CoordinatorResult::NativeSyncMetadataInvalid);
            }
            if (value.nativeResult != 0) return ResultLocked(CoordinatorResult::NativeSubmissionFailed);
            const bool alreadySubmitted = frame->submitted;
            const bool newWork = RecordSuccessfulWorkLocked(*frame, value, false);
            frame->submitted = true;
            if (!newWork) return ResultLocked(CoordinatorResult::DuplicateSubmission);
            if (value.submitSerial <= previousAttempt) frame->producerNativeWorkHazard = true;
            if (alreadySubmitted)
            {
                frame->producerNativeWorkHazard = true; frame->submissionQualified = false;
                return ResultLocked(CoordinatorResult::NativeWorkUnproven);
            }
            const bool previousHazard = frame->producerNativeWorkHazard;
            frame->producerNativeWorkHazard = true;
            frame->submission = value;
            frame->producerNativeWorkHazard = previousHazard || !value.nativeDevice || value.nativeDevice != frame->gate->scope.device || !value.queue ||
                !value.command || !value.allocationGeneration || !value.recordingGeneration || !value.submitSerial ||
                value.submitSerial <= frame->gate->lastSubmitSerial ||
                !SignalValid(value.completionKind, value.completionObject, value.completionValue, value.queue);
            CoordinatorResult qualification = CoordinatorResult::Accepted;
            if (drainOnly || frame->gate->closed.load()) qualification = CoordinatorResult::RuntimeClosed;
            else if (frame->presentBegun) qualification = CoordinatorResult::PresentAlreadyStarted;
            else if (frame->invalidated || frame->gate->ambiguous.load()) qualification = CoordinatorResult::AmbiguousAcquire;
            else if (value.nativeDevice != frame->gate->scope.device || value.queue != frame->capture.queue || value.queue != frame->gate->scope.graphicsQueue ||
                     value.command != frame->capture.command || value.allocationGeneration != frame->capture.allocationGeneration ||
                     value.recordingGeneration != frame->capture.recordingGeneration || !value.submitSerial || value.submitSerial <= frame->gate->lastSubmitSerial)
                qualification = CoordinatorResult::SubmissionMismatch;
            else if (value.layerOrder != ProducerLayerOrder::ReShadeBeforeFrameGeneration) qualification = CoordinatorResult::UnsupportedLayerOrder;
            else if (value.phase != ProducerPhase::AfterReShadeSubmitBeforeInputConsume) qualification = CoordinatorResult::WrongPhase;
            else if (!value.applicationWaitSetObserved || !value.applicationPresentObservationSerial ||
                     value.applicationPresentObservationSerial >= value.submitSerial) qualification = CoordinatorResult::ApplicationWaitsUnobserved;
            else if (!WaitsValid(value)) qualification = CoordinatorResult::ApplicationWaitMissing;
            else if (!SignalValid(value.completionKind, value.completionObject, value.completionValue, value.queue)) qualification = CoordinatorResult::InvalidCompletionSignal;
            frame->gate->lastSubmitSerial = (std::max)(frame->gate->lastSubmitSerial, value.submitSerial);
            frame->submissionQualified = qualification == CoordinatorResult::Accepted;
            return ResultLocked(qualification);
        }
        CoordinatorResult ObserveGpuCompletion(const NativeCompletionObservation &value, bool drainOnly = false) const
        {
            if (!frame) return CoordinatorResult::InvalidCapture;
            std::shared_ptr<Retention> retired;
            std::shared_ptr<Frame> unpin;
            CoordinatorResult result;
            {
                std::lock_guard<std::mutex> lock(frame->mutex);
                if (frame->retired) return ResultLocked(CoordinatorResult::AlreadyRetired);
                if (frame->pendingDispatches) return ResultLocked(CoordinatorResult::NativeDispatchPending);
                if (frame->gpuCompleted) return ResultLocked(CoordinatorResult::DuplicateCompletion);
                const auto &submitted = frame->submission;
                if (!frame->submitted || !SameProducerTicket(value.ticket, frame->capture.ticket) ||
                    !value.nativeDevice || value.nativeDevice != submitted.nativeDevice || value.nativeDevice != frame->gate->scope.device ||
                    value.queue != submitted.queue || value.command != submitted.command || value.allocationGeneration != submitted.allocationGeneration ||
                    value.recordingGeneration != submitted.recordingGeneration || value.submitSerial != submitted.submitSerial ||
                    value.completionObservationSerial <= (std::max)(submitted.submitSerial, frame->lastNativeReturnSerial))
                    return ResultLocked(CoordinatorResult::CompletionMismatch);
                if (submitted.sync.metadataPresent && !SameNativeSyncIdentity(value.sync, submitted.sync))
                    return ResultLocked(CoordinatorResult::NativeSyncMetadataMismatch);
                const bool exactSignal = value.completionKind == submitted.completionKind && value.completionObject == submitted.completionObject &&
                    value.completedValue >= submitted.completionValue && SignalValid(value.completionKind, value.completionObject, submitted.completionValue, value.queue);
                const bool queueDrain = drainOnly && value.completionKind == CompletionKind::QueueIdle && value.completedValue == 0 &&
                    SignalValid(value.completionKind, value.completionObject, 0, value.queue);
                if (!exactSignal && !queueDrain) return ResultLocked(CoordinatorResult::CompletionMismatch);
                if (value.nativeResult != 0) return ResultLocked(CoordinatorResult::NativeCompletionFailed);
                frame->gpuCompleted = true;
                frame->captureCompletionSerial = value.completionObservationSerial;
                if (drainOnly) { frame->invalidated = true; frame->submissionQualified = false; }
                if (frame->gate->closed.load() && !frame->published && !frame->consumerAttemptObserved && !frame->producerNativeWorkHazard)
                    RetireLocked(*frame, retired, unpin);
                result = ResultLocked(CoordinatorResult::Accepted);
            }
            return result;
        }
        CoordinatorResult QualifyAndPublish(const ProducerInputSemantics &semantics) const
        {
            if (!frame) return CoordinatorResult::InvalidCapture;
            std::lock_guard<std::mutex> lock(frame->mutex);
            if (frame->retired) return ResultLocked(CoordinatorResult::AlreadyRetired);
                if (frame->pendingDispatches) return ResultLocked(CoordinatorResult::NativeDispatchPending);
            if (frame->gate->closed.load()) return ResultLocked(CoordinatorResult::RuntimeClosed);
            if (frame->presentBegun) return ResultLocked(CoordinatorResult::PresentAlreadyStarted);
            if (frame->published) return ResultLocked(CoordinatorResult::DuplicatePublication);
            if (frame->producerNativeWorkHazard || frame->consumerAttemptObserved) return ResultLocked(CoordinatorResult::NativeWorkUnproven);
            if (frame->invalidated || frame->gate->ambiguous.load()) return ResultLocked(CoordinatorResult::AmbiguousAcquire);
            if (!frame->submitted || !frame->submissionQualified || !frame->gpuCompleted) return ResultLocked(CoordinatorResult::CaptureNotCompleted);
            if (!SemanticsValid(semantics)) return ResultLocked(CoordinatorResult::SemanticsUnverified);
            frame->published = true;
            return ResultLocked(CoordinatorResult::Accepted);
        }
        CoordinatorResult BeginPresent(const AsteriaFrameInputTicketV1 &ticket) const
        {
            if (!frame) return CoordinatorResult::InvalidCapture;
            std::lock_guard<std::mutex> lock(frame->mutex);
            if (frame->retired) return ResultLocked(CoordinatorResult::AlreadyRetired);
            if (!SameProducerTicket(ticket, frame->capture.ticket)) return ResultLocked(CoordinatorResult::StaleTicket);
            if (frame->presentBegun) return ResultLocked(CoordinatorResult::PresentAlreadyStarted);
            frame->presentBegun = true;
            if (frame->pendingDispatches) return ResultLocked(CoordinatorResult::NativeDispatchPending);
            if (frame->gate->closed.load()) return ResultLocked(CoordinatorResult::RuntimeClosed);
            if (frame->producerNativeWorkHazard || frame->consumerNativeWorkHazard || frame->consumerAttemptObserved || frame->invalidated)
                return ResultLocked(CoordinatorResult::NativeWorkUnproven);
            if (frame->gate->ambiguous.load()) return ResultLocked(CoordinatorResult::AmbiguousAcquire);
            return ResultLocked(frame->published ? CoordinatorResult::Accepted : CoordinatorResult::PublicationUnavailable);
        }
        CoordinatorResult ObserveConsumerSubmission(const ConsumerSubmissionObservation &value, bool drainOnly = false) const
        {
            if (!frame) return CoordinatorResult::InvalidCapture;
            std::lock_guard<std::mutex> lock(frame->mutex);
            if (frame->retired)
            {
                if (SameProducerTicket(value.ticket, frame->capture.ticket) && value.nativeResult == 0)
                {
                    RecordSuccessfulWorkLocked(*frame, value, true);
                    frame->lateConsumerSuccessObserved = true; frame->consumerNativeWorkHazard = true;
                    return ResultLocked(CoordinatorResult::ConsumerSubmissionAfterRetirement);
                }
                return ResultLocked(CoordinatorResult::AlreadyRetired);
            }
            if (!SameProducerTicket(value.ticket, frame->capture.ticket)) return ResultLocked(CoordinatorResult::ConsumerSubmissionMismatch);
            if (!StrictConsumerSyncValid(value, frame->gate->scope))
            {
                frame->consumerNativeWorkHazard = true;
                return ResultLocked(CoordinatorResult::NativeSyncMetadataInvalid);
            }
            const uint64_t previousAttempt = frame->lastNativeAttemptSerial;
            RecordAttemptLocked(*frame, value, true);
            if (value.nativeResult != 0) return ResultLocked(CoordinatorResult::NativeConsumerSubmissionFailed);
            const bool alreadySubmitted = frame->consumerSubmitted;
            const bool newWork = RecordSuccessfulWorkLocked(*frame, value, true);
            frame->consumerSubmitted = true;
            if (!newWork) return ResultLocked(CoordinatorResult::DuplicateConsumerSubmission);
            if (value.submitSerial <= previousAttempt) frame->consumerNativeWorkHazard = true;
            if (alreadySubmitted) return ResultLocked(CoordinatorResult::NativeWorkUnproven);
            frame->consumerSubmission = value;
            if (!frame->published || !frame->presentBegun) return ResultLocked(CoordinatorResult::ConsumerNotStarted);
            if (frame->gate->closed.load() && !drainOnly) return ResultLocked(CoordinatorResult::RuntimeClosed);
            if (value.nativeDevice != frame->gate->scope.device || value.queue != frame->gate->scope.graphicsQueue ||
                !value.command || !value.allocationGeneration || !value.recordingGeneration || value.submitSerial <= frame->captureCompletionSerial ||
                !SignalValid(value.completionKind, value.completionObject, value.completionValue, value.queue))
                return ResultLocked(CoordinatorResult::ConsumerSubmissionMismatch);
            if (frame->consumerAbortObserved) return ResultLocked(CoordinatorResult::ConsumerAttemptConflict);
            return ResultLocked(CoordinatorResult::Accepted);
        }
        CoordinatorResult ObserveConsumerAbort(const ConsumerAbortObservation &value) const
        {
            if (!frame) return CoordinatorResult::InvalidCapture;
            std::lock_guard<std::mutex> lock(frame->mutex);
            if (frame->retired) return ResultLocked(CoordinatorResult::AlreadyRetired);
            if (!frame->published || !frame->presentBegun) return ResultLocked(CoordinatorResult::ConsumerAbortUnavailable);
            if (frame->consumerSubmitted) return ResultLocked(CoordinatorResult::ConsumerAttemptConflict);
            if (!SameProducerTicket(value.ticket, frame->capture.ticket) || value.queue != frame->gate->scope.graphicsQueue ||
                value.attemptSerial <= frame->captureCompletionSerial)
                return ResultLocked(CoordinatorResult::ConsumerAbortMismatch);
            if (value.phase != ProducerPhase::InputConsumeStarted) return ResultLocked(CoordinatorResult::WrongPhase);
            const bool completeCommand = value.command && value.allocationGeneration && value.recordingGeneration;
            const bool noCommand = !value.command && !value.allocationGeneration && !value.recordingGeneration;
            const bool failed = value.disposition == ConsumerAbortDisposition::NativeSubmissionFailed && value.nativeResult < 0 && completeCommand;
            const bool abandoned = value.disposition == ConsumerAbortDisposition::AbandonedBeforeNativeSubmission &&
                value.nativeResult == NoConsumerNativeSubmissionResult && (completeCommand || noCommand);
            if (!failed && !abandoned) return ResultLocked(CoordinatorResult::ConsumerAbortMismatch);
            if (frame->consumerAbortObserved)
                return ResultLocked(value.attemptSerial == frame->consumerAbort.attemptSerial ?
                    CoordinatorResult::DuplicateConsumerAbort : CoordinatorResult::ConsumerAttemptConflict);
            frame->consumerAbort = value; frame->consumerAbortObserved = true;
            return ResultLocked(CoordinatorResult::Accepted);
        }
        CoordinatorResult RetireAbortedConsumer(const ConsumerAbortDrainObservation &value) const
        {
            if (!frame) return CoordinatorResult::InvalidCapture;
            std::shared_ptr<Retention> retired;
            std::shared_ptr<Frame> unpin;
            CoordinatorResult result;
            {
                std::lock_guard<std::mutex> lock(frame->mutex);
                if (frame->retired) return ResultLocked(CoordinatorResult::AlreadyRetired);
                if (frame->pendingDispatches) return ResultLocked(CoordinatorResult::NativeDispatchPending);
                if (!frame->published || !frame->presentBegun || !frame->consumerAbortObserved)
                    return ResultLocked(CoordinatorResult::ConsumerAbortUnavailable);
                if (frame->consumerSubmitted) return ResultLocked(CoordinatorResult::ConsumerAttemptConflict);
                if (frame->producerNativeWorkHazard || frame->consumerNativeWorkHazard) return ResultLocked(CoordinatorResult::NativeWorkUnproven);
                const auto &attempt = frame->consumerAbort;
                const bool queueDrain = value.drainKind == ConsumerDrainKind::QueueIdle && value.drainObject == value.queue;
                const bool deviceDrain = value.drainKind == ConsumerDrainKind::DeviceIdle && value.drainObject == frame->gate->scope.device;
                if (!SameProducerTicket(value.ticket, frame->capture.ticket) || value.queue != attempt.queue || value.command != attempt.command ||
                    value.allocationGeneration != attempt.allocationGeneration || value.recordingGeneration != attempt.recordingGeneration ||
                    value.attemptSerial != attempt.attemptSerial || value.disposition != attempt.disposition ||
                    value.drainObservationSerial <= (std::max)({attempt.attemptSerial, frame->lastNativeAttemptSerial, frame->lastNativeReturnSerial}) ||
                    value.leaseReleaseObservationSerial <= value.drainObservationSerial ||
                    (!queueDrain && !deviceDrain))
                    return ResultLocked(CoordinatorResult::ConsumerAbortDrainMismatch);
                if (value.nativeDrainResult != 0) return ResultLocked(CoordinatorResult::NativeConsumerDrainFailed);
                frame->consumerAbortDrained = true; RetireLocked(*frame, retired, unpin);
                result = ResultLocked(CoordinatorResult::Accepted);
            }
            return result;
        }
        CoordinatorResult ObserveConsumerCompletion(const ConsumerCompletionObservation &value) const
        {
            if (!frame) return CoordinatorResult::InvalidCapture;
            std::shared_ptr<Retention> retired;
            std::shared_ptr<Frame> unpin;
            CoordinatorResult result;
            {
                std::lock_guard<std::mutex> lock(frame->mutex);
                if (frame->retired) return ResultLocked(CoordinatorResult::AlreadyRetired);
                if (frame->pendingDispatches) return ResultLocked(CoordinatorResult::NativeDispatchPending);
                if (!frame->published || !frame->presentBegun || !frame->consumerSubmitted) return ResultLocked(CoordinatorResult::ConsumerNotStarted);
                if (frame->producerNativeWorkHazard || frame->consumerNativeWorkHazard) return ResultLocked(CoordinatorResult::NativeWorkUnproven);
                const auto &submitted = frame->consumerSubmission;
                if (!SameProducerTicket(value.ticket, frame->capture.ticket) || !value.nativeDevice || value.nativeDevice != submitted.nativeDevice ||
                    value.nativeDevice != frame->gate->scope.device || value.queue != submitted.queue || value.command != submitted.command ||
                    value.allocationGeneration != submitted.allocationGeneration || value.recordingGeneration != submitted.recordingGeneration ||
                    value.submitSerial != submitted.submitSerial || value.completionObservationSerial <= (std::max)(value.submitSerial, frame->lastNativeReturnSerial) ||
                    value.leaseReleaseObservationSerial <= (std::max)(value.completionObservationSerial, frame->lastNativeSuccessSerial) ||
                    value.completionKind != submitted.completionKind || value.completionObject != submitted.completionObject ||
                    value.completedValue < submitted.completionValue || !SignalValid(value.completionKind, value.completionObject, submitted.completionValue, value.queue))
                    return ResultLocked(CoordinatorResult::ConsumerCompletionMismatch);
                if (submitted.sync.metadataPresent && !SameNativeSyncIdentity(value.sync, submitted.sync))
                    return ResultLocked(CoordinatorResult::NativeSyncMetadataMismatch);
                if (value.nativeResult != 0) return ResultLocked(CoordinatorResult::NativeConsumerCompletionFailed);
                frame->consumerCompleted = true; RetireLocked(*frame, retired, unpin);
                result = ResultLocked(CoordinatorResult::Accepted);
            }
            return result;
        }
        CoordinatorResult ReleaseUnconsumedPublication(const UnconsumedReleaseObservation &value) const
        {
            if (!frame) return CoordinatorResult::InvalidCapture;
            std::shared_ptr<Retention> retired;
            std::shared_ptr<Frame> unpin;
            CoordinatorResult result;
            {
                std::lock_guard<std::mutex> lock(frame->mutex);
                if (frame->retired) return ResultLocked(CoordinatorResult::AlreadyRetired);
                if (frame->pendingDispatches) return ResultLocked(CoordinatorResult::NativeDispatchPending);
                if (!frame->published || frame->presentBegun || frame->consumerAttemptObserved || frame->producerNativeWorkHazard ||
                    !SameProducerTicket(value.ticket, frame->capture.ticket) || !value.providerConfirmedNeverAcquired ||
                    value.leaseReleaseObservationSerial <= (std::max)(frame->captureCompletionSerial, frame->lastNativeReturnSerial))
                    return ResultLocked(CoordinatorResult::ConsumerCompletionMismatch);
                RetireLocked(*frame, retired, unpin);
                result = ResultLocked(CoordinatorResult::Accepted);
            }
            return result;
        }
        CoordinatorResult ObserveNativeDeviceDrain(const NativeDeviceDrainObservation &value) const
        {
            if (!frame) return CoordinatorResult::InvalidCapture;
            std::shared_ptr<Retention> retired;
            std::shared_ptr<Frame> unpin;
            CoordinatorResult result;
            {
                std::lock_guard<std::mutex> lock(frame->mutex);
                if (frame->retired) return ResultLocked(CoordinatorResult::AlreadyRetired);
                if (frame->pendingDispatches) return ResultLocked(CoordinatorResult::NativeDispatchPending);
                const uint64_t latest = (std::max)({frame->lastNativeSuccessSerial, frame->lastNativeAttemptSerial, frame->captureCompletionSerial, frame->lastNativeReturnSerial,
                    frame->consumerAbortObserved ? frame->consumerAbort.attemptSerial : uint64_t(0)});
                if (!frame->gate->closed.load() || !frame->lastNativeAttemptSerial ||
                    !SameProducerTicket(value.ticket, frame->capture.ticket) || value.nativeDevice != frame->gate->scope.device ||
                    !frame->allNativeDevicesKnown || !frame->allNativeSerialsKnown ||
                    value.deviceIdleObservationSerial <= latest ||
                    ((frame->published || frame->consumerAttemptObserved) && value.leaseReleaseObservationSerial <= value.deviceIdleObservationSerial))
                    return ResultLocked(CoordinatorResult::NativeDeviceDrainMismatch);
                if (value.nativeResult != 0) return ResultLocked(CoordinatorResult::NativeDeviceDrainFailed);
                frame->nativeDeviceDrainObserved = true; frame->gpuCompleted = true;
                frame->submissionQualified = false; frame->invalidated = true;
                RetireLocked(*frame, retired, unpin);
                result = ResultLocked(CoordinatorResult::Accepted);
            }
            return result;
        }
        CoordinatorResult DiscardUnsubmittedRecording(const RecordingDiscardObservation &value) const
        {
            if (!frame) return CoordinatorResult::InvalidCapture;
            std::shared_ptr<Retention> retired;
            std::shared_ptr<Frame> unpin;
            CoordinatorResult result;
            {
                std::lock_guard<std::mutex> lock(frame->mutex);
                if (frame->retired) return ResultLocked(CoordinatorResult::AlreadyRetired);
                if (frame->pendingDispatches) return ResultLocked(CoordinatorResult::NativeDispatchPending);
                if (frame->submitted || frame->consumerAttemptObserved || frame->producerNativeWorkHazard) return ResultLocked(CoordinatorResult::RecordingAlreadySubmitted);
                if (value.command != frame->capture.command || value.allocationGeneration != frame->capture.allocationGeneration ||
                    value.recordingGeneration != frame->capture.recordingGeneration ||
                    value.observationSerial <= (std::max)(frame->lastNativeAttemptSerial, frame->lastNativeReturnSerial) || value.nativeResetResult != 0)
                    return ResultLocked(CoordinatorResult::RecordingDiscardUnverified);
                frame->invalidated = true; RetireLocked(*frame, retired, unpin);
                result = ResultLocked(CoordinatorResult::Accepted);
            }
            return result;
        }
        CoordinatorResult RetireCompletedCapture() const
        {
            if (!frame) return CoordinatorResult::InvalidCapture;
            std::shared_ptr<Retention> retired;
            std::shared_ptr<Frame> unpin;
            CoordinatorResult result;
            {
                std::lock_guard<std::mutex> lock(frame->mutex);
                if (frame->retired) return ResultLocked(CoordinatorResult::AlreadyRetired);
                if (frame->pendingDispatches) return ResultLocked(CoordinatorResult::NativeDispatchPending);
                if (frame->published || frame->consumerAttemptObserved) return ResultLocked(CoordinatorResult::ConsumerNotStarted);
                if (frame->producerNativeWorkHazard) return ResultLocked(CoordinatorResult::NativeWorkUnproven);
                if (!frame->gpuCompleted) return ResultLocked(CoordinatorResult::CaptureNotCompleted);
                RetireLocked(*frame, retired, unpin);
                result = ResultLocked(CoordinatorResult::Accepted);
            }
            return result;
        }
        void Invalidate() const
        {
            if (frame) { std::lock_guard<std::mutex> lock(frame->mutex); frame->invalidated = true; }
        }
    };

    class NativeDispatchToken
    {
        friend class FrameHandle;
        std::shared_ptr<Frame> frame;
        std::shared_ptr<Retention> lifetime;
        uint64_t id = 0;
        NativeDispatchIntent intent;
        bool resolved = false;
        NativeDispatchToken(std::shared_ptr<Frame> value, uint64_t dispatchId, const NativeDispatchIntent &submitted,
                            std::shared_ptr<Retention> retained) : frame(std::move(value)), lifetime(std::move(retained)), id(dispatchId), intent(submitted) {}
        Frame::Dispatch *FindLocked() const
        {
            for (auto &slot : frame->dispatches) if (slot.id == id) return &slot;
            return nullptr;
        }
        void AbandonLocked(Frame::Dispatch &slot) const
        {
            if (slot.state == Frame::DispatchState::Pending || slot.state == Frame::DispatchState::Resolving)
            {
                slot.state = Frame::DispatchState::Abandoned; ++frame->abandonedDispatches; frame->invalidated = true;
                if (intent.role == NativeDispatchRole::Consumer) frame->consumerNativeWorkHazard = true;
                else frame->producerNativeWorkHazard = true;
            }
        }
        void Drop() noexcept
        {
            if (frame)
            {
                try
                {
                    std::lock_guard<std::mutex> lock(frame->mutex);
                    if (auto *slot = FindLocked()) AbandonLocked(*slot);
                }
                catch (...) {}
            }
            lifetime.reset(); frame.reset(); id = 0;
        }
        template<typename Observation>
        CoordinatorResult Claim(const Observation &value, NativeDispatchRole role, uint64_t returnObservationSerial)
        {
            if (!frame) return CoordinatorResult::InvalidNativeDispatchToken;
            std::lock_guard<std::mutex> lock(frame->mutex);
            if (resolved) return frame->last = CoordinatorResult::NativeDispatchAlreadyResolved;
            auto *slot = FindLocked();
            if (!slot) return frame->last = CoordinatorResult::InvalidNativeDispatchToken;
            if (slot->state == Frame::DispatchState::Resolved) return frame->last = CoordinatorResult::NativeDispatchAlreadyResolved;
            if (slot->state == Frame::DispatchState::Resolving) return frame->last = CoordinatorResult::NativeDispatchResultInProgress;
            if (slot->state == Frame::DispatchState::Abandoned) return frame->last = CoordinatorResult::NativeDispatchUnresolved;
            if (role != intent.role || !SameProducerTicket(value.ticket, intent.ticket) || value.nativeDevice != intent.nativeDevice ||
                value.queue != intent.queue || value.command != intent.command || value.allocationGeneration != intent.allocationGeneration ||
                value.recordingGeneration != intent.recordingGeneration || value.submitSerial != intent.submitSerial ||
                value.completionKind != intent.completionKind || value.completionObject != intent.completionObject ||
                value.completionValue != intent.completionValue || !SameNativeSyncIdentity(value.sync, intent.sync) || value.nativeResult == NoConsumerNativeSubmissionResult ||
                returnObservationSerial <= intent.submitSerial)
            {
                AbandonLocked(*slot);
                return frame->last = CoordinatorResult::NativeDispatchResultMismatch;
            }
            slot->state = Frame::DispatchState::Resolving;
            return CoordinatorResult::Accepted;
        }
        CoordinatorResult Finish(CoordinatorResult result, uint64_t returnObservationSerial)
        {
            std::shared_ptr<Retention> released;
            {
                std::lock_guard<std::mutex> lock(frame->mutex);
                auto *slot = FindLocked();
                if (!slot || slot->state != Frame::DispatchState::Resolving)
                    return frame->last = CoordinatorResult::NativeDispatchUnresolved;
                slot->state = Frame::DispatchState::Resolved; --frame->pendingDispatches; resolved = true;
                frame->lastNativeReturnSerial = (std::max)(frame->lastNativeReturnSerial, returnObservationSerial);
                released = std::move(lifetime); frame->last = result;
            }
            return result;
        }
        CoordinatorResult FailedResolution() noexcept
        {
            try
            {
                std::lock_guard<std::mutex> lock(frame->mutex);
                if (auto *slot = FindLocked()) AbandonLocked(*slot);
                frame->last = CoordinatorResult::NativeDispatchUnresolved;
            }
            catch (...) {}
            return CoordinatorResult::NativeDispatchUnresolved;
        }
    public:
        NativeDispatchToken() = default;
        NativeDispatchToken(const NativeDispatchToken &) = delete;
        NativeDispatchToken &operator=(const NativeDispatchToken &) = delete;
        NativeDispatchToken(NativeDispatchToken &&other) noexcept : frame(std::move(other.frame)), lifetime(std::move(other.lifetime)),
            id(other.id), intent(other.intent), resolved(other.resolved) { other.id = 0; other.resolved = false; }
        NativeDispatchToken &operator=(NativeDispatchToken &&other) noexcept
        {
            if (this != &other)
            {
                Drop(); frame = std::move(other.frame); lifetime = std::move(other.lifetime); id = other.id; intent = other.intent;
                resolved = other.resolved; other.id = 0; other.resolved = false;
            }
            return *this;
        }
        ~NativeDispatchToken() { Drop(); }
        explicit operator bool() const { return bool(frame); }
        CoordinatorResult ResolveProducer(NativeSubmissionObservation value, uint64_t returnObservationSerial, bool drainOnly = false)
        {
            const auto claimed = Claim(value, NativeDispatchRole::Producer, returnObservationSerial);
            if (claimed != CoordinatorResult::Accepted) return claimed;
            try { return Finish(FrameHandle(frame).ObserveNativeSubmission(value, drainOnly), returnObservationSerial); }
            catch (...) { return FailedResolution(); }
        }
        CoordinatorResult ResolveConsumer(ConsumerSubmissionObservation value, uint64_t returnObservationSerial, bool drainOnly = false)
        {
            const auto claimed = Claim(value, NativeDispatchRole::Consumer, returnObservationSerial);
            if (claimed != CoordinatorResult::Accepted) return claimed;
            try { return Finish(FrameHandle(frame).ObserveConsumerSubmission(value, drainOnly), returnObservationSerial); }
            catch (...) { return FailedResolution(); }
        }
    };

    explicit ProducerFrameCoordinator(const ProducerScope &scope) : gate(std::make_shared<Gate>(scope)) {}
    ProducerFrameCoordinator(const ProducerFrameCoordinator &) = delete;
    ProducerFrameCoordinator &operator=(const ProducerFrameCoordinator &) = delete;
    ~ProducerFrameCoordinator() { try { CloseRuntime(); } catch (...) { gate->closed.store(true); } }
    CoordinatorResult RecordCapture(const CaptureObservation &value, std::shared_ptr<void> owner,
                                    ProducerRetireCallback retireCallback, void *retireContext, FrameHandle &output)
    {
        std::lock_guard<std::mutex> lock(framesMutex);
        if (gate->closed.load()) return CoordinatorResult::RuntimeClosed;
        if (gate->ambiguous.load()) return CoordinatorResult::AmbiguousAcquire;
        const auto &scope = gate->scope;
        const auto &ticket = value.ticket;
        if (!scope.registration || !scope.device || !scope.proxySwapchain || !scope.generation || !scope.graphicsQueue ||
            !scope.width || !scope.height || scope.maximumFrames < 1 || scope.maximumFrames > 8)
            return CoordinatorResult::InvalidScope;
        if (ticket.struct_size != sizeof(ticket) || ticket.api_version != ASTERIA_FG_INPUT_API_V1 || ticket.reserved ||
            ticket.registration != scope.registration || ticket.device != scope.device || ticket.swapchain != scope.proxySwapchain ||
            ticket.generation != scope.generation || ticket.width != scope.width || ticket.height != scope.height || !ticket.acquire_id)
            return CoordinatorResult::StaleTicket;
        if (!value.outstandingAcquires) return CoordinatorResult::AcquisitionUnavailable;
        if (value.outstandingAcquires != 1) { gate->ambiguous.store(true); return CoordinatorResult::AmbiguousAcquire; }
        if (value.layerOrder != ProducerLayerOrder::ReShadeBeforeFrameGeneration) return CoordinatorResult::UnsupportedLayerOrder;
        if (value.phase != ProducerPhase::TechniqueRecording) return CoordinatorResult::WrongPhase;
        if (value.queue != scope.graphicsQueue || !value.command || !value.allocationGeneration || !value.recordingGeneration || !owner || !retireCallback)
            return CoordinatorResult::InvalidCapture;
        for (auto found = frames.begin(); found != frames.end();)
        {
            const auto current = found->second.lock();
            if (!current) { found = frames.erase(found); continue; }
            std::lock_guard<std::mutex> frameLock(current->mutex);
            if (current->retired) { found = frames.erase(found); continue; }
            if (found->first == ticket.acquire_id) return CoordinatorResult::DuplicateCapture;
            if (!current->presentBegun) { gate->ambiguous.store(true); return CoordinatorResult::AmbiguousAcquire; }
            ++found;
        }
        if (ticket.acquire_id <= gate->lastAcquire || (gate->hasCapture && ticket.frame_id <= gate->lastFrame)) return CoordinatorResult::StaleTicket;
        if (frames.size() >= scope.maximumFrames) return CoordinatorResult::CapacityExceeded;
        auto frame = std::make_shared<Frame>(gate, value);
        auto retained = std::make_shared<Retention>();
        frames.emplace(ticket.acquire_id, frame);
        retained->owner = std::move(owner); retained->callback = retireCallback; retained->context = retireContext;
        frame->retention = std::move(retained); frame->pendingLifetime = frame;
        gate->lastAcquire = ticket.acquire_id; gate->lastFrame = ticket.frame_id; gate->hasCapture = true;
        output = FrameHandle(std::move(frame));
        return CoordinatorResult::Accepted;
    }
    void ObserveAcquireMultiplicity(uint32_t outstandingAcquires)
    {
        if (outstandingAcquires <= 1) return;
        gate->ambiguous.store(true);
        for (const auto &frame : DrainPending()) frame.Invalidate();
    }
    std::vector<FrameHandle> DrainPending() const
    {
        std::vector<FrameHandle> result;
        std::lock_guard<std::mutex> lock(framesMutex);
        for (const auto &entry : frames)
            if (const auto frame = entry.second.lock())
            {
                std::lock_guard<std::mutex> frameLock(frame->mutex);
                if (!frame->retired) result.push_back(FrameHandle(frame));
            }
        return result;
    }
    void CloseRuntime()
    {
        gate->closed.store(true);
        for (const auto &handle : DrainPending())
        {
            std::shared_ptr<Retention> retired;
            std::shared_ptr<Frame> unpin;
            const auto frame = handle.frame;
            {
                std::lock_guard<std::mutex> lock(frame->mutex);
                frame->invalidated = true;
                if (frame->gpuCompleted && !frame->published && !frame->consumerAttemptObserved && !frame->producerNativeWorkHazard && !frame->pendingDispatches && !frame->retired)
                    RetireLocked(*frame, retired, unpin);
            }
        }
    }
};

inline CoordinatorResult ProducerFrameCoordinator::FrameHandle::BeginNativeDispatch(const NativeDispatchIntent &intent, NativeDispatchToken &output) const
{
    if (!frame) return CoordinatorResult::InvalidCapture;
    if (output) return CoordinatorResult::NativeDispatchTokenOccupied;
    std::lock_guard<std::mutex> serialLock(frame->gate->mutex);
    std::lock_guard<std::mutex> lock(frame->mutex);
    if (frame->retired) return ResultLocked(CoordinatorResult::AlreadyRetired);
    if (frame->gate->closed.load()) return ResultLocked(CoordinatorResult::RuntimeClosed);
    if (!SameProducerTicket(intent.ticket, frame->capture.ticket) || intent.nativeDevice != frame->gate->scope.device ||
        !intent.queue || !intent.command || !intent.allocationGeneration || !intent.recordingGeneration || !intent.submitSerial ||
        intent.submitSerial <= (std::max)({frame->lastDispatchSerial, frame->lastNativeAttemptSerial, frame->lastNativeReturnSerial}) ||
        (intent.role != NativeDispatchRole::Producer && intent.role != NativeDispatchRole::Consumer))
        return ResultLocked(CoordinatorResult::NativeDispatchIntentMismatch);
    if (!SyncIdentityValid(intent.sync, frame->gate->scope)) return ResultLocked(CoordinatorResult::NativeSyncMetadataInvalid);
    if (intent.role == NativeDispatchRole::Producer &&
        (intent.command != frame->capture.command || intent.allocationGeneration != frame->capture.allocationGeneration ||
         intent.recordingGeneration != frame->capture.recordingGeneration || intent.submitSerial <= frame->gate->lastSubmitSerial))
        return ResultLocked(CoordinatorResult::NativeDispatchIntentMismatch);
    if (intent.role == NativeDispatchRole::Consumer &&
        (!frame->published || !frame->presentBegun || intent.submitSerial <= frame->captureCompletionSerial))
        return ResultLocked(CoordinatorResult::ConsumerNotStarted);
    Frame::Dispatch *available = nullptr;
    for (auto &slot : frame->dispatches)
        if (slot.state == Frame::DispatchState::Empty || slot.state == Frame::DispatchState::Resolved) { available = &slot; break; }
    if (!available || frame->nextDispatchId == UINT64_MAX) return ResultLocked(CoordinatorResult::CapacityExceeded);
    available->intent = intent; available->id = ++frame->nextDispatchId; available->state = Frame::DispatchState::Pending;
    ++frame->pendingDispatches; frame->lastDispatchSerial = intent.submitSerial;
    if (intent.role == NativeDispatchRole::Consumer) frame->consumerAttemptObserved = true;
    output = NativeDispatchToken(frame, available->id, intent, frame->retention);
    return ResultLocked(CoordinatorResult::Accepted);
}
}
