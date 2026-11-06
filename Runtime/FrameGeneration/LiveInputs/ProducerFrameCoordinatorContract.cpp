#include "ProducerFrameCoordinator.h"
#include <json.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>

using namespace asteria_live_inputs;
using Json = nlohmann::json;
static uint32_t Checks = 0, Rejections = 0;
static void Check(bool value, const char *message)
{
    ++Checks;
    if (!value) throw std::runtime_error(message);
}
static ProducerScope Scope(uint64_t generation = 41, uint32_t capacity = 4)
{
    return {11, UINT64_C(0x123400001), UINT64_C(0x567800001), generation, UINT64_C(0xABC000001), 640, 360, capacity};
}
static CaptureObservation Capture(uint64_t acquire = 1, uint64_t frame = 0, uint64_t generation = 41)
{
    const auto scope = Scope(generation);
    CaptureObservation value;
    value.ticket = {sizeof(value.ticket), ASTERIA_FG_INPUT_API_V1, scope.registration, scope.device, scope.proxySwapchain,
        scope.generation, frame, scope.width, scope.height, 1, 0, acquire};
    value.queue = scope.graphicsQueue; value.command = UINT64_C(0xFED000001); value.allocationGeneration = 79;
    value.recordingGeneration = 83; value.outstandingAcquires = 1;
    value.layerOrder = ProducerLayerOrder::ReShadeBeforeFrameGeneration; value.phase = ProducerPhase::TechniqueRecording;
    return value;
}
static NativeSubmissionObservation Submission(const CaptureObservation &capture, uint64_t serial = 100)
{
    NativeSubmissionObservation value;
    value.ticket = capture.ticket; value.nativeDevice = capture.ticket.device; value.queue = capture.queue; value.command = capture.command;
    value.allocationGeneration = capture.allocationGeneration; value.recordingGeneration = capture.recordingGeneration;
    value.submitSerial = serial; value.applicationPresentObservationSerial = serial - 1; value.nativeResult = 0;
    value.layerOrder = ProducerLayerOrder::ReShadeBeforeFrameGeneration;
    value.phase = ProducerPhase::AfterReShadeSubmitBeforeInputConsume; value.applicationWaitSetObserved = true;
    value.applicationWaitSemaphores = {UINT64_C(0x100000001), UINT64_C(0x100000002)};
    value.submittedWaitSemaphores = {UINT64_C(0x100000003), UINT64_C(0x100000002), UINT64_C(0x100000001)};
    value.completionKind = CompletionKind::Fence; value.completionObject = UINT64_C(0x200000001);
    return value;
}
static NativeSubmissionObservation StrictSubmission(const CaptureObservation &capture, uint64_t serial = 100)
{
    auto value = Submission(capture, serial);
    value.sync = {true, UINT64_C(0x700000001), UINT64_C(0x900000001), 0, 2, 3};
    value.applicationWaitDetailCount = 2;
    value.applicationWaitDetails[0] = {UINT64_C(0x100000001), 4, 1};
    value.applicationWaitDetails[1] = {UINT64_C(0x100000002), 9, 2};
    value.submittedWaitDetailCount = 3;
    value.submittedWaitDetails[0] = value.applicationWaitDetails[0];
    value.submittedWaitDetails[1] = value.applicationWaitDetails[1];
    value.submittedWaitDetails[2] = {UINT64_C(0x100000003), 0, 4};
    value.signalDetailCount = 1; value.signalDetails[0] = {UINT64_C(0x500000001), 17};
    return value;
}
static NativeCompletionObservation Completion(const NativeSubmissionObservation &submit)
{
    NativeCompletionObservation value;
    value.ticket = submit.ticket; value.nativeDevice = submit.nativeDevice; value.queue = submit.queue; value.command = submit.command;
    value.allocationGeneration = submit.allocationGeneration; value.recordingGeneration = submit.recordingGeneration;
    value.submitSerial = submit.submitSerial; value.completionObservationSerial = submit.submitSerial + 1;
    value.completionKind = submit.completionKind; value.completionObject = submit.completionObject;
    value.completedValue = submit.completionValue; value.nativeResult = 0; value.sync = submit.sync;
    return value;
}
static ProducerInputSemantics Semantics() { return {true, true, true, true, true, true, true, true}; }
static ConsumerSubmissionObservation ConsumerSubmission(const CaptureObservation &capture, uint64_t serial = 300)
{
    ConsumerSubmissionObservation value;
    value.ticket = capture.ticket; value.nativeDevice = capture.ticket.device; value.queue = capture.queue; value.command = UINT64_C(0x300000001);
    value.allocationGeneration = 91; value.recordingGeneration = 93; value.submitSerial = serial;
    value.completionKind = CompletionKind::TimelineSemaphore; value.completionObject = UINT64_C(0x400000001);
    value.completionValue = 17; value.nativeResult = 0; return value;
}
static ConsumerSubmissionObservation StrictConsumerSubmission(const CaptureObservation &capture, uint64_t serial = 300)
{
    auto value = ConsumerSubmission(capture, serial);
    value.sync = {true, UINT64_C(0x700000002), UINT64_C(0x900000002), 0, 1, 3};
    value.signalDetailCount = 1; value.signalDetails[0] = {UINT64_C(0x500000002), 18};
    return value;
}
static ConsumerCompletionObservation ConsumerCompletion(const ConsumerSubmissionObservation &submit)
{
    ConsumerCompletionObservation value;
    value.ticket = submit.ticket; value.nativeDevice = submit.nativeDevice; value.queue = submit.queue; value.command = submit.command;
    value.allocationGeneration = submit.allocationGeneration; value.recordingGeneration = submit.recordingGeneration;
    value.submitSerial = submit.submitSerial; value.completionObservationSerial = submit.submitSerial + 1;
    value.leaseReleaseObservationSerial = submit.submitSerial + 2;
    value.completionKind = submit.completionKind; value.completionObject = submit.completionObject;
    value.completedValue = submit.completionValue; value.nativeResult = 0; value.sync = submit.sync; return value;
}
static ConsumerAbortObservation AbortAttempt(const CaptureObservation &capture, uint64_t serial = 300)
{
    ConsumerAbortObservation value;
    value.ticket = capture.ticket; value.queue = capture.queue; value.command = UINT64_C(0x300000001);
    value.allocationGeneration = 91; value.recordingGeneration = 93; value.attemptSerial = serial;
    value.disposition = ConsumerAbortDisposition::NativeSubmissionFailed;
    value.phase = ProducerPhase::InputConsumeStarted; value.nativeResult = -4; return value;
}
static ConsumerAbortDrainObservation AbortDrain(const ConsumerAbortObservation &attempt)
{
    ConsumerAbortDrainObservation value;
    value.ticket = attempt.ticket; value.queue = attempt.queue; value.command = attempt.command;
    value.allocationGeneration = attempt.allocationGeneration; value.recordingGeneration = attempt.recordingGeneration;
    value.attemptSerial = attempt.attemptSerial; value.disposition = attempt.disposition;
    value.drainKind = ConsumerDrainKind::QueueIdle; value.drainObject = attempt.queue;
    value.drainObservationSerial = attempt.attemptSerial + 1; value.leaseReleaseObservationSerial = attempt.attemptSerial + 2;
    value.nativeDrainResult = 0; return value;
}
static NativeDeviceDrainObservation DeviceDrain(const CaptureObservation &capture, uint64_t serial = 10000)
{
    NativeDeviceDrainObservation value;
    value.ticket = capture.ticket; value.nativeDevice = capture.ticket.device; value.deviceIdleObservationSerial = serial;
    value.leaseReleaseObservationSerial = serial + 1; value.nativeResult = 0; return value;
}
template<typename Observation>
static NativeDispatchIntent DispatchIntent(const Observation &value, NativeDispatchRole role)
{
    NativeDispatchIntent intent;
    intent.ticket = value.ticket; intent.role = role; intent.nativeDevice = value.nativeDevice; intent.queue = value.queue;
    intent.command = value.command; intent.allocationGeneration = value.allocationGeneration; intent.recordingGeneration = value.recordingGeneration;
    intent.submitSerial = value.submitSerial; intent.completionKind = value.completionKind;
    intent.completionObject = value.completionObject; intent.completionValue = value.completionValue;
    intent.sync = value.sync;
    return intent;
}
template<typename Observation> static uint64_t DispatchReturnSerial(const Observation &value) { return value.submitSerial + 10; }
static NativeCompletionObservation DispatchCompletion(const NativeSubmissionObservation &value)
{
    auto result = Completion(value); result.completionObservationSerial = DispatchReturnSerial(value) + 1; return result;
}
static ConsumerCompletionObservation DispatchCompletion(const ConsumerSubmissionObservation &value)
{
    auto result = ConsumerCompletion(value); result.completionObservationSerial = DispatchReturnSerial(value) + 1;
    result.leaseReleaseObservationSerial = result.completionObservationSerial + 1; return result;
}
struct Counts
{
    uint32_t resourcesDestroyed = 0, retireCallbacks = 0, reentrantCallbacks = 0;
    ProducerFrameCoordinator::FrameHandle *reenter = nullptr;
};
struct Owner
{
    Counts *counts;
    std::shared_ptr<Counts> pinnedCounts;
    ~Owner() { ++counts->resourcesDestroyed; }
};
static void Retire(void *context) noexcept
{
    auto &counts = *static_cast<Counts *>(context);
    ++counts.retireCallbacks;
    if (counts.reenter) { const auto snapshot = counts.reenter->Snapshot(); if (snapshot.retired) ++counts.reentrantCallbacks; }
}
struct Fixture
{
    std::shared_ptr<Counts> countsOwner = std::make_shared<Counts>();
    Counts &counts;
    ProducerFrameCoordinator coordinator;
    CaptureObservation capture;
    ProducerFrameCoordinator::FrameHandle handle;
    std::weak_ptr<void> weakOwner;
    explicit Fixture(bool record = true) : counts(*countsOwner), coordinator(Scope()), capture(Capture()) { if (record) Record(); }
    void Record()
    {
        auto owner = std::make_shared<Owner>(); owner->counts = &counts; owner->pinnedCounts = countsOwner; weakOwner = owner;
        Check(coordinator.RecordCapture(capture, owner, &Retire, &counts, handle) == CoordinatorResult::Accepted, "valid capture rejected");
        counts.reenter = &handle;
    }
    void CompleteCapture()
    {
        const auto submit = Submission(capture);
        Check(handle.ObserveNativeSubmission(submit) == CoordinatorResult::Accepted, "valid native submission rejected");
        Check(!handle.Snapshot().gpuCompletionObserved, "native submit was treated as GPU completion");
        Check(handle.ObserveGpuCompletion(Completion(submit)) == CoordinatorResult::Accepted, "matching GPU completion rejected");
    }
    void Publish()
    {
        CompleteCapture();
        Check(handle.QualifyAndPublish(Semantics()) == CoordinatorResult::Accepted, "qualified controlled publication rejected");
    }
    void Cleanup()
    {
        coordinator.CloseRuntime();
        if (!handle || handle.Snapshot().retired) return;
        const auto state = handle.Snapshot();
        if (state.pendingNativeDispatches) return;
        if (state.producerNativeWorkHazard || state.consumerNativeWorkHazard || (state.nativeSubmissionObserved && !state.submissionQualified))
        {
            if (state.nativeDeviceDrainProofPossible)
                Check(handle.ObserveNativeDeviceDrain(DeviceDrain(capture)) == CoordinatorResult::Accepted, "known-device native hazard could not drain");
            return;
        }
        if (!state.nativeSubmissionObserved)
        {
            const RecordingDiscardObservation discard{capture.command, capture.allocationGeneration, capture.recordingGeneration, 5000, 0};
            Check(handle.DiscardUnsubmittedRecording(discard) == CoordinatorResult::Accepted, "recorded-only cleanup rejected");
        }
        else if (!state.gpuCompletionObserved)
        {
            auto complete = Completion(Submission(capture, state.submitSerial));
            complete.queue = state.submittedQueue;
            complete.completionKind = CompletionKind::QueueIdle; complete.completionObject = state.submittedQueue; complete.completedValue = 0;
            Check(handle.ObserveGpuCompletion(complete, true) == CoordinatorResult::Accepted, "queue-idle drain rejected");
        }
        else if (state.published && !state.presentBegun)
        {
            Check(handle.ReleaseUnconsumedPublication({capture.ticket, 900, true}) == CoordinatorResult::Accepted, "unconsumed release rejected");
        }
        else if (state.published)
        {
            if (state.consumerAbortObserved && !state.consumerSubmissionObserved)
            {
                Check(handle.RetireAbortedConsumer(AbortDrain(AbortAttempt(capture))) == CoordinatorResult::Accepted,
                      "failed consumer cleanup drain rejected");
                return;
            }
            const auto submit = ConsumerSubmission(capture);
            if (!state.consumerSubmissionObserved)
                Check(handle.ObserveConsumerSubmission(submit, true) == CoordinatorResult::Accepted, "consumer drain submission rejected");
            Check(handle.ObserveConsumerCompletion(ConsumerCompletion(submit)) == CoordinatorResult::Accepted, "consumer drain completion rejected");
        }
    }
    ~Fixture() { try { Cleanup(); } catch (...) {} }
};
static void CaptureRejection(const std::function<void(CaptureObservation &)> &change)
{
    Fixture fixture(false); change(fixture.capture);
    auto owner = std::make_shared<int>(1);
    Check(fixture.coordinator.RecordCapture(fixture.capture, owner, &Retire, &fixture.counts, fixture.handle) != CoordinatorResult::Accepted,
          "unqualified capture accepted");
    Check(!fixture.handle && fixture.counts.retireCallbacks == 0, "failed capture changed output or invoked retirement callback");
    ++Rejections;
}
static void SubmissionRejection(const std::function<void(NativeSubmissionObservation &)> &change)
{
    Fixture fixture; auto submit = Submission(fixture.capture); change(submit);
    Check(fixture.handle.ObserveNativeSubmission(submit) != CoordinatorResult::Accepted, "unqualified submission accepted");
    Check(!fixture.handle.Snapshot().completedCapture && !fixture.handle.Snapshot().producerSubmissionQualified,
          "rejected submission promoted readiness");
    ++Rejections;
    fixture.Cleanup();
    if (fixture.handle.Snapshot().nativeDeviceDrainProofPossible)
        Check(fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1, "rejected submission owner was not safely drained");
    else Check(!fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 0, "unknown native work was retired without complete drain proof");
}
static void AbortRejection(const std::function<void(ConsumerAbortObservation &)> &change)
{
    Fixture fixture; fixture.Publish();
    Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "abort rejection begin failed");
    auto attempt = AbortAttempt(fixture.capture); change(attempt);
    Check(fixture.handle.ObserveConsumerAbort(attempt) != CoordinatorResult::Accepted, "unverified failed or abandoned consumer accepted");
    Check(!fixture.handle.Snapshot().consumerAbortObserved && !fixture.weakOwner.expired(), "invalid abort retired or changed owner state");
    ++Rejections;
}
static void AbortDrainRejection(const std::function<void(ConsumerAbortDrainObservation &)> &change)
{
    Fixture fixture; fixture.Publish();
    Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "abort drain rejection begin failed");
    const auto attempt = AbortAttempt(fixture.capture);
    Check(fixture.handle.ObserveConsumerAbort(attempt) == CoordinatorResult::Accepted, "valid failed consumer attempt rejected");
    auto drain = AbortDrain(attempt); change(drain);
    Check(fixture.handle.RetireAbortedConsumer(drain) != CoordinatorResult::Accepted, "unverified failed consumer drain retired owner");
    Check(!fixture.handle.Snapshot().consumerAbortDrained && !fixture.weakOwner.expired(), "failed drain changed retirement state");
    ++Rejections;
    fixture.Cleanup();
    Check(fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1, "valid recovery from rejected consumer drain failed");
}
static void ConsumerSuccessSafety(const std::function<void(ConsumerSubmissionObservation &)> &change, bool closeFirst = false)
{
    Fixture fixture; fixture.Publish();
    Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "consumer hazard fixture begin failed");
    if (closeFirst) fixture.coordinator.CloseRuntime();
    auto native = ConsumerSubmission(fixture.capture); change(native);
    Check(fixture.handle.ObserveConsumerSubmission(native) != CoordinatorResult::Accepted, "unqualified successful consumer submission was qualified");
    Check(fixture.handle.Snapshot().consumerSubmissionObserved && !fixture.weakOwner.expired(), "successful consumer physical work was forgotten");
    Check(fixture.handle.ObserveConsumerAbort(AbortAttempt(fixture.capture)) == CoordinatorResult::ConsumerAttemptConflict,
          "scope abort erased a successful foreign or unqualified consumer");
    Check(fixture.handle.RetireAbortedConsumer(AbortDrain(AbortAttempt(fixture.capture))) != CoordinatorResult::Accepted,
          "scope queue idle retired successful foreign or unqualified consumer work");
    fixture.coordinator.CloseRuntime();
    Check(!fixture.weakOwner.expired(), "close forgot successful unqualified consumer work");
    if (fixture.handle.Snapshot().nativeDeviceDrainProofPossible)
    {
        Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::Accepted,
              "known-device successful work could not be explicitly device-drained");
        Check(fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1, "device-drained successful consumer did not retire exactly once");
    }
    else
    {
        Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::NativeDeviceDrainMismatch,
              "unknown native identity was treated as covered by the scope device idle");
        Check(!fixture.weakOwner.expired(), "unknown successful native work released owner without proof");
    }
    ++Rejections;
}
static void TestDispatchLifetime()
{
    using Token = ProducerFrameCoordinator::NativeDispatchToken;
    static_assert(!std::is_copy_constructible_v<Token> && !std::is_copy_assignable_v<Token>);
    static_assert(std::is_nothrow_move_constructible_v<Token> && std::is_nothrow_move_assignable_v<Token>);
    {
        Fixture fixture; const auto submit = Submission(fixture.capture); Token token;
        Check(fixture.handle.BeginNativeDispatch(DispatchIntent(submit, NativeDispatchRole::Producer), token) == CoordinatorResult::Accepted,
              "producer pre-submit lifetime rejected valid native intent");
        const auto native = [&] {
            Check(fixture.handle.Snapshot().pendingNativeDispatches == 1 && !fixture.weakOwner.expired(), "native entry did not retain its resource owner");
            fixture.coordinator.CloseRuntime();
            Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::NativeDispatchPending,
                  "device idle retired a native call that has not returned");
            Check(!fixture.weakOwner.expired(), "runtime close retired native dispatch in progress");
            return submit;
        }();
        Check(token.ResolveProducer(native, DispatchReturnSerial(native)) == CoordinatorResult::RuntimeClosed, "closed producer did not record returned native success");
        Check(!fixture.handle.Snapshot().pendingNativeDispatches && fixture.handle.Snapshot().nativeSubmissionObserved,
              "matching native return did not transfer from pending dispatch to observed work");
        Check(fixture.handle.ObserveGpuCompletion(DispatchCompletion(native)) == CoordinatorResult::Accepted, "closed returned native work could not complete");
        Check(fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1 && fixture.counts.reentrantCallbacks == 1,
              "native-call completion did not retire once outside callback locks");
        Check(token.ResolveProducer(native, DispatchReturnSerial(native)) == CoordinatorResult::NativeDispatchAlreadyResolved, "same native token resolved twice");
    }
    {
        Fixture fixture; fixture.Publish(); const auto submit = ConsumerSubmission(fixture.capture); Token token;
        Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "consumer native entry present failed");
        Check(fixture.handle.BeginNativeDispatch(DispatchIntent(submit, NativeDispatchRole::Consumer), token) == CoordinatorResult::Accepted,
              "consumer pre-submit lifetime rejected valid intent");
        fixture.coordinator.CloseRuntime();
        Check(fixture.handle.ObserveConsumerCompletion(DispatchCompletion(submit)) == CoordinatorResult::NativeDispatchPending,
              "consumer signal released native call before its return");
        Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::NativeDispatchPending,
              "closed consumer native entry was bypassed by device idle");
        Check(token.ResolveConsumer(submit, DispatchReturnSerial(submit)) == CoordinatorResult::RuntimeClosed, "closed consumer native return was forgotten");
        Check(!fixture.weakOwner.expired() && fixture.handle.Snapshot().consumerSubmissionObserved,
              "returned consumer GPU work was not kept alive");
        Check(fixture.handle.ObserveConsumerCompletion(DispatchCompletion(submit)) == CoordinatorResult::Accepted,
              "returned consumer work did not retire with exact completion and release");
        Check(fixture.weakOwner.expired() && fixture.counts.reentrantCallbacks == 1, "consumer native lifetime retired before or inside locks");
    }
    {
        Fixture fixture; fixture.CompleteCapture(); const auto submit = Submission(fixture.capture, 200); Token token;
        Check(fixture.handle.BeginNativeDispatch(DispatchIntent(submit, NativeDispatchRole::Producer), token) == CoordinatorResult::Accepted,
              "producer resubmit lifetime was not admitted before native entry");
        Check(!fixture.handle.Snapshot().completedCapture, "pending native rewrite left producer capture completed");
        Check(fixture.handle.QualifyAndPublish(Semantics()) == CoordinatorResult::NativeDispatchPending &&
              fixture.handle.RetireCompletedCapture() == CoordinatorResult::NativeDispatchPending,
              "old producer completion bypassed a new native call");
        fixture.coordinator.CloseRuntime();
        Check(!fixture.weakOwner.expired(), "close used the first producer completion while second submit entered native");
        Check(token.ResolveProducer(submit, DispatchReturnSerial(submit)) == CoordinatorResult::NativeWorkUnproven, "returned second producer work lost its hazard");
        Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::Accepted && fixture.weakOwner.expired(),
              "returned producer hazard did not retire after stronger device drain");
    }
    {
        Fixture fixture; fixture.Publish(); const auto first = ConsumerSubmission(fixture.capture); Token token;
        Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "second consumer native fixture begin failed");
        Check(fixture.handle.ObserveConsumerSubmission(first) == CoordinatorResult::Accepted, "first consumer submission failed");
        auto second = ConsumerSubmission(fixture.capture, 400); ++second.queue;
        Check(fixture.handle.BeginNativeDispatch(DispatchIntent(second, NativeDispatchRole::Consumer), token) == CoordinatorResult::Accepted,
              "foreign queue native lifetime was rejected before it could retain work");
        Check(fixture.handle.ObserveConsumerCompletion(DispatchCompletion(first)) == CoordinatorResult::NativeDispatchPending,
              "first consumer completion retired second native dispatch");
        Check(token.ResolveConsumer(second, DispatchReturnSerial(second)) == CoordinatorResult::NativeWorkUnproven, "second native consumer return lost distinct-work hazard");
        fixture.coordinator.CloseRuntime();
        Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::Accepted && fixture.weakOwner.expired(),
              "returned foreign native work could not device-drain");
    }
    for (bool consumer : {false, true})
    {
        Fixture fixture; Token token;
        if (consumer)
        {
            fixture.Publish(); Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "failed consumer native begin failed");
            auto submit = ConsumerSubmission(fixture.capture); submit.nativeResult = -4;
            Check(fixture.handle.BeginNativeDispatch(DispatchIntent(submit, NativeDispatchRole::Consumer), token) == CoordinatorResult::Accepted, "failed consumer token rejected");
            Check(token.ResolveConsumer(submit, DispatchReturnSerial(submit)) == CoordinatorResult::NativeConsumerSubmissionFailed, "failed native consumer return was accepted as success");
            const auto attempt = AbortAttempt(fixture.capture);
            Check(fixture.handle.ObserveConsumerAbort(attempt) == CoordinatorResult::Accepted, "returned failed consumer attempt rejected");
            Check(fixture.handle.RetireAbortedConsumer(AbortDrain(attempt)) == CoordinatorResult::ConsumerAbortDrainMismatch,
                  "failed consumer reused idle evidence from before its native return");
            auto drain = AbortDrain(attempt); drain.drainObservationSerial = DispatchReturnSerial(submit) + 1;
            drain.leaseReleaseObservationSerial = drain.drainObservationSerial + 1;
            Check(fixture.handle.RetireAbortedConsumer(drain) == CoordinatorResult::Accepted, "returned failed consumer could not safely drain");
        }
        else
        {
            auto submit = Submission(fixture.capture); submit.nativeResult = -4;
            Check(fixture.handle.BeginNativeDispatch(DispatchIntent(submit, NativeDispatchRole::Producer), token) == CoordinatorResult::Accepted, "failed producer token rejected");
            Check(token.ResolveProducer(submit, DispatchReturnSerial(submit)) == CoordinatorResult::NativeSubmissionFailed, "failed native producer return was accepted as success");
            Check(fixture.handle.DiscardUnsubmittedRecording({fixture.capture.command, fixture.capture.allocationGeneration,
                fixture.capture.recordingGeneration, 5000, 0}) == CoordinatorResult::Accepted, "returned failed producer could not discard reset recording");
        }
        Check(!fixture.handle.Snapshot().pendingNativeDispatches && fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1,
              "native failed return did not close lifetime safely");
    }
    for (bool consumer : {false, true})
    {
        Fixture fixture;
        try
        {
            Token token;
            if (consumer)
            {
                fixture.Publish(); Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "abandoned consumer present failed");
                Check(fixture.handle.BeginNativeDispatch(DispatchIntent(ConsumerSubmission(fixture.capture), NativeDispatchRole::Consumer), token) == CoordinatorResult::Accepted,
                      "abandoned consumer token begin failed");
            }
            else Check(fixture.handle.BeginNativeDispatch(DispatchIntent(Submission(fixture.capture), NativeDispatchRole::Producer), token) == CoordinatorResult::Accepted,
                       "abandoned producer token begin failed");
            throw std::runtime_error("controlled native-call unwind");
        }
        catch (const std::runtime_error &) {}
        fixture.coordinator.CloseRuntime();
        const auto state = fixture.handle.Snapshot();
        Check(state.pendingNativeDispatches == 1 && state.abandonedNativeDispatches == 1 && !state.nativeDeviceDrainProofPossible && !fixture.weakOwner.expired(),
              "token unwind pretended no native call remained");
        Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::NativeDispatchPending,
              "abandoned native call was released through device idle");
        Check(fixture.handle.DiscardUnsubmittedRecording({fixture.capture.command, fixture.capture.allocationGeneration,
            fixture.capture.recordingGeneration, 5000, 0}) == CoordinatorResult::NativeDispatchPending,
              "abandoned token was released by recording reset");
        ++Rejections;
    }
    const std::function<void(NativeSubmissionObservation &)> mismatches[] = {
        [](auto &v) { ++v.ticket.acquire_id; }, [](auto &v) { ++v.nativeDevice; }, [](auto &v) { ++v.queue; },
        [](auto &v) { ++v.command; }, [](auto &v) { ++v.allocationGeneration; }, [](auto &v) { ++v.recordingGeneration; },
        [](auto &v) { ++v.submitSerial; }, [](auto &v) { v.completionKind = CompletionKind::Unknown; },
        [](auto &v) { ++v.completionObject; }, [](auto &v) { ++v.completionValue; },
        [](auto &v) { v.nativeResult = NoConsumerNativeSubmissionResult; }
    };
    for (const auto &change : mismatches)
    {
        Fixture fixture; const auto actual = Submission(fixture.capture); Token token;
        Check(fixture.handle.BeginNativeDispatch(DispatchIntent(actual, NativeDispatchRole::Producer), token) == CoordinatorResult::Accepted, "mismatch lifetime begin failed");
        auto wrong = actual; change(wrong);
        Check(token.ResolveProducer(wrong, DispatchReturnSerial(wrong)) == CoordinatorResult::NativeDispatchResultMismatch, "unrelated native return resolved token");
        Check(token.ResolveProducer(actual, DispatchReturnSerial(actual)) == CoordinatorResult::NativeDispatchUnresolved, "ambiguous native result was silently overwritten");
        fixture.coordinator.CloseRuntime();
        Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::NativeDispatchPending && !fixture.weakOwner.expired(),
              "mismatched native result released its owner");
        ++Rejections;
    }
    {
        Fixture fixture; const auto submit = Submission(fixture.capture); Token token;
        Check(fixture.handle.BeginNativeDispatch(DispatchIntent(submit, NativeDispatchRole::Producer), token) == CoordinatorResult::Accepted, "move fixture lifetime begin failed");
        Token moved(std::move(token));
        Check(token.ResolveProducer(submit, DispatchReturnSerial(submit)) == CoordinatorResult::InvalidNativeDispatchToken, "moved-from native token resolved work");
        Check(moved.ResolveProducer(submit, DispatchReturnSerial(submit)) == CoordinatorResult::Accepted, "moved native token could not resolve exact return");
        Check(fixture.handle.Snapshot().pendingNativeDispatches == 0, "resolved moved token left an active native call");
        Token second; const auto next = Submission(fixture.capture, 200);
        Check(fixture.handle.BeginNativeDispatch(DispatchIntent(next, NativeDispatchRole::Producer), second) == CoordinatorResult::Accepted, "resolved dispatch slot was not reusable");
        Check(moved.ResolveProducer(submit, DispatchReturnSerial(submit)) == CoordinatorResult::NativeDispatchAlreadyResolved, "slot reuse revived an old resolved token");
        Check(second.ResolveProducer(next, DispatchReturnSerial(next)) == CoordinatorResult::NativeWorkUnproven, "slot reuse hid second native work");
        fixture.coordinator.CloseRuntime();
        Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::Accepted && fixture.weakOwner.expired(), "moved-token fixture did not device-drain");
    }
    {
        Fixture fixture; Token tokens[17];
        for (uint64_t index = 0; index < 16; ++index)
            Check(fixture.handle.BeginNativeDispatch(DispatchIntent(Submission(fixture.capture, 100 + index), NativeDispatchRole::Producer), tokens[index]) == CoordinatorResult::Accepted,
                  "bounded native token slot was rejected early");
        Check(fixture.handle.BeginNativeDispatch(DispatchIntent(Submission(fixture.capture, 116), NativeDispatchRole::Producer), tokens[16]) == CoordinatorResult::CapacityExceeded,
              "native token storage grew beyond its fixed capacity");
        Check(!tokens[16] && fixture.handle.Snapshot().pendingNativeDispatches == 16, "failed native slot allocation changed pending count");
        fixture.coordinator.CloseRuntime();
        for (uint64_t index = 0; index < 16; ++index)
            Check(tokens[index].ResolveProducer(Submission(fixture.capture, 100 + index), DispatchReturnSerial(Submission(fixture.capture, 100 + index))) ==
                  (index ? CoordinatorResult::NativeWorkUnproven : CoordinatorResult::RuntimeClosed), "bounded native result failed to retain success history");
        Check(fixture.handle.Snapshot().pendingNativeDispatches == 0 && !fixture.weakOwner.expired(), "bounded pending native calls released on return alone");
        Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::Accepted && fixture.weakOwner.expired(), "bounded native calls failed safe drain");
    }
    const std::function<void(NativeDispatchIntent &)> badIntents[] = {
        [](auto &v) { ++v.ticket.device; }, [](auto &v) { ++v.nativeDevice; }, [](auto &v) { v.queue = 0; },
        [](auto &v) { ++v.command; }, [](auto &v) { v.allocationGeneration = 0; }, [](auto &v) { v.recordingGeneration = 0; },
        [](auto &v) { v.submitSerial = 0; }, [](auto &v) { v.role = NativeDispatchRole::Unknown; }
    };
    for (const auto &change : badIntents)
    {
        Fixture fixture; Token token; auto intent = DispatchIntent(Submission(fixture.capture), NativeDispatchRole::Producer); change(intent);
        Check(fixture.handle.BeginNativeDispatch(intent, token) == CoordinatorResult::NativeDispatchIntentMismatch,
              "unrelated pre-submit native intent was admitted");
        Check(!token && fixture.handle.Snapshot().pendingNativeDispatches == 0, "invalid native intent occupied a lifetime slot");
        ++Rejections;
    }
    {
        Fixture fixture; const auto submit = Submission(fixture.capture); Token token, other;
        const auto intent = DispatchIntent(submit, NativeDispatchRole::Producer);
        Check(fixture.handle.BeginNativeDispatch(intent, token) == CoordinatorResult::Accepted, "duplicate-token fixture begin failed");
        Check(fixture.handle.BeginNativeDispatch(intent, token) == CoordinatorResult::NativeDispatchTokenOccupied, "live token output was overwritten");
        Check(fixture.handle.BeginNativeDispatch(intent, other) == CoordinatorResult::NativeDispatchIntentMismatch, "same submit serial opened duplicate native token");
        Check(token.ResolveProducer(submit, DispatchReturnSerial(submit)) == CoordinatorResult::Accepted, "duplicate-token rejection corrupted original lifetime");
    }
    for (unsigned iteration = 0; iteration < 50; ++iteration)
    {
        Fixture fixture; const auto submit = Submission(fixture.capture); Token token;
        Check(fixture.handle.BeginNativeDispatch(DispatchIntent(submit, NativeDispatchRole::Producer), token) == CoordinatorResult::Accepted,
              "concurrent native token begin failed");
        CoordinatorResult result = CoordinatorResult::NativeDispatchUnresolved;
        std::thread returned([&] { result = token.ResolveProducer(submit, DispatchReturnSerial(submit)); });
        std::thread closing([&] { fixture.coordinator.CloseRuntime(); });
        returned.join(); closing.join();
        Check((result == CoordinatorResult::Accepted || result == CoordinatorResult::RuntimeClosed) &&
              fixture.handle.Snapshot().pendingNativeDispatches == 0 && !fixture.weakOwner.expired(), "close/return race retired active GPU work");
        Check(fixture.handle.ObserveGpuCompletion(DispatchCompletion(submit)) == CoordinatorResult::Accepted, "closed native call completion could not drain");
        Check(fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1 && fixture.counts.reentrantCallbacks == 1,
              "close/return/completion race did not retire once outside locks");
    }
    for (bool consumer : {false, true})
    {
        Fixture fixture; Token token;
        if (consumer)
        {
            fixture.Publish(); Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "return-boundary consumer begin failed");
            const auto submit = ConsumerSubmission(fixture.capture);
            Check(fixture.handle.BeginNativeDispatch(DispatchIntent(submit, NativeDispatchRole::Consumer), token) == CoordinatorResult::Accepted,
                  "return-boundary consumer native entry failed");
            fixture.coordinator.CloseRuntime();
            const auto oldIdle = DeviceDrain(fixture.capture, submit.submitSerial + 5);
            Check(fixture.handle.ObserveNativeDeviceDrain(oldIdle) == CoordinatorResult::NativeDispatchPending, "idle during native call was accepted");
            Check(token.ResolveConsumer(submit, DispatchReturnSerial(submit)) == CoordinatorResult::RuntimeClosed, "return-boundary consumer result rejected");
            Check(fixture.handle.ObserveNativeDeviceDrain(oldIdle) == CoordinatorResult::NativeDeviceDrainMismatch,
                  "idle from inside consumer native call was replayed after return");
            Check(fixture.handle.ObserveConsumerCompletion(ConsumerCompletion(submit)) == CoordinatorResult::ConsumerCompletionMismatch,
                  "pre-return consumer completion was reused after native return");
            Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture, DispatchReturnSerial(submit) + 1)) == CoordinatorResult::Accepted,
                  "newer consumer device idle did not safely drain after return");
        }
        else
        {
            const auto submit = Submission(fixture.capture);
            Check(fixture.handle.BeginNativeDispatch(DispatchIntent(submit, NativeDispatchRole::Producer), token) == CoordinatorResult::Accepted,
                  "return-boundary producer native entry failed");
            fixture.coordinator.CloseRuntime();
            const auto oldIdle = DeviceDrain(fixture.capture, submit.submitSerial + 5);
            Check(fixture.handle.ObserveNativeDeviceDrain(oldIdle) == CoordinatorResult::NativeDispatchPending, "producer idle during call was accepted");
            Check(token.ResolveProducer(submit, DispatchReturnSerial(submit)) == CoordinatorResult::RuntimeClosed, "return-boundary producer result rejected");
            Check(fixture.handle.ObserveNativeDeviceDrain(oldIdle) == CoordinatorResult::NativeDeviceDrainMismatch,
                  "idle from inside producer native call was replayed after return");
            Check(fixture.handle.ObserveGpuCompletion(Completion(submit)) == CoordinatorResult::CompletionMismatch,
                  "pre-return producer completion was reused after native return");
            Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture, DispatchReturnSerial(submit) + 1)) == CoordinatorResult::Accepted,
                  "newer producer device idle did not safely drain after return");
        }
        Check(fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1, "native return boundary fixture did not retire once");
    }
    for (uint64_t badReturn : {uint64_t(0), uint64_t(100)})
    {
        Fixture fixture; Token token; const auto submit = Submission(fixture.capture);
        Check(fixture.handle.BeginNativeDispatch(DispatchIntent(submit, NativeDispatchRole::Producer), token) == CoordinatorResult::Accepted,
              "invalid native return serial fixture failed");
        Check(token.ResolveProducer(submit, badReturn) == CoordinatorResult::NativeDispatchResultMismatch,
              "invalid native return boundary closed an active dispatch");
        fixture.coordinator.CloseRuntime();
        Check(fixture.handle.Snapshot().pendingNativeDispatches == 1 && fixture.handle.Snapshot().abandonedNativeDispatches == 1 && !fixture.weakOwner.expired(),
              "invalid native return serial released in-flight lifetime");
        ++Rejections;
    }
    {
        Fixture fixture; Token token; const auto submit = Submission(fixture.capture);
        Check(fixture.handle.BeginNativeDispatch(DispatchIntent(submit, NativeDispatchRole::Producer), token) == CoordinatorResult::Accepted,
              "native role mismatch fixture failed");
        Check(token.ResolveConsumer(ConsumerSubmission(fixture.capture), 500) == CoordinatorResult::NativeDispatchResultMismatch,
              "consumer result resolved a producer native call");
        Check(fixture.handle.Snapshot().pendingNativeDispatches == 1 && !fixture.weakOwner.expired(), "native role mismatch released lifetime");
        ++Rejections;
    }
}
static void WriteReport(bool passed, const char *error = nullptr)
{
    std::filesystem::create_directories("results");
    Json report = {{"author", "Anokianx"}, {"passed", passed}, {"checks", Checks}, {"rejection_cases", Rejections},
        {"scope", "producer_frame_coordination_cpu_mock_contract_only"}, {"cpu_mock_only", true},
        {"native_gpu_submission_observed", false}, {"actual_gpu_completion_verified", false}, {"game_started", false},
        {"game_frame_generation", false}, {"real_game_producer_connected", false}, {"main_view_verified", false},
        {"callback_stage_not_gpu_proof", true}, {"completed_capture_separate_from_fg_qualification", true},
        {"consumer_native_submission_required_before_retirement", true}, {"runtime_close_keeps_inflight_owner", true},
        {"consumer_lease_release_required_before_retirement", true},
        {"failed_or_abandoned_consumer_retirement_requires_native_drain", true},
        {"consumer_abort_retirement_requires_final_lease_release", true},
        {"successful_consumer_submission_blocks_abort_retirement", true},
        {"consumer_abort_after_runtime_close_supported", true},
        {"consumer_abort_drain_not_normal_consumer_completion", true},
        {"same_ticket_success_recorded_before_qualification", true},
        {"foreign_queue_success_blocks_scope_abort_retirement", true},
        {"distinct_successes_require_device_drain", true},
        {"invalid_success_metadata_persists_as_hazard", true},
        {"device_drain_requires_all_native_devices_and_serials_known", true},
        {"device_drain_follows_latest_attempt_and_abort", true},
        {"unconsumed_release_follows_capture_completion", true},
        {"begin_present_rejects_invalidated_publication", true},
        {"native_pre_submit_lifetime_handshake", true}, {"pending_native_dispatch_blocks_retirement", true},
        {"native_return_requires_exact_dispatch_identity", true}, {"abandoned_dispatch_retains_owner", true},
        {"dispatch_tokens_move_only_and_bounded", true}, {"native_dispatch_close_return_races_checked", true},
        {"native_return_serial_excludes_earlier_idle_proofs", true},
        {"strict_sync_metadata_optional_for_legacy_observations", true}, {"bounded_wait_stage_values_validated", true},
        {"signal_semaphore_identity_set_validated", true}, {"submit_batch_identity_validated", true},
        {"queue_family_and_ownership_epoch_validated", true}, {"completion_sync_identity_rechecked", true},
        {"retirement_callbacks_reentrant_outside_locks", true}, {"closed_runtime_handles_remain_drainable", true}};
    report["state_names"] = {"recorded_capture", "native_submission_observed", "completed_capture", "published", "consumer_in_flight", "retired"};
    report["undrained_native_work_retains_owner"] = true;
    if (error) report["error"] = error;
    std::ofstream output("results/producer-frame-coordinator-contract.json"); output << report.dump(2) << '\n';
}
int main()
{
    try
    {
        {
            Fixture fixture;
            Check(fixture.handle.Snapshot().state == ProducerFrameState::RecordedCapture, "technique did not remain recorded-only");
            Check(!fixture.handle.Snapshot().completedCapture && !fixture.handle.Snapshot().producerSubmissionQualified, "technique callback fabricated readiness");
            Check(fixture.handle.QualifyAndPublish(Semantics()) == CoordinatorResult::CaptureNotCompleted, "publication before native completion accepted");
            fixture.CompleteCapture();
            Check(fixture.handle.Snapshot().completedCapture && fixture.handle.Snapshot().captureReady &&
                  !fixture.handle.Snapshot().producerSubmissionQualified, "completion and semantics were conflated");
            Check(fixture.handle.QualifyAndPublish({}) == CoordinatorResult::SemanticsUnverified, "unknown game semantics were accepted");
            Check(!fixture.handle.Snapshot().producerSubmissionQualified, "unknown semantics set FG qualification");
            fixture.Cleanup();
            Check(fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1 && fixture.counts.reentrantCallbacks == 1,
                  "completed capture did not retire outside locks");
        }
        CaptureRejection([](auto &value) { value.ticket.struct_size = 8; });
        CaptureRejection([](auto &value) { value.ticket.api_version = 2; });
        CaptureRejection([](auto &value) { value.ticket.reserved = 1; });
        CaptureRejection([](auto &value) { ++value.ticket.registration; });
        CaptureRejection([](auto &value) { ++value.ticket.device; });
        CaptureRejection([](auto &value) { ++value.ticket.swapchain; });
        CaptureRejection([](auto &value) { --value.ticket.generation; });
        CaptureRejection([](auto &value) { value.ticket.acquire_id = 0; });
        CaptureRejection([](auto &value) { ++value.ticket.width; });
        CaptureRejection([](auto &value) { value.outstandingAcquires = 0; });
        CaptureRejection([](auto &value) { value.outstandingAcquires = 2; });
        CaptureRejection([](auto &value) { value.layerOrder = ProducerLayerOrder::Unknown; });
        CaptureRejection([](auto &value) { value.layerOrder = ProducerLayerOrder::FrameGenerationBeforeReShade; });
        CaptureRejection([](auto &value) { value.phase = ProducerPhase::AfterReShadeSubmitBeforeInputConsume; });
        CaptureRejection([](auto &value) { ++value.queue; });
        CaptureRejection([](auto &value) { value.command = 0; });
        CaptureRejection([](auto &value) { value.allocationGeneration = 0; });
        CaptureRejection([](auto &value) { value.recordingGeneration = 0; });
        SubmissionRejection([](auto &value) { ++value.ticket.generation; });
        SubmissionRejection([](auto &value) { ++value.ticket.acquire_id; });
        SubmissionRejection([](auto &value) { ++value.command; });
        SubmissionRejection([](auto &value) { ++value.allocationGeneration; });
        SubmissionRejection([](auto &value) { ++value.recordingGeneration; });
        SubmissionRejection([](auto &value) { ++value.queue; });
        SubmissionRejection([](auto &value) { value.submitSerial = 0; });
        SubmissionRejection([](auto &value) { value.nativeResult = -4; });
        SubmissionRejection([](auto &value) { value.applicationWaitSetObserved = false; });
        SubmissionRejection([](auto &value) { value.applicationPresentObservationSerial = 0; });
        SubmissionRejection([](auto &value) { value.applicationPresentObservationSerial = value.submitSerial; });
        SubmissionRejection([](auto &value) { value.submittedWaitSemaphores.pop_back(); });
        SubmissionRejection([](auto &value) { value.submittedWaitSemaphores.push_back(value.submittedWaitSemaphores[0]); });
        SubmissionRejection([](auto &value) { value.completionKind = CompletionKind::Unknown; });
        SubmissionRejection([](auto &value) { value.phase = ProducerPhase::TechniqueRecording; });
        SubmissionRejection([](auto &value) { value.layerOrder = ProducerLayerOrder::FrameGenerationBeforeReShade; });
        {
            Fixture fixture;
            auto submit = Submission(fixture.capture); submit.applicationWaitSemaphores.clear(); submit.submittedWaitSemaphores.clear();
            Check(fixture.handle.ObserveNativeSubmission(submit) == CoordinatorResult::Accepted, "observed empty application wait set rejected");
            Check(fixture.handle.ObserveNativeSubmission(submit) == CoordinatorResult::DuplicateSubmission, "duplicate native submission accepted");
            auto complete = Completion(submit); ++complete.recordingGeneration;
            Check(fixture.handle.ObserveGpuCompletion(complete) == CoordinatorResult::CompletionMismatch, "wrong recording completion accepted");
            complete = Completion(submit); ++complete.queue;
            Check(fixture.handle.ObserveGpuCompletion(complete) == CoordinatorResult::CompletionMismatch, "wrong queue completion accepted");
            complete = Completion(submit); ++complete.submitSerial;
            Check(fixture.handle.ObserveGpuCompletion(complete) == CoordinatorResult::CompletionMismatch, "wrong submit serial completion accepted");
            complete = Completion(submit); ++complete.completionObject;
            Check(fixture.handle.ObserveGpuCompletion(complete) == CoordinatorResult::CompletionMismatch, "unrelated fence completion accepted");
            complete = Completion(submit); complete.nativeResult = 1;
            Check(fixture.handle.ObserveGpuCompletion(complete) == CoordinatorResult::NativeCompletionFailed, "timeout was treated as GPU completion");
            Check(!fixture.weakOwner.expired(), "timeout released in-flight owner");
            complete = Completion(submit);
            Check(fixture.handle.ObserveGpuCompletion(complete) == CoordinatorResult::Accepted, "matching native completion rejected");
            Check(fixture.handle.ObserveGpuCompletion(complete) == CoordinatorResult::DuplicateCompletion, "repeated completion accepted");
        }
        for (unsigned field = 0; field < 8; ++field)
        {
            Fixture fixture; fixture.CompleteCapture(); auto semantics = Semantics();
            switch (field)
            {
            case 0: semantics.mainViewVerified = false; break;
            case 1: semantics.sceneDepthVerified = false; break;
            case 2: semantics.cameraVerified = false; break;
            case 3: semantics.hudAbsentVerified = false; break;
            case 4: semantics.motionVerified = false; break;
            case 5: semantics.matchingColorStreamVerified = false; break;
            case 6: semantics.sceneEpochVerified = false; break;
            case 7: semantics.pipelineStateVerified = false; break;
            }
            Check(fixture.handle.QualifyAndPublish(semantics) == CoordinatorResult::SemanticsUnverified, "one missing semantic qualification accepted");
            Check(fixture.handle.Snapshot().completedCapture && !fixture.handle.Snapshot().producerSubmissionQualified, "semantic rejection destroyed capture/FG separation");
            ++Rejections;
        }
        {
            Fixture fixture; fixture.Publish();
            Check(fixture.handle.QualifyAndPublish(Semantics()) == CoordinatorResult::DuplicatePublication, "duplicate publication accepted");
            const auto consumer = ConsumerSubmission(fixture.capture);
            Check(fixture.handle.ObserveConsumerCompletion(ConsumerCompletion(consumer)) == CoordinatorResult::ConsumerNotStarted,
                  "consumer completion without begin/submission accepted");
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "published BeginPresent rejected");
            Check(fixture.handle.ObserveConsumerCompletion(ConsumerCompletion(consumer)) == CoordinatorResult::ConsumerNotStarted,
                  "callback stage alone fabricated native consumer submission");
            Check(fixture.handle.ObserveConsumerSubmission(consumer) == CoordinatorResult::Accepted, "native consumer submission rejected");
            auto noRelease = ConsumerCompletion(consumer); noRelease.leaseReleaseObservationSerial = 0;
            Check(fixture.handle.ObserveConsumerCompletion(noRelease) == CoordinatorResult::ConsumerCompletionMismatch,
                  "GPU completion alone retired resources before consumer lease release");
            auto complete = ConsumerCompletion(consumer); ++complete.submitSerial;
            Check(fixture.handle.ObserveConsumerCompletion(complete) == CoordinatorResult::ConsumerCompletionMismatch, "wrong consumer submit retired owner");
            complete = ConsumerCompletion(consumer); complete.nativeResult = 1;
            Check(fixture.handle.ObserveConsumerCompletion(complete) == CoordinatorResult::NativeConsumerCompletionFailed, "consumer timeout retired owner");
            fixture.coordinator.CloseRuntime();
            Check(!fixture.weakOwner.expired() && fixture.coordinator.DrainPending().size() == 1, "runtime close retired in-flight consumer");
            Check(fixture.handle.ObserveConsumerCompletion(ConsumerCompletion(consumer)) == CoordinatorResult::Accepted, "closed runtime handle could not complete consumer");
            Check(fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1, "completed consumer retained resource or retired twice");
            Check(fixture.handle.ObserveConsumerCompletion(ConsumerCompletion(consumer)) == CoordinatorResult::AlreadyRetired, "retired consumer accepted another completion");
        }
        {
            Fixture fixture; fixture.CompleteCapture();
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::PublicationUnavailable, "unpublished begin implied producer publication");
            Check(fixture.handle.QualifyAndPublish(Semantics()) == CoordinatorResult::PresentAlreadyStarted, "late producer submission accepted");
        }
        {
            Fixture fixture;
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::PublicationUnavailable, "begin without capture completion accepted");
            Check(fixture.handle.ObserveNativeSubmission(Submission(fixture.capture)) == CoordinatorResult::PresentAlreadyStarted,
                  "submission after FG input consumption qualified");
        }
        {
            Fixture fixture; fixture.coordinator.ObserveAcquireMultiplicity(2);
            Check(fixture.handle.ObserveNativeSubmission(Submission(fixture.capture)) == CoordinatorResult::AmbiguousAcquire,
                  "multi-acquire submission was qualified");
            Check(fixture.handle.ObserveGpuCompletion(Completion(Submission(fixture.capture))) == CoordinatorResult::Accepted,
                  "ambiguous submitted work could not be completed for draining");
            Check(fixture.handle.QualifyAndPublish(Semantics()) == CoordinatorResult::AmbiguousAcquire, "multi-acquire invalidation was lost");
        }
        {
            Fixture fixture; auto discarded = RecordingDiscardObservation{fixture.capture.command, fixture.capture.allocationGeneration,
                fixture.capture.recordingGeneration + 1, 10, 0};
            Check(fixture.handle.DiscardUnsubmittedRecording(discarded) == CoordinatorResult::RecordingDiscardUnverified, "wrong reset generation retired owner");
            fixture.coordinator.CloseRuntime();
            Check(!fixture.weakOwner.expired(), "runtime close discarded a recorded command without reset proof");
            discarded.recordingGeneration = fixture.capture.recordingGeneration;
            Check(fixture.handle.DiscardUnsubmittedRecording(discarded) == CoordinatorResult::Accepted, "successful exact reset proof rejected");
            Check(fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1, "discarded recording did not retire owner once");
        }
        {
            Fixture fixture; const auto submit = Submission(fixture.capture);
            Check(fixture.handle.ObserveNativeSubmission(submit) == CoordinatorResult::Accepted, "drain fixture submission failed");
            Check(fixture.handle.DiscardUnsubmittedRecording({fixture.capture.command, fixture.capture.allocationGeneration,
                fixture.capture.recordingGeneration, 200, 0}) == CoordinatorResult::RecordingAlreadySubmitted, "reset fabricated completion of submitted work");
            fixture.coordinator.CloseRuntime();
            Check(fixture.handle.QualifyAndPublish(Semantics()) == CoordinatorResult::RuntimeClosed, "closed runtime dispatched new publication");
            Check(!fixture.weakOwner.expired(), "close freed a pending GPU capture");
            Check(fixture.handle.ObserveGpuCompletion(Completion(submit)) == CoordinatorResult::Accepted, "capture could not finish after close");
            Check(fixture.weakOwner.expired() && fixture.coordinator.DrainPending().empty(), "completed closing capture did not drain");
        }
        {
            Counts counts; ProducerFrameCoordinator::FrameHandle surviving;
            const auto capture = Capture(); const auto submit = Submission(capture); std::weak_ptr<void> weak;
            {
                ProducerFrameCoordinator coordinator(Scope()); auto owner = std::make_shared<Owner>(); owner->counts = &counts; weak = owner;
                Check(coordinator.RecordCapture(capture, owner, &Retire, &counts, surviving) == CoordinatorResult::Accepted, "surviving capture failed");
                Check(surviving.ObserveNativeSubmission(submit) == CoordinatorResult::Accepted, "surviving submission failed");
            }
            Check(!weak.expired() && surviving.Snapshot().runtimeClosed, "coordinator destruction released in-flight owner");
            Check(surviving.ObserveGpuCompletion(Completion(submit)) == CoordinatorResult::Accepted, "orphaned drain handle could not complete");
            Check(weak.expired() && counts.retireCallbacks == 1, "surviving handle did not retire outside runtime lifetime");
        }
        {
            Fixture fixture;
            fixture.coordinator.CloseRuntime();
            Check(fixture.handle.ObserveNativeSubmission(Submission(fixture.capture)) == CoordinatorResult::RuntimeClosed,
                  "closing runtime accepted a new capture dispatch");
            Check(fixture.handle.ObserveNativeSubmission(Submission(fixture.capture), true) == CoordinatorResult::DuplicateSubmission,
                  "repeated closed-runtime native observation was not deduplicated");
            Check(fixture.handle.Snapshot().nativeSubmissionObserved && !fixture.handle.Snapshot().submissionQualified,
                  "late native submission was not retained for safe draining");
            Check(fixture.handle.ObserveGpuCompletion(Completion(Submission(fixture.capture))) == CoordinatorResult::Accepted,
                  "closing runtime could not drain the observed native submission");
            Check(fixture.weakOwner.expired() && !fixture.handle.Snapshot().producerSubmissionQualified,
                  "drained closing submission retained owner or qualification");
        }
        {
            Counts counts; ProducerFrameCoordinator::FrameHandle surviving;
            const auto capture = Capture(); const auto submit = Submission(capture); const auto consumer = ConsumerSubmission(capture);
            std::weak_ptr<void> weak;
            {
                ProducerFrameCoordinator coordinator(Scope()); auto owner = std::make_shared<Owner>(); owner->counts = &counts; weak = owner;
                Check(coordinator.RecordCapture(capture, owner, &Retire, &counts, surviving) == CoordinatorResult::Accepted, "published survival capture failed");
                Check(surviving.ObserveNativeSubmission(submit) == CoordinatorResult::Accepted, "published survival submit failed");
                Check(surviving.ObserveGpuCompletion(Completion(submit)) == CoordinatorResult::Accepted, "published survival complete failed");
                Check(surviving.QualifyAndPublish(Semantics()) == CoordinatorResult::Accepted, "published survival qualification failed");
                Check(surviving.BeginPresent(capture.ticket) == CoordinatorResult::Accepted, "published survival begin failed");
                Check(surviving.ObserveConsumerSubmission(consumer) == CoordinatorResult::Accepted, "published survival consumer submit failed");
            }
            Check(!weak.expired() && !surviving.Snapshot().producerSubmissionQualified, "runtime destruction freed or requalified consumer resources");
            Check(surviving.ObserveConsumerCompletion(ConsumerCompletion(consumer)) == CoordinatorResult::Accepted,
                  "consumer completion could not drain after runtime object destruction");
            Check(weak.expired() && counts.retireCallbacks == 1, "destroyed runtime consumer owner did not retire exactly once");
        }
        {
            Fixture fixture;
            auto next = Capture(2, 1); ProducerFrameCoordinator::FrameHandle second;
            auto owner = std::make_shared<int>(1);
            Check(fixture.coordinator.RecordCapture(next, owner, &Retire, &fixture.counts, second) == CoordinatorResult::AmbiguousAcquire,
                  "a new acquisition was associated before prior Present observation");
            Check(fixture.handle.ObserveNativeSubmission(Submission(fixture.capture)) == CoordinatorResult::AmbiguousAcquire,
                  "ambiguous acquisition did not poison the prior pending capture");
        }
        for (unsigned iteration = 0; iteration < 100; ++iteration)
        {
            Fixture fixture; const auto submit = Submission(fixture.capture);
            Check(fixture.handle.ObserveNativeSubmission(submit) == CoordinatorResult::Accepted, "concurrent close fixture failed");
            CoordinatorResult completed = CoordinatorResult::NativeCompletionFailed;
            std::thread completion([&] { completed = fixture.handle.ObserveGpuCompletion(Completion(submit)); });
            std::thread closing([&] { fixture.coordinator.CloseRuntime(); });
            completion.join(); closing.join();
            Check(completed == CoordinatorResult::Accepted, "concurrent close lost native completion");
            Check(fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1 && fixture.counts.reentrantCallbacks == 1,
                  "concurrent close/completion retired twice or under callback lock");
            Check(fixture.coordinator.DrainPending().empty(), "concurrent completed close left pending owner");
        }
        {
            ProducerFrameCoordinator coordinator(Scope(41, 1)); Counts counts;
            auto capture = Capture(); ProducerFrameCoordinator::FrameHandle first, second; auto owner = std::make_shared<int>(1);
            Check(coordinator.RecordCapture(capture, owner, &Retire, &counts, first) == CoordinatorResult::Accepted, "capacity fixture failed");
            Check(coordinator.RecordCapture(capture, owner, &Retire, &counts, second) == CoordinatorResult::DuplicateCapture, "same ticket was captured twice");
            Check(first.ObserveNativeSubmission(Submission(capture)) == CoordinatorResult::Accepted, "capacity submission failed");
            Check(first.ObserveGpuCompletion(Completion(Submission(capture))) == CoordinatorResult::Accepted, "capacity completion failed");
            Check(first.QualifyAndPublish(Semantics()) == CoordinatorResult::Accepted, "capacity publication failed");
            Check(first.BeginPresent(capture.ticket) == CoordinatorResult::Accepted, "capacity begin failed");
            auto newer = Capture(2, 1);
            Check(coordinator.RecordCapture(newer, owner, &Retire, &counts, second) == CoordinatorResult::CapacityExceeded, "live slot capacity was exceeded");
            const auto consumer = ConsumerSubmission(capture);
            Check(first.ObserveConsumerSubmission(consumer) == CoordinatorResult::Accepted, "capacity consumer submission failed");
            Check(first.ObserveConsumerCompletion(ConsumerCompletion(consumer)) == CoordinatorResult::Accepted, "capacity consumer completion failed");
            auto oldFrame = Capture(2, 0);
            Check(coordinator.RecordCapture(oldFrame, owner, &Retire, &counts, second) == CoordinatorResult::StaleTicket, "new acquire serial reused old frame identity");
            Check(coordinator.RecordCapture(newer, owner, &Retire, &counts, second) == CoordinatorResult::Accepted, "retired slot was not reusable");
            Check(second.DiscardUnsubmittedRecording({newer.command, newer.allocationGeneration, newer.recordingGeneration, 500, 0}) == CoordinatorResult::Accepted,
                  "new slot cleanup failed");
            ProducerFrameCoordinator recreated(Scope(42)); auto stale = Capture(3, 2, 41);
            Check(recreated.RecordCapture(stale, owner, &Retire, &counts, second) == CoordinatorResult::StaleTicket, "retired swapchain generation accepted");
        }
        AbortRejection([](auto &value) { ++value.ticket.registration; });
        AbortRejection([](auto &value) { ++value.ticket.device; });
        AbortRejection([](auto &value) { ++value.ticket.swapchain; });
        AbortRejection([](auto &value) { ++value.ticket.generation; });
        AbortRejection([](auto &value) { ++value.ticket.acquire_id; });
        AbortRejection([](auto &value) { ++value.ticket.image_index; });
        AbortRejection([](auto &value) { ++value.queue; });
        AbortRejection([](auto &value) { value.command = 0; });
        AbortRejection([](auto &value) { value.allocationGeneration = 0; });
        AbortRejection([](auto &value) { value.recordingGeneration = 0; });
        AbortRejection([](auto &value) { value.attemptSerial = 0; });
        AbortRejection([](auto &value) { value.attemptSerial = 101; });
        AbortRejection([](auto &value) { value.disposition = ConsumerAbortDisposition::Unknown; });
        AbortRejection([](auto &value) { value.phase = ProducerPhase::AfterReShadeSubmitBeforeInputConsume; });
        AbortRejection([](auto &value) { value.nativeResult = 0; });
        AbortRejection([](auto &value) { value.nativeResult = 1; });
        AbortRejection([](auto &value) { value.disposition = ConsumerAbortDisposition::AbandonedBeforeNativeSubmission; });
        AbortDrainRejection([](auto &value) { ++value.ticket.generation; });
        AbortDrainRejection([](auto &value) { ++value.ticket.acquire_id; });
        AbortDrainRejection([](auto &value) { ++value.ticket.device; });
        AbortDrainRejection([](auto &value) { ++value.queue; });
        AbortDrainRejection([](auto &value) { ++value.command; });
        AbortDrainRejection([](auto &value) { ++value.allocationGeneration; });
        AbortDrainRejection([](auto &value) { ++value.recordingGeneration; });
        AbortDrainRejection([](auto &value) { ++value.attemptSerial; });
        AbortDrainRejection([](auto &value) { value.disposition = ConsumerAbortDisposition::AbandonedBeforeNativeSubmission; });
        AbortDrainRejection([](auto &value) { value.drainKind = ConsumerDrainKind::Unknown; });
        AbortDrainRejection([](auto &value) { ++value.drainObject; });
        AbortDrainRejection([](auto &value) { value.drainObservationSerial = 0; });
        AbortDrainRejection([](auto &value) { value.drainObservationSerial = value.attemptSerial; });
        AbortDrainRejection([](auto &value) { value.nativeDrainResult = 1; });
        AbortDrainRejection([](auto &value) { value.nativeDrainResult = -4; });
        AbortDrainRejection([](auto &value) { value.leaseReleaseObservationSerial = 0; });
        AbortDrainRejection([](auto &value) { value.leaseReleaseObservationSerial = value.drainObservationSerial; });
        {
            Fixture fixture; fixture.Publish();
            const auto attempt = AbortAttempt(fixture.capture);
            Check(fixture.handle.ObserveConsumerAbort(attempt) == CoordinatorResult::ConsumerAbortUnavailable,
                  "failed consumer attempt accepted before BeginPresent");
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "failed submit begin rejected");
            Check(fixture.handle.RetireAbortedConsumer(AbortDrain(attempt)) == CoordinatorResult::ConsumerAbortUnavailable,
                  "drain and release without a failed/abandoned attempt retired owner");
            auto failedSubmit = ConsumerSubmission(fixture.capture); failedSubmit.nativeResult = -4;
            Check(fixture.handle.ObserveConsumerSubmission(failedSubmit) == CoordinatorResult::NativeConsumerSubmissionFailed,
                  "native consumer submit failure was accepted as successful GPU work");
            Check(fixture.handle.ObserveConsumerAbort(attempt) == CoordinatorResult::Accepted, "explicit failed native attempt rejected");
            Check(fixture.handle.ObserveConsumerAbort(attempt) == CoordinatorResult::DuplicateConsumerAbort, "duplicate failed native attempt accepted");
            Check(!fixture.weakOwner.expired(), "consumer submit failure alone retired owner");
            fixture.coordinator.CloseRuntime();
            Check(!fixture.weakOwner.expired() && fixture.coordinator.DrainPending().size() == 1,
                  "close skipped failed consumer drain requirements");
            Check(fixture.handle.RetireAbortedConsumer(AbortDrain(attempt)) == CoordinatorResult::Accepted,
                  "matching native drain plus final release did not recover failed consumer");
            const auto state = fixture.handle.Snapshot();
            Check(state.consumerAbortDrained && state.retired && !state.consumerCompleted && !state.consumerSubmissionObserved,
                  "failed-consumer retirement fabricated a successful consumer GPU completion");
            Check(fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1 && fixture.counts.reentrantCallbacks == 1,
                  "failed consumer did not retire exactly once outside callback locks");
            Check(fixture.handle.RetireAbortedConsumer(AbortDrain(attempt)) == CoordinatorResult::AlreadyRetired,
                  "failed consumer retired twice");
        }
        for (bool noRecording : {false, true})
        {
            Fixture fixture; fixture.Publish();
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "abandoned consumer begin failed");
            fixture.coordinator.CloseRuntime();
            auto attempt = AbortAttempt(fixture.capture);
            attempt.disposition = ConsumerAbortDisposition::AbandonedBeforeNativeSubmission;
            attempt.nativeResult = NoConsumerNativeSubmissionResult;
            if (noRecording) { attempt.command = 0; attempt.allocationGeneration = 0; attempt.recordingGeneration = 0; }
            Check(fixture.handle.ObserveConsumerAbort(attempt) == CoordinatorResult::Accepted,
                  "closed runtime could not record explicit abandonment");
            auto drain = AbortDrain(attempt); drain.drainKind = ConsumerDrainKind::DeviceIdle; drain.drainObject = Scope().device;
            Check(fixture.handle.RetireAbortedConsumer(drain) == CoordinatorResult::Accepted, "matching successful device drain did not recover abandoned consumer");
            Check(fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1 && !fixture.handle.Snapshot().consumerCompleted,
                  "abandoned consumer retirement was incomplete or misclassified");
        }
        {
            Fixture fixture; fixture.Publish();
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "successful consumer conflict begin failed");
            const auto consumer = ConsumerSubmission(fixture.capture);
            Check(fixture.handle.ObserveConsumerSubmission(consumer) == CoordinatorResult::Accepted, "successful consumer conflict submit failed");
            Check(fixture.handle.ObserveConsumerAbort(AbortAttempt(fixture.capture)) == CoordinatorResult::ConsumerAttemptConflict,
                  "successful consumer submission was reclassified as failed/abandoned");
            Check(fixture.handle.RetireAbortedConsumer(AbortDrain(AbortAttempt(fixture.capture))) != CoordinatorResult::Accepted,
                  "unrelated failed-consumer drain bypassed successful consumer completion");
            Check(!fixture.weakOwner.expired(), "failed-consumer conflict retired a successful pending consumer");
        }
        {
            Fixture fixture; fixture.Publish();
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "post-abort successful submit begin failed");
            const auto attempt = AbortAttempt(fixture.capture);
            Check(fixture.handle.ObserveConsumerAbort(attempt) == CoordinatorResult::Accepted, "post-abort successful submit attempt failed");
            const auto consumer = ConsumerSubmission(fixture.capture, 350);
            Check(fixture.handle.ObserveConsumerSubmission(consumer) == CoordinatorResult::ConsumerAttemptConflict &&
                  fixture.handle.Snapshot().consumerSubmissionObserved, "real successful native submit after abort was not retained as in-flight conflict");
            Check(fixture.handle.RetireAbortedConsumer(AbortDrain(attempt)) == CoordinatorResult::ConsumerAttemptConflict,
                  "abort path retired an observed later successful consumer submission");
            Check(!fixture.weakOwner.expired(), "successful submit conflict lost owner before GPU completion");
            Check(fixture.handle.ObserveConsumerCompletion(ConsumerCompletion(consumer)) == CoordinatorResult::Accepted,
                  "successful submit conflict could not drain through exact consumer completion");
            Check(fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1,
                  "successful submit conflict owner did not retire exactly once");
        }
        for (unsigned iteration = 0; iteration < 50; ++iteration)
        {
            Fixture fixture; fixture.Publish();
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "concurrent abort begin failed");
            const auto attempt = AbortAttempt(fixture.capture);
            Check(fixture.handle.ObserveConsumerAbort(attempt) == CoordinatorResult::Accepted, "concurrent abort observation failed");
            CoordinatorResult result = CoordinatorResult::NativeConsumerDrainFailed;
            std::thread drained([&] { result = fixture.handle.RetireAbortedConsumer(AbortDrain(attempt)); });
            std::thread closing([&] { fixture.coordinator.CloseRuntime(); });
            drained.join(); closing.join();
            Check(result == CoordinatorResult::Accepted && fixture.weakOwner.expired() && fixture.counts.retireCallbacks == 1 &&
                  fixture.counts.reentrantCallbacks == 1, "concurrent close/failed-consumer drain leaked or retired twice");
            Check(fixture.coordinator.DrainPending().empty() && !fixture.handle.Snapshot().consumerCompleted,
                  "failed-consumer drain remained pending or fabricated successful consumer work");
        }
        ConsumerSuccessSafety([](auto &value) { ++value.queue; });
        ConsumerSuccessSafety([](auto &value) { value.command = 0; });
        ConsumerSuccessSafety([](auto &value) { value.allocationGeneration = 0; });
        ConsumerSuccessSafety([](auto &value) { value.recordingGeneration = 0; });
        ConsumerSuccessSafety([](auto &value) { value.completionKind = CompletionKind::Unknown; });
        ConsumerSuccessSafety([](auto &value) { value.completionObject = 0; });
        ConsumerSuccessSafety([](auto &value) { value.nativeDevice = 0; });
        ConsumerSuccessSafety([](auto &value) { value.submitSerial = 0; });
        ConsumerSuccessSafety([](auto &) {}, true);
        {
            Fixture fixture; fixture.Publish();
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "foreign exact completion begin failed");
            auto foreign = ConsumerSubmission(fixture.capture); ++foreign.queue;
            Check(fixture.handle.ObserveConsumerSubmission(foreign) == CoordinatorResult::ConsumerSubmissionMismatch,
                  "foreign queue was treated as qualified scope submission");
            Check(fixture.handle.ObserveConsumerCompletion(ConsumerCompletion(foreign)) == CoordinatorResult::Accepted,
                  "exact successful foreign queue completion and release could not retire its work");
            Check(fixture.weakOwner.expired(), "completed foreign queue work leaked despite exact completion proof");
        }
        {
            Fixture fixture; const auto first = Submission(fixture.capture);
            Check(fixture.handle.ObserveNativeSubmission(first) == CoordinatorResult::Accepted, "producer resubmit first failed");
            Check(fixture.handle.ObserveNativeSubmission(first) == CoordinatorResult::DuplicateSubmission &&
                  !fixture.handle.Snapshot().producerNativeWorkHazard, "exact same producer observation created a false extra-work hazard");
            auto second = Submission(fixture.capture, 200); ++second.completionObject;
            Check(fixture.handle.ObserveNativeSubmission(second) == CoordinatorResult::NativeWorkUnproven,
                  "distinct producer native success was discarded as an ordinary duplicate");
            auto complete = Completion(first); complete.completionObservationSerial = 201;
            Check(fixture.handle.ObserveGpuCompletion(complete) == CoordinatorResult::Accepted, "first producer signal could not be recorded");
            Check(fixture.handle.Snapshot().producerNativeWorkHazard && !fixture.handle.Snapshot().completedCapture,
                  "first producer signal cleared the second producer work hazard");
            Check(fixture.handle.RetireCompletedCapture() == CoordinatorResult::NativeWorkUnproven,
                  "first producer signal retired a different in-flight producer submission");
            Check(fixture.handle.QualifyAndPublish(Semantics()) == CoordinatorResult::NativeWorkUnproven,
                  "resubmitted producer content was published using the first signal");
            fixture.coordinator.CloseRuntime();
            Check(!fixture.weakOwner.expired(), "close retired resubmitted producer work on the first signal");
            auto drain = DeviceDrain(fixture.capture); drain.deviceIdleObservationSerial = 201;
            Check(fixture.handle.ObserveNativeDeviceDrain(drain) == CoordinatorResult::NativeDeviceDrainMismatch,
                  "device idle not later than known producer completion was accepted");
            Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::Accepted && fixture.weakOwner.expired(),
                  "resubmitted producer work could not drain through later successful device idle");
        }
        {
            Fixture fixture; fixture.Publish();
            const auto first = ConsumerSubmission(fixture.capture);
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "consumer resubmit begin failed");
            Check(fixture.handle.ObserveConsumerSubmission(first) == CoordinatorResult::Accepted, "consumer resubmit first failed");
            Check(fixture.handle.ObserveConsumerSubmission(first) == CoordinatorResult::DuplicateConsumerSubmission &&
                  !fixture.handle.Snapshot().consumerNativeWorkHazard, "exact consumer observation was treated as extra work");
            auto second = ConsumerSubmission(fixture.capture, 400); ++second.queue; ++second.completionObject;
            Check(fixture.handle.ObserveConsumerSubmission(second) == CoordinatorResult::NativeWorkUnproven,
                  "second successful foreign consumer was discarded as an ordinary duplicate");
            Check(fixture.handle.ObserveConsumerCompletion(ConsumerCompletion(first)) == CoordinatorResult::NativeWorkUnproven,
                  "first consumer completion retired a second outstanding submission");
            fixture.coordinator.CloseRuntime();
            Check(!fixture.weakOwner.expired(), "close cleared consumer extra-work hazard");
            auto missingRelease = DeviceDrain(fixture.capture); missingRelease.leaseReleaseObservationSerial = 0;
            Check(fixture.handle.ObserveNativeDeviceDrain(missingRelease) == CoordinatorResult::NativeDeviceDrainMismatch,
                  "device idle alone retired a published consumer lease");
            auto failedIdle = DeviceDrain(fixture.capture); failedIdle.nativeResult = -4;
            Check(fixture.handle.ObserveNativeDeviceDrain(failedIdle) == CoordinatorResult::NativeDeviceDrainFailed,
                  "failed device idle retired consumer work");
            Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::Accepted && fixture.weakOwner.expired(),
                  "all known-device consumer work did not retire after later idle plus final release");
        }
        {
            Fixture fixture; fixture.Publish();
            auto second = Submission(fixture.capture, 200); ++second.queue;
            Check(fixture.handle.ObserveNativeSubmission(second) == CoordinatorResult::NativeWorkUnproven,
                  "post-publication producer resubmit was ignored");
            Check(fixture.handle.ReleaseUnconsumedPublication({fixture.capture.ticket, 900, true}) != CoordinatorResult::Accepted,
                  "unconsumed publication release discarded extra producer work");
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::NativeWorkUnproven &&
                  fixture.handle.Snapshot().presentBegun, "published stale bit authorized consumption after producer resubmission hazard");
            Check(fixture.handle.ObserveConsumerAbort(AbortAttempt(fixture.capture)) == CoordinatorResult::Accepted, "producer hazard abort fixture failed");
            Check(fixture.handle.RetireAbortedConsumer(AbortDrain(AbortAttempt(fixture.capture))) == CoordinatorResult::NativeWorkUnproven,
                  "scope queue abort idle discarded extra foreign producer work");
            fixture.coordinator.CloseRuntime();
            Check(!fixture.weakOwner.expired(), "close discarded foreign producer work after publication");
            Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::Accepted,
                  "published producer extra work did not device-drain");
        }
        {
            Fixture fixture; fixture.CompleteCapture();
            Check(fixture.handle.ObserveConsumerSubmission(ConsumerSubmission(fixture.capture)) == CoordinatorResult::ConsumerNotStarted,
                  "consumer success before publication was qualified");
            Check(fixture.handle.Snapshot().consumerSubmissionObserved, "consumer success before publication was forgotten");
            Check(fixture.handle.RetireCompletedCapture() == CoordinatorResult::ConsumerNotStarted,
                  "ordinary completed-capture retirement discarded unexpected consumer work");
            fixture.coordinator.CloseRuntime();
            Check(!fixture.weakOwner.expired(), "close discarded consumer work observed before publication");
            Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::Accepted,
                  "unexpected consumer work could not explicitly device-drain after close");
        }
        {
            Fixture fixture; fixture.Publish();
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "abort strong-drain order fixture begin failed");
            const auto attempt = AbortAttempt(fixture.capture);
            Check(fixture.handle.ObserveConsumerAbort(attempt) == CoordinatorResult::Accepted, "abort strong-drain order attempt failed");
            fixture.coordinator.CloseRuntime();
            auto beforeAbort = DeviceDrain(fixture.capture); beforeAbort.deviceIdleObservationSerial = 200;
            Check(fixture.handle.ObserveNativeDeviceDrain(beforeAbort) == CoordinatorResult::NativeDeviceDrainMismatch,
                  "strong idle before known abort bypassed attempt ordering");
            Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::Accepted,
                  "later strong idle could not drain the known abort");
        }
        {
            Fixture fixture; fixture.Publish();
            Check(fixture.handle.ReleaseUnconsumedPublication({fixture.capture.ticket, 100, true}) != CoordinatorResult::Accepted &&
                  fixture.handle.ReleaseUnconsumedPublication({fixture.capture.ticket, 101, true}) != CoordinatorResult::Accepted,
                  "unconsumed release before or at producer GPU completion was accepted");
            Check(!fixture.weakOwner.expired(), "stale release retired publication owner");
            Check(fixture.handle.ReleaseUnconsumedPublication({fixture.capture.ticket, 102, true}) == CoordinatorResult::Accepted,
                  "unconsumed release later than producer completion was rejected");
        }
        for (bool producer : {false, true})
        {
            Fixture fixture;
            if (producer)
            {
                for (uint64_t serial = 100; serial < 120; ++serial)
                {
                    const auto native = Submission(fixture.capture, serial);
                    const auto result = fixture.handle.ObserveNativeSubmission(native);
                    Check(serial == 100 ? result == CoordinatorResult::Accepted : result == CoordinatorResult::NativeWorkUnproven,
                          "producer fact overflow lost distinct success");
                }
            }
            else
            {
                fixture.Publish();
                Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted, "consumer overflow begin failed");
                for (uint64_t serial = 300; serial < 320; ++serial)
                {
                    const auto result = fixture.handle.ObserveConsumerSubmission(ConsumerSubmission(fixture.capture, serial));
                    Check(serial == 300 ? result == CoordinatorResult::Accepted : result == CoordinatorResult::NativeWorkUnproven,
                          "consumer fact overflow lost distinct success");
                }
            }
            Check(fixture.handle.Snapshot().successfulNativeWorkFactsOverflow && fixture.handle.Snapshot().successfulNativeWorkFacts == 16,
                  "successful native fact storage was not bounded");
            fixture.coordinator.CloseRuntime();
            Check(!fixture.weakOwner.expired(), "overflowing native facts released owner");
            Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::Accepted,
                  "bounded native fact overflow lost known-device drain recovery");
        }
        {
            Fixture fixture; fixture.Publish(); fixture.handle.Invalidate();
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::NativeWorkUnproven && fixture.handle.Snapshot().presentBegun,
                  "invalidated publication authorized input consumption");
            Check(!fixture.weakOwner.expired(), "denied present intent retired a publication lease");
        }
        {
            Fixture fixture; fixture.Publish();
            auto failed = ConsumerSubmission(fixture.capture); failed.nativeResult = -4;
            Check(fixture.handle.ObserveConsumerSubmission(failed) == CoordinatorResult::NativeConsumerSubmissionFailed,
                  "failed pre-present consumer attempt was qualified");
            Check(!fixture.handle.Snapshot().captureReady && !fixture.handle.Snapshot().producerSubmissionQualified,
                  "consumer attempt left an already published input available for fresh producer consumption");
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::NativeWorkUnproven,
                  "failed pre-present native attempt did not block further input consumption");
            fixture.coordinator.CloseRuntime();
            Check(fixture.handle.ObserveNativeDeviceDrain(DeviceDrain(fixture.capture)) == CoordinatorResult::Accepted,
                  "failed pre-present attempt could not drain with final lease release");
        }
        {
            Fixture fixture; const auto submit = StrictSubmission(fixture.capture);
            Check(fixture.handle.ObserveNativeSubmission(submit) == CoordinatorResult::Accepted,
                  "strict native sync metadata was rejected");
            auto completion = Completion(submit); completion.sync = submit.sync; completion.completionObservationSerial = 101;
            Check(fixture.handle.ObserveGpuCompletion(completion) == CoordinatorResult::Accepted,
                  "strict native sync completion was rejected");
        }
        {
            const std::function<void(NativeSubmissionObservation &)> invalid[] = {
                [](auto &value) { value.sync.queueFamilyIndex = UnknownQueueFamily; },
                [](auto &value) { value.sync.submitBatchIndex = value.sync.submitBatchCount; },
                [](auto &value) { ++value.submittedWaitDetails[0].value; },
                [](auto &value) { value.submittedWaitDetails[1].stageMask = 0; },
                [](auto &value) { value.signalDetailCount = 2; value.signalDetails[1] = value.signalDetails[0]; }
            };
            for (const auto &mutate : invalid)
            {
                Fixture fixture; auto submit = StrictSubmission(fixture.capture); mutate(submit);
                Check(fixture.handle.ObserveNativeSubmission(submit) == CoordinatorResult::NativeSyncMetadataInvalid,
                      "invalid strict native sync metadata was accepted");
                Check(!fixture.handle.Snapshot().captureReady && !fixture.weakOwner.expired(),
                      "invalid strict native sync metadata released capture owner");
            }
        }
        {
            Fixture fixture; const auto submit = StrictSubmission(fixture.capture);
            Check(fixture.handle.ObserveNativeSubmission(submit) == CoordinatorResult::Accepted,
                  "strict completion mismatch fixture submit failed");
            auto completion = Completion(submit); completion.sync = submit.sync; ++completion.sync.ownershipTransferEpoch;
            completion.completionObservationSerial = 101;
            Check(fixture.handle.ObserveGpuCompletion(completion) == CoordinatorResult::NativeSyncMetadataMismatch,
                  "completion with a different sync identity was accepted");
            Check(!fixture.handle.Snapshot().gpuCompletionObserved && !fixture.weakOwner.expired(),
                  "completion sync mismatch released producer owner");
        }
        {
            Fixture fixture; fixture.Publish();
            Check(fixture.handle.BeginPresent(fixture.capture.ticket) == CoordinatorResult::Accepted,
                  "strict consumer sync fixture begin failed");
            const auto submit = StrictConsumerSubmission(fixture.capture);
            Check(fixture.handle.ObserveConsumerSubmission(submit) == CoordinatorResult::Accepted,
                  "strict consumer sync metadata was rejected");
            auto completion = ConsumerCompletion(submit); completion.sync = submit.sync;
            completion.completionObservationSerial = 301; completion.leaseReleaseObservationSerial = 302;
            Check(fixture.handle.ObserveConsumerCompletion(completion) == CoordinatorResult::Accepted,
                  "strict consumer sync completion was rejected");
        }
        TestDispatchLifetime();
        WriteReport(true);
        std::cout << "ProducerFrameCoordinatorContract passed " << Checks << " checks, CPU mock only\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        WriteReport(false, error.what()); std::cerr << "ProducerFrameCoordinatorContract failed: " << error.what() << '\n'; return 1;
    }
}
