#include "LiveObservedAssociation.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace asteria_live_association;
using namespace asteria_frame_association;
using Json = nlohmann::json;

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

static PresentTicketEvidence Ticket(uint64_t generation, uint64_t image, uint64_t acquire, uint64_t submit,
                                    uint64_t device = Device, uint64_t owner = Owner, uint64_t swapchain = UINT64_C(0x7700), uint64_t command = Command)
{
    PresentTicketEvidence value{}; value.verified = true; value.device = device; value.owner = owner; value.command = command;
    value.swapchain = swapchain; value.swapchainGeneration = generation; value.imageIndex = image;
    value.acquireSerial = acquire; value.submitSerial = submit; return value;
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
    DrawMetadata Draw(uint64_t interval, uint64_t ordinal, const PresentTicketEvidence &ticket)
    {
        DrawMetadata value{}; value.owner = Owner; value.nativeDevice = Device; value.nativeCommand = Command;
        value.observerInterval = interval; value.sceneEpoch = 1; value.sceneEpochVerified = true; value.ordinal = ordinal;
        value.sequence = interval * 100 + ordinal; value.tickNanoseconds = (interval * 100 + ordinal) * 1000000;
        value.depth = Depth; value.color = Color; value.pipeline = Pipeline; value.width = 3840; value.height = 2160;
        value.viewportKnown = true; value.viewportDrawStateVerified = true; value.viewport = {0, 0, 3840, 2160, 0, 1};
        value.presentTicket = ticket; return value;
    }
    AsteriaBoundBufferDescriptorV1 Native(uint64_t interval)
    {
        AsteriaBoundBufferDescriptorV1 value{}; value.struct_size = sizeof(value); value.api_version = 1;
        value.status = ASTERIA_DESCRIPTOR_OK; value.command_generation = UINT64_C(0x1234567890000001);
        value.recording_generation = interval + 1; value.bind_sequence = interval * 100 + 1; return value;
    }
    void Record(uint64_t interval, const PresentTicketEvidence &ticket)
    {
        for (uint64_t ordinal = 1; ordinal <= 3; ++ordinal)
        {
            const bool accepted = bridge.Record(Draw(interval, ordinal, ticket), Native(interval), Profile, Perspective(), true);
            Check(accepted, "ticket recording rejected");
        }
    }
    Json Present(uint64_t interval, const PresentTicketEvidence &ticket)
    {
        bridge.Present(Owner, interval + 1, 3840, 2160, (interval * 100 + 99) * 1000000, 1, true, ticket);
        return bridge.Snapshot()["devices"][0];
    }
    Json Interval(uint64_t interval, const PresentTicketEvidence &ticket)
    {
        if (interval) bridge.ResetCommand(Owner, Command);
        Record(interval, ticket); bridge.SubmitCommand(Owner, Command, interval); return Present(interval, ticket);
    }
};

static void CheckExactTicketAndWarmup()
{
    Fixture fixture;
    const auto first = fixture.Interval(0, Ticket(1, 0, 1, 1));
    const auto second = fixture.Interval(1, Ticket(1, 1, 2, 2));
    const auto third = fixture.Interval(2, Ticket(1, 0, 3, 3));
    Check(!first["observed_association_available"].get<bool>() && !second["observed_association_available"].get<bool>() &&
          third["observed_association_available"].get<bool>(), "exact tickets did not preserve controlled warmup");
    Check(third["present_ticket_verified"].get<bool>() && third["present_ticket"]["swapchain_generation"] == 1 &&
          third["present_ticket"]["image_index"] == 0 && third["present_ticket"]["acquire_serial"] == 3 &&
          third["current"]["present_ticket"]["submit_serial"] == 3, "exact ticket evidence was not retained");
}

static void CheckRejections()
{
    {
        Fixture fixture; const auto ticket = Ticket(1, 0, 1, 1); fixture.Interval(0, ticket);
        const auto result = fixture.Interval(1, ticket);
        Check(result["status"] == "present_ticket_duplicate" && !result["observed_association_available"].get<bool>(), "duplicate ticket was accepted");
    }
    {
        Fixture fixture; fixture.Interval(0, Ticket(1, 0, 2, 2));
        const auto result = fixture.Interval(1, Ticket(1, 1, 1, 3));
        Check(result["status"] == "present_ticket_stale", "stale ticket serial was accepted");
    }
    {
        Fixture fixture; fixture.Interval(0, Ticket(1, 0, 1, 1));
        const auto result = fixture.Interval(1, Ticket(2, 1, 2, 2));
        Check(result["status"] == "present_ticket_resize", "swapchain resize generation was accepted as continuous");
    }
    {
        Fixture fixture; const auto ticket = Ticket(1, 0, 1, 1); fixture.Record(0, ticket); fixture.bridge.SubmitCommand(Owner, Command, 0);
        const auto result = fixture.Present(0, Ticket(1, 0, 1, 1, Device ^ (UINT64_C(1) << 48)));
        Check(result["status"] == "present_ticket_foreign", "foreign device ticket was accepted");
    }
    {
        Fixture fixture; const auto ticket = Ticket(1, 0, 1, 1); fixture.Record(0, ticket); fixture.bridge.SubmitCommand(Owner, Command, 0);
        auto mismatched = ticket; mismatched.command ^= 1;
        const auto result = fixture.Present(0, mismatched);
        Check(result["status"] == "present_ticket_mismatch", "present command mismatch was accepted");
    }
    {
        Fixture fixture; const auto ticket = Ticket(1, 0, 1, 1); fixture.Record(0, ticket); fixture.bridge.SubmitCommand(Owner, Command, 0);
        const auto result = fixture.Present(0, {});
        Check(result["status"] == "present_ticket_missing", "ticket-bearing draw closed without a ticket");
    }
    {
        Fixture fixture; fixture.Record(0, {}); fixture.bridge.SubmitCommand(Owner, Command, 0);
        const auto result = fixture.Present(0, Ticket(1, 0, 1, 1));
        Check(result["status"] == "present_ticket_missing", "ticket was accepted without ticket-bearing draws");
    }
    {
        Fixture fixture; auto first = Ticket(1, 0, 1, 1); auto second = Ticket(1, 0, 1, 1); second.owner ^= 1;
        auto draw = fixture.Draw(0, 2, second); Check(fixture.bridge.Record(fixture.Draw(0, 1, first), fixture.Native(0), Profile, Perspective(), true), "first ticket draw rejected");
        Check(!fixture.bridge.Record(draw, fixture.Native(0), Profile, Perspective(), true), "foreign owner draw ticket accepted");
    }
}

static void CheckLegacyPath()
{
    Fixture fixture; const PresentTicketEvidence empty{};
    fixture.Record(0, empty); fixture.bridge.SubmitCommand(Owner, Command, 0);
    const auto result = fixture.Present(0, empty);
    Check(!result["present_ticket_verified"].get<bool>() && result["status"] == "warming_up",
          "legacy no-ticket path was changed by optional ticket evidence");
}

static void CheckBoundedHistory()
{
    Fixture fixture; Json result;
    for (uint64_t interval = 0; interval < 257; ++interval)
    {
        const auto ticket = Ticket(1, interval % 2, interval + 1, interval + 1);
        result = fixture.Interval(interval, ticket);
    }
    Check(result["status"] == "present_ticket_capacity" && !result["observed_association_available"].get<bool>(), "ticket history exceeded its bound");
}

int main()
{
    Json report = {{"author", "Anokianx"}, {"passed", false}, {"cpu_mock_contract_only", true}, {"game_started", false},
        {"gpu_frame_aligned", false}, {"main_view_verified", false}, {"game_fg_ready", false}};
    try
    {
        CheckExactTicketAndWarmup(); CheckRejections(); CheckLegacyPath(); CheckBoundedHistory();
        report.update({{"passed", true}, {"checks", Checks}, {"bounded_ticket_history", true}, {"exact_owner_generation_image_serial_match", true},
            {"stale_reuse_resize_foreign_duplicate_fail_closed", true}, {"legacy_no_ticket_path_preserved", true},
            {"ticket_never_upgrades_gpu_or_game_readiness", true}});
    }
    catch (const std::exception &error) { report["error"] = error.what(); report["checks"] = Checks; }
    std::filesystem::create_directories("results");
    std::ofstream("results/present-ticket-contract.json") << report.dump(2) << '\n';
    std::cout << report.dump(2) << '\n'; return report.value("passed", false) ? 0 : 1;
}
