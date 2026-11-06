#include "../../../InputProbe/EngineCameraProbe.h"
#include <limits>
#include <iostream>
#include <stdexcept>

using namespace asteria_native_input;
using namespace asteria_native_input::engine_camera_detail;

void Check(bool valid, const char* message)
{
    if (!valid) throw std::runtime_error(message);
}

struct FakeEngine
{
    int width = 5760;
    int height = 3240;
    NativeAngle angle{10.0f, 20.0f, 30.0f};
    NativeMatrix matrix{{{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}}};
    NativeMatrix view = matrix;
    bool changeScreen = false;
    bool changeView = false;

    bool Connected() { return true; }
    void Size(int& x, int& y) { x = width; y = height; }
    void Angle(NativeAngle& output) { output = angle; }
    const NativeMatrix& Matrix() { return matrix; }
    const NativeMatrix& Screen()
    {
        if (changeScreen) matrix.m[0][3] += 1.0f;
        return matrix;
    }
    const NativeMatrix& View()
    {
        if (changeView) view.m[1][3] += 1.0f;
        return view;
    }
};

template<typename Member>
std::uintptr_t Address(Member member)
{
    static_assert(sizeof(member) == sizeof(std::uintptr_t));
    std::uintptr_t address = 0;
    std::memcpy(&address, &member, sizeof(address));
    return address;
}

NativeMatrix Compose(const NativeMatrix& left, const NativeMatrix& right)
{
    NativeMatrix result{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            double sum = 0.0;
            for (int index = 0; index < 4; ++index) sum += static_cast<double>(left.m[row][index]) * right.m[index][column];
            result.m[row][column] = static_cast<float>(sum);
        }
    return result;
}

void ProjectionContract()
{
    const double nearDistance = 0.5;
    const double farDistance = 80.0;
    const double rightAtNear = 0.75;
    const double topAtNear = rightAtNear * 9.0 / 16.0;
    const double eye[3] = {37.0, -19.0, 5.0};
    const double basis[3][3] = {{0.6, 0.0, 0.8}, {0.0, 1.0, 0.0}, {-0.8, 0.0, 0.6}};
    NativeMatrix view{};
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            view.m[row][column] = static_cast<float>(basis[row][column]);
            view.m[row][3] -= static_cast<float>(basis[row][column] * eye[column]);
        }
    }
    view.m[3][3] = 1.0f;
    NativeMatrix perspective{};
    perspective.m[0][0] = static_cast<float>(nearDistance / rightAtNear);
    perspective.m[1][1] = static_cast<float>(nearDistance / topAtNear);
    perspective.m[2][2] = static_cast<float>(-farDistance / (farDistance - nearDistance));
    perspective.m[2][3] = static_cast<float>(-farDistance * nearDistance / (farDistance - nearDistance));
    perspective.m[3][2] = -1.0f;
    const NativeMatrix screen = Compose(perspective, view);
    const Json recovered = NativeProjectionDiagnostic(screen, view, 3840, 2160);
    Check(recovered.at("available").get<bool>(), "A translated rotated perspective camera must recover a projection");
    Check(recovered.at("derived_from_native_matrices").get<bool>(), "Recovered camera must identify its native matrix source");
    Check(recovered.at("diagnostics_only").get<bool>(), "Recovered parameters must remain diagnostic");
    Check(!recovered.at("native_camera_ready").get<bool>() && !recovered.at("camera_semantics_verified").get<bool>() &&
          !recovered.at("frame_timing_verified").get<bool>(), "Projection recovery must not certify a main render view");
    Check(std::fabs(recovered.at("near_plane").get<double>() - nearDistance) < 2e-5, "Recovered near clipping distance does not match the frustum");
    Check(std::fabs(recovered.at("far_plane").get<double>() - farDistance) < 0.01, "Recovered far clipping distance does not match the frustum");
    Check(std::fabs(recovered.at("projection_aspect_ratio").get<double>() - 16.0 / 9.0) < 1e-5, "Recovered frustum aspect ratio is incorrect");
    Check(recovered.at("render_surface_aspect_matches_projection").get<bool>(), "A matching backbuffer must preserve the frustum aspect");
    Check(!NativeProjectionDiagnostic(screen, view, 1000, 1000).at("render_surface_aspect_matches_projection").get<bool>(),
          "A square backbuffer must expose the view aspect mismatch");
    for (int axis = 0; axis < 3; ++axis)
        Check(std::fabs(recovered.at("origin")[axis].get<double>() - eye[axis]) < 1e-4, "Recovered camera eye position is incorrect");
    const double pi = 3.14159265358979323846;
    const double expectedHorizontal = 2.0 * std::atan2(rightAtNear, nearDistance) * 180.0 / pi;
    const double expectedVertical = 2.0 * std::atan2(topAtNear, nearDistance) * 180.0 / pi;
    Check(std::fabs(recovered.at("horizontal_fov_degrees").get<double>() - expectedHorizontal) < 1e-4,
          "Horizontal FOV does not match the frustum's geometric opening angle");
    Check(std::fabs(recovered.at("vertical_fov_degrees").get<double>() - expectedVertical) < 1e-4,
          "Vertical FOV does not match the frustum's geometric opening angle");
    const auto projectWorld = [&screen, &basis, &eye](double cameraX, double cameraY, double cameraZ, double expectedX, double expectedY, double expectedDepth)
    {
        double world[4] = {eye[0], eye[1], eye[2], 1.0};
        const double camera[3] = {cameraX, cameraY, cameraZ};
        for (int axis = 0; axis < 3; ++axis)
            for (int component = 0; component < 3; ++component) world[axis] += basis[component][axis] * camera[component];
        double clip[4]{};
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column) clip[row] += screen.m[row][column] * world[column];
        Check(clip[3] > 0.0, "Visible frustum points must have positive clip W");
        Check(std::fabs(clip[0] / clip[3] - expectedX) < 2e-5, "Horizontal screen projection is incorrect");
        Check(std::fabs(clip[1] / clip[3] - expectedY) < 2e-5, "Vertical screen projection is incorrect");
        Check(std::fabs(clip[2] / clip[3] - expectedDepth) < 2e-5, "Native clip depth does not map near/far to zero/one");
    };
    projectWorld(0.0, 0.0, -nearDistance, 0.0, 0.0, 0.0);
    projectWorld(0.0, 0.0, -farDistance, 0.0, 0.0, 1.0);
    projectWorld(rightAtNear, topAtNear, -nearDistance, 1.0, 1.0, 0.0);
    projectWorld(-rightAtNear, -topAtNear, -nearDistance, -1.0, -1.0, 0.0);
    NativeMatrix offcenter = perspective;
    offcenter.m[0][2] = 0.02f;
    Check(!NativeProjectionDiagnostic(Compose(offcenter, view), view).at("available").get<bool>(), "Offcenter projection must be rejected");
    NativeMatrix orthographic{{{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, -0.1f, -0.05f}, {0.0f, 0.0f, 0.0f, 1.0f}}};
    Check(!NativeProjectionDiagnostic(Compose(orthographic, view), view).at("available").get<bool>(), "Orthographic projection must be rejected");
    NativeMatrix invalidDepth = perspective;
    invalidDepth.m[2][2] = -0.5f;
    Check(!NativeProjectionDiagnostic(Compose(invalidDepth, view), view).at("available").get<bool>(), "An invalid clipping range must be rejected");
    NativeMatrix singular{};
    Check(!NativeProjectionDiagnostic(screen, singular).at("available").get<bool>(), "A singular view matrix must be rejected");
    NativeMatrix nonfinite = screen;
    nonfinite.m[2][3] = std::numeric_limits<float>::infinity();
    Check(!NativeProjectionDiagnostic(nonfinite, view).at("available").get<bool>(), "An infinite native matrix must be rejected");
    NativeMatrix scaledView = view;
    scaledView.m[0][0] *= 2.0f;
    Check(!NativeProjectionDiagnostic(screen, scaledView).at("available").get<bool>(), "A non-rigid camera basis must be rejected");
}

int main()
{
    try
    {
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_ENGINE_CAMERA", nullptr);
        const auto snapshot = SampleEngineCamera();
        Check(snapshot.at("status") == "unavailable", "Host must report unavailable without engine.dll");
        Check(!snapshot.at("available").get<bool>(), "Missing engine must not be available");
        Check(snapshot.at("raw_world_to_screen").is_null(), "Missing engine must not synthesize a matrix");
        Check(!snapshot.at("camera_semantics_verified").get<bool>(), "Camera semantics must remain unverified");
        Check(!snapshot.at("frame_timing_verified").get<bool>(), "Frame timing must remain unverified");
        Check(snapshot.at("diagnostics_only").get<bool>(), "Raw camera samples must remain diagnostic only");
        Check(!snapshot.at("native_camera_ready").get<bool>(), "Diagnostic reads must not mark a camera ready");
        Check(!snapshot.at("enabled").get<bool>(), "Engine calls must be disabled by default");
        Check(!snapshot.at("engine_thread_safety_verified").get<bool>(), "Render callback must not imply engine thread safety");
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_ENGINE_CAMERA", L"1");
        const auto enabledSnapshot = SampleEngineCamera();
        Check(enabledSnapshot.at("enabled").get<bool>(), "Explicit engine diagnostic opt-in was not recognized");
        Check(enabledSnapshot.at("status") == "unavailable", "Enabled standalone host must still report missing engine");
        Check(enabledSnapshot.at("raw_world_to_screen").is_null(), "Missing engine must not return diagnostic matrices");
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_ENGINE_CAMERA", nullptr);

        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto bytes = static_cast<std::size_t>(system.dwPageSize) * 2;
        auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Check(memory != nullptr, "VirtualAlloc failed");
        DWORD oldProtect = 0;
        const bool protectedPage = VirtualProtect(memory + system.dwPageSize, system.dwPageSize, PAGE_NOACCESS, &oldProtect) != FALSE;
        Check(protectedPage, "VirtualProtect failed");
        Check(ReadableRange(memory, system.dwPageSize), "Committed readable page must be readable");
        Check(!ReadableRange(memory, system.dwPageSize + 1), "Range crossing a no-access page must be rejected");
        Check(!ExecutableAddress(reinterpret_cast<std::uintptr_t>(memory)), "Data page must not be executable");
        Check(!ReadableRange(nullptr, 4), "Null address must be rejected");
        Check(!ExecutableAddress(0), "Null code must be rejected");
        Check(ExecutableAddress(reinterpret_cast<std::uintptr_t>(GetTickCount64)), "System function must be executable");
        memory[0] = 0x45;
        unsigned char value = 0;
        Check(SafeCopy(&value, memory, 1) && value == 0x45, "Readable copy failed");
        Check(!SafeCopy(&value, memory + system.dwPageSize, 1), "Inaccessible copy must be rejected");
        VirtualFree(memory, 0, MEM_RELEASE);

        FakeEngine fake;
        Check(MatrixValid(fake.matrix), "Identity matrix must pass structural validation");
        NativeMatrix zero{};
        Check(!MatrixValid(zero), "Singular matrix must be rejected");
        NativeMatrix invalid = fake.matrix;
        invalid.m[2][3] = std::numeric_limits<float>::quiet_NaN();
        Check(!MatrixValid(invalid), "Non-finite matrix must be rejected");
        Check(MatrixDiagnostic(invalid, true).at("failure") == "not_finite", "Non-finite diagnostic must distinguish rank failure");
        Check(MatrixDiagnostic(zero, true).at("failure") == "rank_deficient", "Singular diagnostic must identify rank failure");
        Check(MatrixDiagnostic(fake.matrix, false).at("failure") == "copy_failed", "Failed copy must not classify guessed matrix data");
        Check(MatrixDiagnostic(fake.matrix, true).at("failure") == "none", "Valid matrix diagnostic failed");
        int width = 0;
        int height = 0;
        NativeAngle angle{};
        NativeMatrix matrix{};
        bool connected = false;
        Check(QueryBool(Address(&FakeEngine::Connected), &fake, connected) && connected, "Native bool member ABI failed");
        Check(QueryScreenSize(Address(&FakeEngine::Size), &fake, width, height) && width == 5760 && height == 3240, "Native screen-size member ABI failed");
        Check(QueryViewAngles(Address(&FakeEngine::Angle), &fake, angle) && angle.x == 10.0f && angle.z == 30.0f, "Native angle member ABI failed");
        Check(QueryMatrix(Address(&FakeEngine::Matrix), &fake, matrix) && std::memcmp(&matrix, &fake.matrix, sizeof(matrix)) == 0, "Native matrix-reference member ABI failed");
        fake.matrix.m[0][0] = 7.0f;
        Check(matrix.m[0][0] == 1.0f, "Matrix snapshot must own copied data");
        NativeMatrix stableScreen{}, stableView{};
        Json pairedValidation;
        Check(QueryStableMatrixPair(Address(&FakeEngine::Screen), Address(&FakeEngine::View), &fake, stableScreen, stableView, pairedValidation),
              "Unchanged paired matrix copies must pass");
        Check(pairedValidation.at("paired_copy_passes") == 2 && pairedValidation.at("stable_during_sample").get<bool>(), "Both paired passes must be reported");
        Check(!pairedValidation.at("frame_timing_verified").get<bool>() && !pairedValidation.at("concurrent_writers_excluded").get<bool>(),
              "Bitwise stability must not imply render-frame synchronization");
        fake.changeScreen = true;
        Check(!QueryStableMatrixPair(Address(&FakeEngine::Screen), Address(&FakeEngine::View), &fake, stableScreen, stableView, pairedValidation),
              "Changing screen matrices must fail paired consistency");
        fake.changeScreen = false;
        fake.changeView = true;
        Check(!QueryStableMatrixPair(Address(&FakeEngine::Screen), Address(&FakeEngine::View), &fake, stableScreen, stableView, pairedValidation),
              "Changing view matrices must fail paired consistency");
        ProjectionContract();
        std::cout << "{\"process_bits\":32,\"host_unavailable\":true,\"page_guards\":true,\"member_calling_convention\":true,\"matrix_copy\":true,\"finite_rank_validation\":true,\"paired_stability\":true,\"native_projection_geometry\":true,\"near_far_depth_zero_one\":true,\"invalid_projection_rejection\":true}\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
