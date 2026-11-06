#include "LiveObservedAssociation.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace asteria_live_association;
static uint32_t Checks = 0;
static constexpr uint64_t Owner = 0x1010, Device = UINT64_C(0xF123456780001234), Command = UINT64_C(0xABCDEF0100000042);
static constexpr uint64_t Depth = UINT64_C(0xF000000000001234), Color = UINT64_C(0xF000000000005678), Pipeline = UINT64_C(0xF000000000000999);
static const std::string Profile = "3824bece18da865e2ea4124679322447ac00cfca2907eb7aa858480d7166513a";

static void Check(bool value, const char *message)
{
    ++Checks; if (!value) throw std::runtime_error(message);
}

static Matrix Perspective()
{
    const double sy = 1.333333333333, sx = sy / (16.0 / 9.0), nearPlane = 7, farPlane = 5000;
    const double a = farPlane / (nearPlane - farPlane), b = farPlane * nearPlane / (nearPlane - farPlane);
    return {float(sx), 0, 0, float(-sx * 100), 0, float(sy), 0, float(-sy * 200), 0, 0, float(a), float(b - a * 300), 0, 0, -1, 300};
}

struct Fixture
{
    Bridge bridge;
    Fixture()
    {
        bridge.RegisterProfile(Profile); bridge.InitDevice(Owner, Device);
        bridge.InitResource(Owner, Depth, 3840, 2160, 4, 45); bridge.InitResource(Owner, Color, 3840, 2160, 4, 87);
        bridge.InitPipeline(Owner, Pipeline, Profile); bridge.InitCommand(Owner, Command);
    }
    DrawMetadata Draw(uint64_t interval, uint64_t ordinal)
    {
        DrawMetadata value{}; value.owner = Owner; value.nativeDevice = Device; value.nativeCommand = Command;
        value.observerInterval = interval; value.sceneEpoch = 1; value.sceneEpochVerified = true; value.ordinal = ordinal; value.sequence = interval * 100 + ordinal;
        value.tickNanoseconds = (interval * 100 + ordinal) * 1000000;
        value.depth = Depth; value.color = Color; value.pipeline = Pipeline; value.width = 3840; value.height = 2160;
        value.viewportKnown = true; value.viewportDrawStateVerified = true; value.viewport = {0, 0, 3840, 2160, 0, 1}; return value;
    }
    AsteriaBoundBufferDescriptorV1 Native(uint64_t interval)
    {
        AsteriaBoundBufferDescriptorV1 value{}; value.struct_size = sizeof(value); value.api_version = 1;
        value.status = ASTERIA_DESCRIPTOR_OK; value.command_generation = UINT64_C(0x1234567890000001);
        value.recording_generation = interval + 1; value.bind_sequence = interval * 100 + 1; return value;
    }
    void Record(uint64_t interval)
    {
        for (uint64_t ordinal = 1; ordinal <= 3; ++ordinal)
            Check(bridge.Record(Draw(interval, ordinal), Native(interval), Profile, Perspective(), true), "verified controlled recording rejected");
    }
    Json Present(uint64_t interval)
    {
        bridge.Present(Owner, interval + 1, 3840, 2160, (interval * 100 + 99) * 1000000, 1, true);
        return bridge.Snapshot()["devices"][0];
    }
    Json Interval(uint64_t interval)
    {
        if (interval) bridge.ResetCommand(Owner, Command);
        Record(interval); bridge.SubmitCommand(Owner, Command, interval); return Present(interval);
    }
};

int main()
{
    Json report = {{"author", "Anokianx"}, {"passed", false}, {"cpu_mock_contract_only", true}, {"game_started", false},
        {"gpu_frame_aligned", false}, {"main_view_verified", false}, {"native_camera_ready", false}, {"game_fg_ready", false}};
    try
    {
        Fixture positive; auto first = positive.Interval(0); auto second = positive.Interval(1); auto third = positive.Interval(2);
        Check(!first["observed_association_available"].get<bool>() && !second["observed_association_available"].get<bool>() &&
            third["observed_association_available"].get<bool>(), "three complete recorded intervals required for observation association");
        Check(third["current"]["depth"]["handle"] == Depth && third["current"]["color"]["handle"] == Color,
            "complete native-width attachment identities retained");
        Check(third["current"]["depth"]["generation"].get<uint64_t>() != 0 && third["current"]["first_pipeline_generation"].get<uint64_t>() != 0,
            "observed creation generations retained");
        Check(third["last_native_recording_evidence"]["native_command_allocation_generation"] == UINT64_C(0x1234567890000001) &&
            third["last_native_recording_evidence"]["native_command_recording_generation"] == 3,
            "native allocation and recording generations retained without narrowing");
        Check(!third["gpu_frame_aligned"].get<bool>() && !third["main_view_verified"].get<bool>() && !third["game_fg_ready"].get<bool>(),
            "CPU observer association never upgrades GPU or game readiness");
        uint32_t rejected = 0;
        auto reject = [&](const std::function<void(Fixture &, DrawMetadata &, AsteriaBoundBufferDescriptorV1 &)> &change) {
            Fixture fixture; auto draw = fixture.Draw(0, 1); auto native = fixture.Native(0); change(fixture, draw, native);
            Check(!fixture.bridge.Record(draw, native, Profile, Perspective(), true), "unqualified live recording accepted");
            auto result = fixture.Present(0);
            Check(!result["observed_association_available"].get<bool>() && result["reset_history"].get<bool>(), "rejected recording retained usable history");
            ++rejected;
        };
        reject([](auto &, auto &draw, auto &) { draw.nativeDevice ^= 1ull << 48; });
        reject([](auto &, auto &draw, auto &) { draw.viewportKnown = false; });
        reject([](auto &, auto &draw, auto &) { draw.viewportDrawStateVerified = false; });
        reject([](auto &, auto &draw, auto &) { draw.viewport[2] = 1920; });
        reject([](auto &, auto &draw, auto &) { draw.width = 1920; });
        reject([](auto &f, auto &, auto &) { f.bridge.InitResource(Owner, Depth, 1920, 1080, 4, 45); });
        reject([](auto &f, auto &, auto &) { f.bridge.InitResource(Owner, Color, 3840, 2160, 1, 87); });
        reject([](auto &, auto &draw, auto &) { draw.sceneEpoch = 0; });
        reject([](auto &, auto &, auto &native) { native.command_generation = 0; });
        reject([](auto &, auto &, auto &native) { native.recording_generation = 0; });
        reject([](auto &, auto &, auto &native) { native.status = ASTERIA_DESCRIPTOR_SECONDARY_UNSUPPORTED; });
        reject([](auto &f, auto &, auto &) { f.bridge.DestroyResource(Owner, Depth); });
        reject([](auto &f, auto &, auto &) { f.bridge.DestroyPipeline(Owner, Pipeline); });
        reject([](auto &f, auto &, auto &) { f.bridge.ResetCommand(Owner, Command, true); });
        reject([](auto &f, auto &, auto &) { f.bridge.UnsupportedCommand(Owner, Command, "secondary_command_executed"); });
        for (const auto *reason : {"uniform_sample_budget", "uniform_range_deduplicated", "indirect_draw_or_dispatch", "missing_attachment_binding"})
        {
            Fixture fixture; fixture.Record(0); fixture.bridge.CoverageLoss(Owner, reason); fixture.bridge.SubmitCommand(Owner, Command, 0);
            const auto result = fixture.Present(0);
            Check(result["status"] == "coverage_loss" && !result["last_interval_coverage_losses"].empty(), "coverage loss was concealed by valid retained draws");
        }
        {
            Fixture fixture; fixture.Record(0); const auto result = fixture.Present(0);
            Check(result["status"] == "coverage_loss" && result["last_interval_coverage_losses"].contains("recording_not_submitted_in_interval"),
                "unsubmitted recording became an execution-aligned observation");
        }
        {
            Fixture fixture; fixture.Record(0); fixture.bridge.SubmitCommand(Owner, Command, 1); const auto result = fixture.Present(0);
            Check(result["last_interval_coverage_losses"].contains("deferred_recording_submission"), "deferred recording submission was silently rebound to present interval");
        }
        {
            Fixture fixture; fixture.Record(0); fixture.bridge.SubmitCommand(Owner, Command, 0); fixture.bridge.SubmitCommand(Owner, Command, 0);
            Check(fixture.Present(0)["last_interval_coverage_losses"].contains("recording_resubmitted"), "reused command recording accepted as fresh draw samples");
        }
        {
            Fixture fixture; fixture.Record(0); fixture.bridge.ResetCommand(Owner, Command);
            Check(fixture.Present(0)["last_interval_coverage_losses"].contains("command_reset_before_interval_closed"), "unclosed command reset did not poison interval");
        }
        {
            Fixture fixture; fixture.Record(0); fixture.bridge.SubmitCommand(Owner, Command, 0);
            fixture.bridge.Present(Owner, 1, 3840, 2160, 99000000, 1, true);
            fixture.bridge.DestroyPipeline(Owner, Pipeline);
            Check(!fixture.bridge.Snapshot()["devices"][0]["pending_coverage_losses"].contains("pipeline_retired_during_open_interval"),
                "pipeline retirement after interval closure was reported as current coverage loss");
        }
        {
            Fixture fixture; fixture.Record(0); fixture.bridge.DestroyPipeline(Owner, Pipeline);
            fixture.bridge.SubmitCommand(Owner, Command, 0);
            Check(fixture.Present(0)["last_interval_coverage_losses"].contains("pipeline_retired_during_open_interval"),
                "pipeline retirement during an open observed interval was not reported");
        }
        {
            Fixture fixture; fixture.Record(0); fixture.bridge.SubmitCommand(Owner, Command, 0);
            fixture.bridge.Present(Owner, 1, 3840, 2160, 99000000, 2, true);
            Check(fixture.bridge.Snapshot()["devices"][0]["status"] == "scene_changed" &&
                !fixture.bridge.Snapshot()["devices"][0]["observed_association_available"].get<bool>(),
                "scene epoch change did not reset observation history");
        }
        {
            Fixture fixture; fixture.Record(0); fixture.bridge.SubmitCommand(Owner, Command, 0);
            fixture.bridge.Present(Owner, 1, 3840, 2160, 99000000);
            const auto result = fixture.bridge.Snapshot()["devices"][0];
            Check(result["last_interval_coverage_losses"].contains("scene_epoch_unknown") &&
                !result["observed_association_available"].get<bool>() && result["reset_history"].get<bool>(),
                "present without a scene epoch retained usable association history");
        }
        {
            Fixture fixture; auto draw = fixture.Draw(0, 1); draw.sceneEpoch = 0; draw.sceneEpochVerified = false;
            const auto native = fixture.Native(0);
            Check(!fixture.bridge.Record(draw, native, Profile, Perspective(), true), "recording without scene epoch was accepted");
            const auto evidence = fixture.bridge.Snapshot()["devices"][0]["last_native_recording_evidence"];
            Check(evidence["scene_epoch"] == 0 && !evidence["scene_epoch_verified"].get<bool>() && evidence["observer_only"].get<bool>(),
                "unknown scene epoch was not clearly marked unverified and observer-only");
            Check(evidence["native_command"] == Command && evidence["native_command_allocation_generation"] == native.command_generation &&
                evidence["native_command_recording_generation"] == native.recording_generation && evidence["draw_ordinal"] == 1 &&
                evidence["viewport_known"].get<bool>(), "unknown scene epoch discarded native recording identity");
            Check(evidence["depth_identity"]["generation"] != 0 && evidence["color_identity"]["generation"] != 0,
                "unknown scene epoch discarded observed attachment generations");
        }
        {
            Fixture fixture; fixture.Record(0); fixture.bridge.InitDevice(Owner, Device);
            auto result = fixture.bridge.Snapshot()["devices"][0];
            Check(result["tracking_reset_count"] == 1 && result["last_tracking_reset"]["reason"] == "same_owner_device_reinitialized" &&
                result["last_tracking_reset"]["discarded_resources"] == 2 && result["pending_coverage_losses"].contains("device_tracking_reset"),
                "same-owner device reinitialization silently discarded tracking state");
            fixture.bridge.Present(Owner, 1, 3840, 2160, 99000000, 1, true);
            result = fixture.bridge.Snapshot()["devices"][0];
            Check(result["last_interval_coverage_losses"].contains("device_tracking_reset") &&
                !result["observed_association_available"].get<bool>(), "device reinitialization loss was not carried into the next interval");
        }
        {
            Fixture fixture; Check(fixture.bridge.Record(fixture.Draw(0, 1), fixture.Native(0), Profile, Perspective(), true), "initial native generation rejected");
            auto native = fixture.Native(0); ++native.recording_generation;
            Check(!fixture.bridge.Record(fixture.Draw(0, 2), native, Profile, Perspective(), true), "changed native recording generation accepted inside interval");
        }
        {
            const auto old = third["current"]["depth"]["generation"];
            positive.bridge.DestroyResource(Owner, Depth); positive.bridge.InitResource(Owner, Depth, 3840, 2160, 4, 45);
            positive.Interval(3); positive.Interval(4); positive.Interval(5); const auto result = positive.Interval(6);
            Check(result["observed_association_available"].get<bool>() && result["current"]["depth"]["generation"] != old,
                "retired resource generation revived after native handle reuse");
        }
        report.update({{"passed", true}, {"checks", Checks}, {"rejected_recording_cases", rejected},
            {"same_recording_matrix_attachment_metadata", true}, {"creation_generations_required", true},
            {"actual_attachment_extent_and_samples_checked", true}, {"scene_epoch_required_and_changes_reset", true},
            {"unknown_epoch_preserves_native_identity_evidence", true}, {"same_owner_device_reinit_is_explicit_loss", true},
            {"closed_pipeline_retirement_not_current_loss", true},
            {"deferred_secondary_indirect_and_reuse_fail_closed", true}, {"coverage_budget_and_dedup_loss_explicit", true},
            {"native_command_generations_preserved", true}});
    }
    catch (const std::exception &failure) { report["error"] = failure.what(); }
    std::filesystem::create_directories("results");
    std::ofstream("results/live-association-contract.json") << report.dump(2) << '\n';
    std::cout << report.dump(2) << '\n';
    return report.value("passed", false) ? 0 : 1;
}
