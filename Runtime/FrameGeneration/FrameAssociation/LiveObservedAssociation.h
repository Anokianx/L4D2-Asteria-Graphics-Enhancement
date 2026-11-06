#pragma once
#include "ObservedFrameAssociation.h"
#include "../Layer/AsteriaDescriptorEvidenceApi.h"
#include <json.hpp>
#include <memory>
#include <unordered_map>

namespace asteria_live_association
{
using Json = nlohmann::json;
using namespace asteria_frame_association;

struct DrawMetadata
{
    uint64_t owner = 0, nativeDevice = 0, nativeCommand = 0, observerInterval = 0;
    uint64_t sceneEpoch = 0;
    bool sceneEpochVerified = false;
    uint64_t ordinal = 0, sequence = 0, tickNanoseconds = 0;
    uint64_t depth = 0, color = 0, pipeline = 0;
    uint32_t width = 0, height = 0;
    bool viewportKnown = false;
    bool viewportDrawStateVerified = false;
    uint32_t nativeViewportSource = 0;
    uint64_t nativeViewportSetSequence = 0;
    Json pipelineViewport=Json::object();
    std::array<float, 6> viewport{};
    PresentTicketEvidence presentTicket;
};

class Bridge
{
    struct Pipeline { uint64_t generation = 0; std::string profile; };
    struct Command
    {
        uint64_t lifetime = 0, nativeGeneration = 0, nativeRecording = 0, collectorGeneration = 0;
        uint64_t observedInterval = UINT64_MAX, submittedInterval = UINT64_MAX, submissions = 0;
        bool unsupported = false;
    };
    struct Device
    {
        uint64_t native = 0, lastInterval = 0, presentations = 0, accepted = 0, rejected = 0;
        uint64_t sceneEpoch = 0;
        uint64_t trackingResets = 0;
        Json lastTrackingReset = Json::object();
        bool stickyLoss = false;
        std::unique_ptr<Collector> collector;
        std::map<uint64_t, ResourceIdentity> resources;
        std::map<uint64_t, Pipeline> pipelines;
        std::map<uint64_t, Command> commands;
        std::map<std::string, uint64_t> losses, totals;
        std::set<uint64_t> touched;
        std::set<uint64_t> touchedPipelines;
        Association last;
        Json lastNative = Json::object(), lastLosses = Json::object();
    };
    mutable std::mutex mutex;
    std::map<uint64_t, Device> devices;
    uint64_t generation = 0, missingDevices = 0;
    std::vector<std::string> profiles;

    static void Lose(Device &device, const std::string &reason)
    {
        ++device.losses[reason]; ++device.totals[reason]; device.collector->MarkCoverageLoss();
    }
    Device *Find(uint64_t owner)
    {
        auto value = devices.find(owner);
        if (value == devices.end()) { ++missingDevices; return nullptr; }
        return &value->second;
    }
    static Json Identity(const ResourceIdentity &value)
    {
        return {{"device", value.device}, {"handle", value.handle}, {"generation", value.generation},
                {"handle_domain", "ReShadeResource"}, {"width", value.width}, {"height", value.height},
                {"samples", value.samples}, {"format", value.format}};
    }
    static Json SnapshotValue(const Snapshot &value)
    {
        return {{"device", value.device}, {"scene_epoch", value.scene}, {"observer_interval", value.observerIntervalId},
                {"present_observer", value.presentObserverId}, {"depth", Identity(value.depth)}, {"color", Identity(value.color)},
                {"draws", value.observedDraws}, {"world_to_clip", value.camera.worldToClip},
                {"first_command", value.firstDraw.command}, {"first_recording_generation", value.firstDraw.commandGeneration},
                {"first_ordinal", value.firstDraw.ordinal}, {"last_command", value.lastDraw.command},
                {"last_recording_generation", value.lastDraw.commandGeneration}, {"last_ordinal", value.lastDraw.ordinal},
                {"first_pipeline", value.firstPipeline}, {"first_pipeline_generation", value.firstPipelineGeneration},
                {"last_pipeline", value.lastPipeline}, {"last_pipeline_generation", value.lastPipelineGeneration},
                {"first_shader_sha256", value.firstNativeShaderSha256}, {"last_shader_sha256", value.lastNativeShaderSha256},
                {"present_ticket", TicketValue(value.presentTicket)}};
    }
    static Json TicketValue(const PresentTicketEvidence &value)
    {
        return {{"verified", value.verified}, {"device", value.device}, {"owner", value.owner}, {"command", value.command},
                {"swapchain", value.swapchain}, {"swapchain_generation", value.swapchainGeneration}, {"image_index", value.imageIndex},
                {"acquire_serial", value.acquireSerial}, {"submit_serial", value.submitSerial}};
    }
public:
    void RegisterProfile(const std::string &hash)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (std::find(profiles.begin(), profiles.end(), hash) == profiles.end()) profiles.push_back(hash);
        for (auto &entry : devices) entry.second.collector->RegisterNativeProfile(hash);
    }
    void InitDevice(uint64_t owner, uint64_t native)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!owner || !native || (!devices.count(owner) && devices.size() >= 8)) { ++missingDevices; return; }
        Device value; value.native = native; value.collector = std::make_unique<Collector>(native);
        for (const auto &profile : profiles) value.collector->RegisterNativeProfile(profile);
        const auto old = devices.find(owner);
        if (old != devices.end())
        {
            const auto &previous = old->second;
            value.trackingResets = previous.trackingResets + 1;
            value.lastTrackingReset = {{"reason", "same_owner_device_reinitialized"}, {"owner", owner},
                {"previous_native_device", previous.native}, {"new_native_device", native},
                {"previous_presentations", previous.presentations}, {"previous_closed_observer_interval", previous.lastInterval},
                {"discarded_resources", previous.resources.size()}, {"discarded_pipelines", previous.pipelines.size()},
                {"discarded_commands", previous.commands.size()}, {"discarded_pending_coverage_losses", previous.losses}};
            value.totals = previous.totals;
            ++value.totals["device_tracking_reset"];
            value.losses["device_tracking_reset"] = 1;
        }
        auto [entry, inserted] = devices.insert_or_assign(owner, std::move(value));
        if (!inserted) entry->second.collector->MarkCoverageLoss();
    }
    void DestroyDevice(uint64_t owner) { std::lock_guard<std::mutex> lock(mutex); devices.erase(owner); }
    void CoverageLoss(uint64_t owner, const std::string &reason)
    {
        std::lock_guard<std::mutex> lock(mutex); if (auto *device = Find(owner)) Lose(*device, reason);
    }
    void InitResource(uint64_t owner, uint64_t handle, uint32_t width, uint32_t height, uint32_t samples, uint32_t format)
    {
        std::lock_guard<std::mutex> lock(mutex); auto *device = Find(owner); if (!device) return;
        auto old = device->resources.find(handle);
        if (old != device->resources.end()) device->collector->RetireResource(old->second);
        if (!handle || !width || !height || !samples || !format) { device->resources.erase(handle); return; }
        if (!device->resources.count(handle) && device->resources.size() >= 8192)
        { device->stickyLoss = true; Lose(*device, "resource_capacity"); return; }
        device->resources[handle] = {device->native, handle, ++generation, HandleDomain::ReShadeResource, width, height, samples, format};
    }
    void DestroyResource(uint64_t owner, uint64_t handle)
    {
        std::lock_guard<std::mutex> lock(mutex); auto *device = Find(owner); if (!device) return;
        auto value = device->resources.find(handle);
        if (value != device->resources.end()) { device->collector->RetireResource(value->second); device->resources.erase(value); }
    }
    void InitPipeline(uint64_t owner, uint64_t handle, const std::string &profile)
    {
        std::lock_guard<std::mutex> lock(mutex); auto *device = Find(owner); if (!device) return;
        if (!device->pipelines.count(handle) && device->pipelines.size() >= 4096)
        { device->stickyLoss = true; Lose(*device, "pipeline_capacity"); return; }
        if (device->pipelines.count(handle)) Lose(*device, "pipeline_reinitialized");
        device->pipelines[handle] = {++generation, profile};
    }
    void DestroyPipeline(uint64_t owner, uint64_t handle)
    {
        std::lock_guard<std::mutex> lock(mutex); auto *device = Find(owner); if (!device) return;
        if (device->pipelines.erase(handle) && device->touchedPipelines.erase(handle)) Lose(*device, "pipeline_retired_during_open_interval");
    }
    void InitCommand(uint64_t owner, uint64_t command)
    {
        std::lock_guard<std::mutex> lock(mutex); auto *device = Find(owner); if (!device) return;
        if (!device->commands.count(command) && device->commands.size() >= 512)
        { device->stickyLoss = true; Lose(*device, "command_capacity"); return; }
        if (device->commands.count(command)) Lose(*device, "command_reinitialized");
        Command value; value.lifetime = ++generation; device->commands[command] = value;
    }
    void ResetCommand(uint64_t owner, uint64_t command, bool destroy = false)
    {
        std::lock_guard<std::mutex> lock(mutex); auto *device = Find(owner); if (!device) return;
        auto value = device->commands.find(command); if (value == device->commands.end()) return;
        device->collector->CommandReset(command, value->second.collectorGeneration);
        if (device->touched.count(command)) Lose(*device, "command_reset_before_interval_closed");
        const uint64_t lifetime = value->second.lifetime;
        if (destroy) device->commands.erase(value);
        else { value->second = {}; value->second.lifetime = lifetime; }
    }
    void SubmitCommand(uint64_t owner, uint64_t command, uint64_t interval)
    {
        std::lock_guard<std::mutex> lock(mutex); auto *device = Find(owner); if (!device) return;
        auto value = device->commands.find(command);
        if (value == device->commands.end()) { Lose(*device, "unknown_command_submission"); return; }
        auto &record = value->second;
        if (record.observedInterval != UINT64_MAX)
        {
            if (record.observedInterval != interval) Lose(*device, "deferred_recording_submission");
            if (record.submissions) Lose(*device, "recording_resubmitted");
            ++record.submissions; record.submittedInterval = interval;
        }
    }
    void UnsupportedCommand(uint64_t owner, uint64_t command, const std::string &reason)
    {
        std::lock_guard<std::mutex> lock(mutex); auto *device = Find(owner); if (!device) return;
        const auto value = device->commands.find(command);
        if (value != device->commands.end()) value->second.unsupported = true;
        Lose(*device, reason);
    }
    bool Record(const DrawMetadata &metadata, const AsteriaBoundBufferDescriptorV1 &native,
                const std::string &profile, const Matrix &matrix, bool stable)
    {
        std::lock_guard<std::mutex> lock(mutex); auto *device = Find(metadata.owner); if (!device) return false;
        const auto fail = [&](const char *reason) { ++device->rejected; Lose(*device, reason); return false; };
        if (metadata.nativeDevice != device->native || native.status != ASTERIA_DESCRIPTOR_OK ||
            !native.command_generation || !native.recording_generation || !stable)
            return fail("native_recording_or_copy_unverified");
        const auto depth = device->resources.find(metadata.depth), color = device->resources.find(metadata.color);
        const auto pipeline = device->pipelines.find(metadata.pipeline);
        const auto command = device->commands.find(metadata.nativeCommand);
        const Json depthEvidence = depth == device->resources.end() ? Json(nullptr) : Identity(depth->second);
        const Json colorEvidence = color == device->resources.end() ? Json(nullptr) : Identity(color->second);
        device->lastNative = {{"scene_epoch", metadata.sceneEpoch}, {"scene_epoch_verified", metadata.sceneEpochVerified && metadata.sceneEpoch != 0},
            {"observer_only", true}, {"native_command", metadata.nativeCommand},
            {"native_command_allocation_generation", native.command_generation}, {"native_command_recording_generation", native.recording_generation},
            {"native_device_generation",native.device_generation},
            {"native_descriptor_status", native.status}, {"native_descriptor_bind_sequence", native.bind_sequence},
            {"draw_ordinal", metadata.ordinal}, {"observer_interval", metadata.observerInterval},
            {"recording_sequence", metadata.sequence}, {"recording_tick_nanoseconds", metadata.tickNanoseconds},
            {"pipeline", metadata.pipeline}, {"pipeline_generation", pipeline == device->pipelines.end() ? Json(nullptr) : Json(pipeline->second.generation)},
            {"profile_shader_sha256", profile}, {"depth_handle", metadata.depth}, {"color_handle", metadata.color},
            {"depth_identity", depthEvidence}, {"color_identity", colorEvidence}, {"viewport_known", metadata.viewportKnown},
            {"viewport", metadata.viewport}, {"width", metadata.width}, {"height", metadata.height},
            {"viewport_draw_state_verified", metadata.viewportDrawStateVerified},
            {"pipeline_viewport", metadata.pipelineViewport},
            {"native_viewport_source", metadata.nativeViewportSource}, {"native_viewport_set_sequence", metadata.nativeViewportSetSequence},
            {"depth_generation", depth == device->resources.end() ? Json(nullptr) : Json(depth->second.generation)},
            {"color_generation", color == device->resources.end() ? Json(nullptr) : Json(color->second.generation)},
            {"pipeline_generation_origin", pipeline == device->pipelines.end() ? Json(nullptr) : Json("observed_init_pipeline")},
            {"resource_generation_origin", depth != device->resources.end() && color != device->resources.end() ? Json("observed_init_resource") : Json(nullptr)},
            {"present_ticket", TicketValue(metadata.presentTicket)}};
        if (!metadata.sceneEpoch || !metadata.sceneEpochVerified) return fail("scene_epoch_unknown");
        if (!metadata.viewportKnown) return fail("viewport_unobserved");
        if (!metadata.viewportDrawStateVerified) return fail("viewport_draw_state_unverified");
        if (device->sceneEpoch && device->sceneEpoch != metadata.sceneEpoch)
        {
            device->collector->Reset(); device->sceneEpoch = metadata.sceneEpoch;
            return fail("scene_epoch_changed");
        }
        if (!device->sceneEpoch) device->sceneEpoch = metadata.sceneEpoch;
        if (depth == device->resources.end() || color == device->resources.end()) return fail("attachment_creation_unobserved");
        if (!metadata.width || !metadata.height || depth->second.width != metadata.width || depth->second.height != metadata.height ||
            color->second.width != metadata.width || color->second.height != metadata.height ||
            depth->second.samples != color->second.samples)
            return fail("attachment_extent_or_sample_mismatch");
        if (pipeline == device->pipelines.end() || pipeline->second.profile != profile || profile.empty()) return fail("pipeline_creation_or_profile_unverified");
        if (command == device->commands.end() || command->second.unsupported) return fail("command_creation_or_direct_recording_unverified");
        auto &record = command->second;
        if (record.nativeGeneration != native.command_generation || record.nativeRecording != native.recording_generation)
        {
            if (record.collectorGeneration && device->touched.count(metadata.nativeCommand)) return fail("recording_changed_inside_interval");
            record.nativeGeneration = native.command_generation; record.nativeRecording = native.recording_generation;
            record.collectorGeneration = ++generation; record.submissions = 0; record.submittedInterval = UINT64_MAX;
        }
        record.observedInterval = metadata.observerInterval;
        device->touched.insert(metadata.nativeCommand);
        device->touchedPipelines.insert(metadata.pipeline);
        for (float value : metadata.viewport) if (!std::isfinite(value)) return fail("viewport_nonfinite");
        if (metadata.viewport[0] != 0 || metadata.viewport[1] != 0 || metadata.viewport[2] != float(metadata.width) ||
            metadata.viewport[3] != float(metadata.height) || metadata.viewport[4] != 0 || metadata.viewport[5] != 1)
            return fail("viewport_not_full_attachment");
        DrawObservation draw{};
        draw.device = device->native; draw.scene = metadata.sceneEpoch; draw.observerIntervalId = metadata.observerInterval;
        draw.draw = {metadata.nativeCommand, record.collectorGeneration, metadata.ordinal, metadata.sequence, metadata.tickNanoseconds};
        draw.depth = depth->second; draw.color = color->second;
        draw.viewportWidth = metadata.width; draw.viewportHeight = metadata.height;
        draw.pipeline = metadata.pipeline; draw.pipelineGeneration = pipeline->second.generation; draw.nativeShaderSha256 = profile;
        draw.worldToClip = matrix; draw.nativeBindingVerified = true; draw.stableMatrixCopy = stable;
        draw.directPrimaryRecording = true; draw.observedOutsideEffects = true; draw.depthAttachment = true; draw.colorAttachment = true;
        draw.presentTicket = metadata.presentTicket;
        const auto status = device->collector->ObserveDraw(draw);
        device->lastNative["collector_recording_generation"] = record.collectorGeneration;
        device->lastNative["depth_generation"] = depth->second.generation;
        device->lastNative["color_generation"] = color->second.generation;
        device->lastNative["pipeline_generation_origin"] = "observed_init_pipeline";
        if (status != Status::AcceptedObservation && status != Status::ExcludedPass) return fail(Name(status));
        ++device->accepted; return true;
    }
    void Present(uint64_t owner, uint64_t nextInterval, uint32_t width, uint32_t height, uint64_t tick, uint64_t sceneEpoch = 0,
                 bool sceneEpochVerified = false, PresentTicketEvidence presentTicket = {})
    {
        std::lock_guard<std::mutex> lock(mutex); auto *device = Find(owner); if (!device || !nextInterval) return;
        const uint64_t interval = nextInterval - 1;
        if (!sceneEpoch || !sceneEpochVerified) Lose(*device, "scene_epoch_unknown");
        for (uint64_t command : device->touched)
        {
            const auto record = device->commands.find(command);
            if (record == device->commands.end() || record->second.submittedInterval != interval)
                Lose(*device, "recording_not_submitted_in_interval");
        }
        if (device->stickyLoss) Lose(*device, "persistent_tracking_capacity_loss");
        PresentObservation present{};
        present.device = device->native; present.scene = sceneEpochVerified ? sceneEpoch : 0;
        present.sceneCut = device->sceneEpoch && sceneEpochVerified && device->sceneEpoch != sceneEpoch;
        present.closedObserverIntervalId = interval;
        present.presentObserverId = nextInterval; present.tickNanoseconds = tick; present.width = width; present.height = height;
        present.presentTicket = presentTicket;
        present.recordingCoverageComplete = device->losses.empty();
        device->last = device->collector->OnPresent(present);
        device->lastLosses = device->losses; device->losses.clear(); device->touched.clear();
        device->lastInterval = interval; if (sceneEpoch && sceneEpochVerified) device->sceneEpoch = sceneEpoch;
        device->touchedPipelines.clear(); ++device->presentations;
    }
    Json Snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        Json result = {{"author", "Anokianx"}, {"scope", "live_draw_recording_observer_association"}, {"gpu_frame_aligned", false},
            {"main_view_verified", false}, {"native_camera_ready", false}, {"game_fg_ready", false}, {"device_tracking_failures", missingDevices},
            {"execution_events_are_submission_intent_only", true}, {"devices", Json::array()}};
        for (const auto &entry : devices)
        {
            const auto &device = entry.second;
            result["devices"].push_back({{"owner", entry.first}, {"native_device", device.native}, {"present_observations", device.presentations},
                {"closed_observer_interval", device.lastInterval}, {"status", Name(device.last.status)},
                {"observed_association_available", device.last.observedAssociationAvailable}, {"reset_history", device.last.resetHistory},
                {"present_ticket_verified", device.last.presentTicketVerified}, {"present_ticket", TicketValue(device.last.presentTicket)},
                {"accepted_recordings", device.accepted}, {"rejected_recordings", device.rejected},
                {"tracking_reset_count", device.trackingResets}, {"last_tracking_reset", device.lastTrackingReset},
                {"last_interval_coverage_losses", device.lastLosses}, {"pending_coverage_losses", device.losses}, {"total_coverage_losses", device.totals},
                {"last_native_recording_evidence", device.lastNative}, {"current", SnapshotValue(device.last.current)}, {"previous", SnapshotValue(device.last.previous)},
                {"gpu_frame_aligned", false}, {"main_view_verified", false}, {"native_camera_ready", false}, {"game_fg_ready", false}});
        }
        return result;
    }
};
}
