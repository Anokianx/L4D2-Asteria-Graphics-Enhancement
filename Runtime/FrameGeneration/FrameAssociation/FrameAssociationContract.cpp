#include "ObservedFrameAssociation.h"
#include <json.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace asteria_frame_association;
using Json = nlohmann::json;
static uint32_t checks = 0;
static const uint64_t DeviceId = 0xF123456780001234ull;
static const std::string Profile = "3824bece18da865e2ea4124679322447ac00cfca2907eb7aa858480d7166513a";

static void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
    ++checks;
}

static Matrix Perspective(Vector origin = {100, 200, 300}, double nearPlane = 7, double sy = 1.333333333333)
{
    const double sx = sy / (16.0 / 9.0), farPlane = 5000;
    const double a = farPlane / (nearPlane - farPlane), b = farPlane * nearPlane / (nearPlane - farPlane);
    return {float(sx), 0, 0, float(-sx * origin[0]), 0, float(sy), 0, float(-sy * origin[1]),
            0, 0, float(a), float(b - a * origin[2]), 0, 0, -1, float(origin[2])};
}

static DrawObservation Draw(uint64_t interval, uint64_t ordinal, const Matrix &matrix = Perspective())
{
    DrawObservation draw;
    draw.device = DeviceId; draw.scene = 17; draw.observerIntervalId = interval;
    draw.draw = {0xABCDEF0100000042ull, 1, ordinal, interval * 100 + ordinal, (interval * 100 + ordinal) * 1000000};
    draw.depth = {DeviceId, 0xF000000000001234ull, 1, HandleDomain::ReShadeResource, 3840, 2160, 4, 45};
    draw.color = {DeviceId, 0xF000000000005678ull, 1, HandleDomain::ReShadeResource, 3840, 2160, 4, 87};
    draw.viewportWidth = 3840; draw.viewportHeight = 2160;
    draw.pipeline = 0xF000000000000999ull; draw.pipelineGeneration = 1; draw.nativeShaderSha256 = Profile;
    draw.worldToClip = matrix;
    draw.nativeBindingVerified = draw.stableMatrixCopy = draw.directPrimaryRecording = draw.observedOutsideEffects = true;
    draw.depthAttachment = draw.colorAttachment = true;
    return draw;
}

static PresentObservation Present(uint64_t interval)
{
    return {DeviceId, 17, interval, interval + 1, (interval * 100 + 99) * 1000000, 3840, 2160, true, false};
}

static void Register(Collector &collector)
{
    Check(collector.RegisterNativeProfile(Profile), "native profile registration failed");
}

static Association Collect(Collector &collector, uint64_t interval, Matrix matrix = Perspective())
{
    for (uint64_t ordinal = 1; ordinal <= 3; ++ordinal)
        Check(collector.ObserveDraw(Draw(interval, ordinal, matrix)) == Status::AcceptedObservation, "valid native draw rejected");
    return collector.OnPresent(Present(interval));
}

static void RejectDraw(const std::function<void(DrawObservation &)> &mutate, Status expected)
{
    Collector collector(DeviceId); Register(collector);
    auto draw = Draw(1, 1); mutate(draw);
    Check(collector.ObserveDraw(draw) == expected, "malformed draw rejected for wrong reason");
    const auto result = collector.OnPresent(Present(1));
    Check(!result.observedAssociationAvailable && result.resetHistory, "invalid observation retained usable history");
}

static void CheckSourceAndOrdering()
{
    for (const auto &mutate : std::vector<std::function<void(DrawObservation &)>>{
        [](auto &v) { v.device ^= 1ull << 48; }, [](auto &v) { v.draw.commandGeneration = 0; },
        [](auto &v) { v.depth.generation = 0; }, [](auto &v) { v.color.generation = 0; },
        [](auto &v) { v.pipelineGeneration = 0; }, [](auto &v) { v.draw.ordinal = 0; },
        [](auto &v) { v.draw.recordingSequence = 0; }, [](auto &v) { v.color.domain = HandleDomain::Unknown; },
        [](auto &v) { v.depth.device ^= 1ull << 48; }, [](auto &v) { v.color.domain = static_cast<HandleDomain>(99); },
        [](auto &v) { v.depth.samples = 3; }}) RejectDraw(mutate, Status::InvalidIdentity);
    for (const auto &mutate : std::vector<std::function<void(DrawObservation &)>>{
        [](auto &v) { v.stableMatrixCopy = false; }, [](auto &v) { v.nativeBindingVerified = false; },
        [](auto &v) { v.directPrimaryRecording = false; }, [](auto &v) { v.observedOutsideEffects = false; },
        [](auto &v) { v.nativeShaderSha256[0] = 'f'; }, [](auto &v) { v.pass = static_cast<PassKind>(99); }}) RejectDraw(mutate, Status::UnverifiedSource);
    RejectDraw([](auto &v) { v.viewportX = 1; }, Status::UnsupportedViewport);
    RejectDraw([](auto &v) { v.viewportWidth /= 2; }, Status::UnsupportedViewport);
    RejectDraw([](auto &v) { v.color.samples = 1; }, Status::InvalidDimensions);
    RejectDraw([](auto &v) { v.color.height /= 2; }, Status::InvalidDimensions);
    RejectDraw([](auto &v) { v.depth.domain = HandleDomain::VulkanImage; }, Status::InvalidDimensions);
    RejectDraw([](auto &v) { v.worldToClip[3] = std::numeric_limits<float>::quiet_NaN(); }, Status::InvalidMatrix);
    RejectDraw([](auto &v) { v.worldToClip[2] = 0.4f; }, Status::InvalidMatrix);
    RejectDraw([](auto &v) { v.worldToClip[0] = -v.worldToClip[0]; }, Status::InvalidMatrix);
    Collector duplicate(DeviceId); Register(duplicate);
    Check(duplicate.ObserveDraw(Draw(1, 1)) == Status::AcceptedObservation, "initial draw unavailable");
    auto repeated = Draw(1, 1); repeated.draw.recordingSequence = 102; repeated.draw.recordingTickNanoseconds = 102000000;
    Check(duplicate.ObserveDraw(repeated) == Status::InvalidIdentity, "duplicate same-command draw accepted");
    Check(!duplicate.OnPresent(Present(1)).observedAssociationAvailable, "duplicate survived interval closure");
    Collector reverse(DeviceId); Register(reverse);
    Check(reverse.ObserveDraw(Draw(1, 2)) == Status::AcceptedObservation, "initial ordering draw unavailable");
    Check(reverse.ObserveDraw(Draw(1, 1)) == Status::InvalidOrder, "recording-order reversal accepted");
    Collector conflict(DeviceId); Register(conflict);
    auto a = Draw(1, 1); a.nativeEngineFrameIdAvailable = true; a.nativeEngineFrameId = 90001;
    auto b = Draw(1, 2); b.nativeEngineFrameIdAvailable = true; b.nativeEngineFrameId = 90002;
    Check(conflict.ObserveDraw(a) == Status::AcceptedObservation, "native frame identity unavailable");
    Check(conflict.ObserveDraw(b) == Status::NativeFrameConflict, "different native IDs merged in one camera group");
}

static void CheckTemporalState()
{
    Collector collector(DeviceId); Register(collector);
    const auto first = Collect(collector, 1);
    const auto second = Collect(collector, 2, Perspective({100.25, 200, 300}));
    const auto third = Collect(collector, 3, Perspective({100.5, 200, 300}));
    Check(!first.observedAssociationAvailable && !second.observedAssociationAvailable, "selection skipped stable-target warmup");
    Check(third.observedAssociationAvailable && !third.resetHistory && third.stableIntervals == 3, "stable camera pair unavailable");
    Check(third.previous.observerIntervalId == 2 && third.current.observerIntervalId == 3 && third.current.presentObserverId == 4,
          "observer IDs were conflated with closure/present IDs");
    Check(!third.current.nativeEngineFrameIdAvailable && !third.current.nativeEngineFrameId, "observer ID became invented native frame ID");
    Check(third.observerIntervalOnly && !third.gpuFrameAligned && !third.mainViewVerified && !third.nativeCameraReady && !third.gameFgReady,
          "CPU correlation became GPU/main-view readiness");
    Check(third.current.firstDraw.ordinal == 1 && third.current.lastDraw.ordinal == 3 &&
          third.current.firstNativeShaderSha256 == Profile && third.current.depth.handle == 0xF000000000001234ull,
          "full native source/target identity was lost");
    auto gap = Collect(collector, 5);
    Check(!gap.observedAssociationAvailable && gap.resetHistory && gap.stableIntervals == 1, "observer gap retained motion history");
    Collect(collector, 6); auto restored = Collect(collector, 7);
    Check(restored.observedAssociationAvailable, "history did not recover after explicit warmup");
    auto cut = Collect(collector, 8, Perspective({900, 200, 300}));
    Check(!cut.observedAssociationAvailable && cut.status == Status::CameraDiscontinuity, "camera teleport retained motion history");
    for (uint64_t ordinal = 1; ordinal <= 3; ++ordinal)
    {
        auto draw = Draw(9, ordinal); draw.depth.generation = 2;
        Check(collector.ObserveDraw(draw) == Status::AcceptedObservation, "new resource generation could not be observed");
    }
    auto changed = collector.OnPresent(Present(9));
    Check(changed.status == Status::IdentityChanged && !changed.observedAssociationAvailable, "resource generation alias retained history");
    collector.RetireResource(changed.current.depth);
    auto retired = Collect(collector, 10);
    Check(retired.status == Status::ResourceRetired && !retired.observedAssociationAvailable, "retired attachment remained eligible");
    Collector reset(DeviceId); Register(reset);
    reset.ObserveDraw(Draw(1, 1)); reset.CommandReset(Draw(1, 1).draw.command, 1);
    Check(reset.OnPresent(Present(1)).status == Status::CommandReset, "unclosed command reset was ignored");
    Collector loss(DeviceId); Register(loss);
    loss.ObserveDraw(Draw(1, 1)); loss.MarkCoverageLoss();
    Check(loss.OnPresent(Present(1)).status == Status::CoverageLoss, "lost coverage promoted a view");
    Collector missing(DeviceId); Register(missing);
    missing.ObserveDraw(Draw(1, 1)); auto present = Present(1); present.recordingCoverageComplete = false;
    Check(missing.OnPresent(present).status == Status::CoverageLoss, "missing complete-coverage declaration accepted");
    Policy cap; cap.maxObservations = 2;
    Collector bounded(DeviceId, cap); Register(bounded);
    bounded.ObserveDraw(Draw(1, 1)); bounded.ObserveDraw(Draw(1, 2));
    Check(bounded.ObserveDraw(Draw(1, 3)) == Status::CapacityLoss, "observation cap did not fail closed");
    Check(!bounded.OnPresent(Present(1)).observedAssociationAvailable, "bounded subset promoted after capacity loss");
    Policy bad; bad.minimumMainNear = std::numeric_limits<double>::quiet_NaN();
    Collector invalidPolicy(DeviceId, bad);
    Check(invalidPolicy.ObserveDraw(Draw(1, 1)) == Status::InvalidPolicy, "NaN selection policy was accepted");
}

static Json CheckNativeMatrices()
{
    const std::filesystem::path source = "../Research/NativeViews/native-view-analysis.json";
    Json native; std::ifstream(source) >> native;
    const auto sky = native.at("groups").at(0).at("raw_register_floats").get<Matrix>();
    const auto main = native.at("groups").at(1).at("raw_register_floats").get<Matrix>();
    Camera mainCamera, skyCamera;
    Check(RecoverNativePerspective(main, mainCamera) && RecoverNativePerspective(sky, skyCamera), "retained native matrices could not be decoded");
    Check(std::abs(mainCamera.nearPlane - 7) < 0.001 && std::abs(skyCamera.nearPlane - 2) < 0.001, "native projection decomposition changed");
    Check(std::abs(mainCamera.origin[0] - 517.9996516221494) < 0.001, "native draw-derived camera position changed");
    Policy policy; policy.skyReferences.push_back({{8053, 12657, -1008}, 32});
    Collector collector(DeviceId, policy); Register(collector);
    Association result;
    for (uint64_t interval = 1; interval <= 3; ++interval)
    {
        for (uint64_t ordinal = 1; ordinal <= 10; ++ordinal)
            Check(collector.ObserveDraw(Draw(interval, ordinal, sky)) == Status::AcceptedObservation, "native sky group was not recorded");
        for (uint64_t ordinal = 11; ordinal <= 13; ++ordinal)
            Check(collector.ObserveDraw(Draw(interval, ordinal, main)) == Status::AcceptedObservation, "native main candidate group was not recorded");
        result = collector.OnPresent(Present(interval));
        Check(result.skyRelationsExcluded == 1, "32x sky relationship was not excluded");
    }
    Check(result.observedAssociationAvailable && result.current.observedDraws == 3, "dominant sky observations displaced main candidate");
    Check(std::abs(result.current.camera.nearPlane - 7) < 0.001 && !result.mainViewVerified, "sky exclusion became certified main-view identity");
    Collector onlySky(DeviceId, policy); Register(onlySky);
    auto skyOnly = Collect(onlySky, 1, sky);
    Check(skyOnly.status == Status::ProjectionPolicy && !skyOnly.observedAssociationAvailable, "sky-only interval became main candidate");
    Collector viewmodel(DeviceId); Register(viewmodel);
    for (uint64_t ordinal = 1; ordinal <= 3; ++ordinal) viewmodel.ObserveDraw(Draw(1, ordinal));
    auto knownWeapon = Draw(1, 4, Perspective({100, 200, 300}, 1, 2)); knownWeapon.pass = PassKind::Viewmodel;
    Check(viewmodel.ObserveDraw(knownWeapon) == Status::ExcludedPass, "known weapon view was not excluded");
    Check(viewmodel.OnPresent(Present(1)).excludedDraws == 1, "excluded pass evidence missing");
    Collector ambiguous(DeviceId); Register(ambiguous);
    for (uint64_t ordinal = 1; ordinal <= 3; ++ordinal) ambiguous.ObserveDraw(Draw(1, ordinal));
    auto unknownSubview = Draw(1, 4, Perspective({105, 200, 300}));
    Check(ambiguous.ObserveDraw(unknownSubview) == Status::AcceptedObservation, "unknown competing view was not retained");
    Check(ambiguous.OnPresent(Present(1)).status == Status::AmbiguousViews, "majority voting silently selected between unknown views");
    Collector shadows(DeviceId); Register(shadows);
    auto shadow = Draw(1, 1); shadow.pass = PassKind::Shadow; shadow.color = {};
    Check(shadows.ObserveDraw(shadow) == Status::ExcludedPass, "known depth-only shadow required an invented color attachment");
    return {{"retained_native_matrix_values_tested", 2}, {"map_sky_scale_tested", 32},
            {"native_values_source", source.generic_string()}, {"observation_metadata", "controlled_fixture_not_live_game"}};
}

static void CheckPresentAndNativeIdentity()
{
    Collector native(DeviceId); Register(native);
    Association result;
    for (uint64_t interval = 1; interval <= 3; ++interval)
    {
        for (uint64_t ordinal = 1; ordinal <= 3; ++ordinal)
        {
            auto draw = Draw(interval, ordinal); draw.nativeEngineFrameIdAvailable = true;
            draw.nativeEngineFrameId = 0xAB00000000000000ull + interval;
            Check(native.ObserveDraw(draw) == Status::AcceptedObservation, "provided native ID rejected");
        }
        result = native.OnPresent(Present(interval));
    }
    Check(result.observedAssociationAvailable && result.current.nativeEngineFrameIdAvailable &&
          result.current.nativeEngineFrameId == 0xAB00000000000003ull && result.current.observerIntervalId == 3,
          "native and observer identity domains were truncated or conflated");
    auto repeated = Present(3);
    Check(native.OnPresent(repeated).status == Status::InvalidOrder, "same observer interval closed twice without reset");
    Collector empty(DeviceId);
    Check(empty.OnPresent(Present(1)).status == Status::NoDraws, "empty present interval became a camera");
    Collector badPresent(DeviceId); Register(badPresent); badPresent.ObserveDraw(Draw(1, 1));
    auto bad = Present(1); bad.presentObserverId = 100;
    Check(badPresent.OnPresent(bad).status == Status::InvalidPresent, "arbitrary present ID invented an association");
    Collector stale(DeviceId); Register(stale);
    stale.ObserveDraw(Draw(1, 1));
    auto later = Draw(1, 2); later.draw.recordingTickNanoseconds += 2000000000ull;
    stale.ObserveDraw(later);
    auto stalePresent = Present(1); stalePresent.tickNanoseconds += 2000000000ull;
    Check(stale.OnPresent(stalePresent).status == Status::InvalidOrder, "fresh last draw concealed a stale first draw");
    Collector scenes(DeviceId); Register(scenes); Collect(scenes, 1);
    auto nextScene = Draw(2, 1); nextScene.scene = 18; scenes.ObserveDraw(nextScene);
    auto nextPresent = Present(2); nextPresent.scene = 18;
    Check(scenes.OnPresent(nextPresent).status == Status::SceneChanged, "scene switch retained camera history");
    Collector command(DeviceId); Register(command); command.ObserveDraw(Draw(1, 1));
    auto reset = Draw(1, 2); reset.draw.commandGeneration = 2;
    Check(command.ObserveDraw(reset) == Status::CommandReset, "command generation reuse escaped history invalidation");
    Collector mixed(DeviceId); Register(mixed);
    for (uint64_t interval = 1; interval <= 3; ++interval)
    {
        for (uint64_t ordinal = 1; ordinal <= 3; ++ordinal)
        {
            auto draw = Draw(interval, ordinal); draw.nativeEngineFrameIdAvailable = ordinal == 1; draw.nativeEngineFrameId = 99;
            mixed.ObserveDraw(draw);
        }
        result = mixed.OnPresent(Present(interval));
    }
    Check(result.observedAssociationAvailable && !result.current.nativeEngineFrameIdAvailable && !result.current.nativeEngineFrameId,
          "one native ID was assigned to source draws without that ID");
}

int main()
{
    try
    {
        Check(sizeof(void *) == 4, "contract must run as x86");
        CheckSourceAndOrdering(); CheckTemporalState(); CheckPresentAndNativeIdentity();
        Json native = CheckNativeMatrices();
        Json report = {{"author", Author}, {"schema_version", 1}, {"passed", true}, {"checks", checks},
            {"actual_gpu_inputs_validated", false}, {"game_started", false}, {"production_runtime_modified", false},
            {"observer_interval_only", true}, {"main_view_verified", false}, {"gpu_frame_aligned", false},
            {"native_camera_ready", false}, {"motion_vectors_verified", false}, {"native_fg_ready", false}, {"native_matrix_fixture", native}};
        std::filesystem::create_directories("results");
        std::ofstream("results/frame-association-contract.json") << report.dump(2) << '\n';
        std::cout << report.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
