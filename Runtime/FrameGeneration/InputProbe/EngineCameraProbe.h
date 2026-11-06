#pragma once

#include <windows.h>
#include <json.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace asteria_native_input
{
using Json = nlohmann::json;

namespace engine_camera_detail
{
enum EngineClient013Method : std::size_t
{
    GetScreenSizeMethod = 5,
    GetViewAnglesMethod = 19,
    IsInGameMethod = 26,
    IsConnectedMethod = 27,
    WorldToScreenMatrixMethod = 37,
    WorldToViewMatrixMethod = 38,
    RequiredMethodCount = 39
};

struct NativeAngle
{
    float x;
    float y;
    float z;
};

struct NativeMatrix
{
    float m[4][4];
};

static_assert(sizeof(NativeAngle) == 12);
static_assert(sizeof(NativeMatrix) == 64);

inline bool ReadableRange(const void* pointer, std::size_t bytes)
{
    if (!pointer || !bytes) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    if (bytes > UINTPTR_MAX - start) return false;
    const auto end = start + bytes;
    auto cursor = start;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) != sizeof(info)) return false;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD access = info.Protect & 0xff;
        if (access != PAGE_READONLY && access != PAGE_READWRITE && access != PAGE_WRITECOPY &&
            access != PAGE_EXECUTE_READ && access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.RegionSize > UINTPTR_MAX - base) return false;
        const auto regionEnd = base + info.RegionSize;
        if (cursor < base || regionEnd <= cursor) return false;
        cursor = regionEnd < end ? regionEnd : end;
    }
    return true;
}

inline bool ExecutableAddress(std::uintptr_t address)
{
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info)) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD access = info.Protect & 0xff;
    return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
        access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}

inline bool SafeCopy(void* destination, const void* source, std::size_t bytes)
{
    if (!destination || !ReadableRange(source, bytes)) return false;
#if defined(_MSC_VER)
    __try
    {
        std::memcpy(destination, source, bytes);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
#else
    std::memcpy(destination, source, bytes);
    return true;
#endif
}

inline bool MatrixFinite(const NativeMatrix& matrix)
{
    for (const auto& row : matrix.m)
        for (const float value : row)
            if (!std::isfinite(value)) return false;
    return true;
}

inline bool MatrixValid(const NativeMatrix& matrix)
{
    if (!MatrixFinite(matrix)) return false;
    double work[4][4]{};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            const float value = matrix.m[row][column];
            work[row][column] = value;
        }
    }
    for (int pivot = 0; pivot < 4; ++pivot)
    {
        int chosen = pivot;
        for (int row = pivot + 1; row < 4; ++row)
            if (std::fabs(work[row][pivot]) > std::fabs(work[chosen][pivot])) chosen = row;
        if (std::fabs(work[chosen][pivot]) < 1e-20) return false;
        if (chosen != pivot)
        {
            for (int column = 0; column < 4; ++column)
            {
                const double value = work[pivot][column];
                work[pivot][column] = work[chosen][column];
                work[chosen][column] = value;
            }
        }
        for (int row = pivot + 1; row < 4; ++row)
        {
            const double scale = work[row][pivot] / work[pivot][pivot];
            for (int column = pivot; column < 4; ++column) work[row][column] -= scale * work[pivot][column];
        }
    }
    return true;
}

inline Json MatrixJson(const NativeMatrix& matrix)
{
    Json result = Json::array();
    for (const auto& row : matrix.m) result.push_back({row[0], row[1], row[2], row[3]});
    return result;
}

inline Json MatrixDiagnostic(const NativeMatrix& matrix, bool copied)
{
    Json result = {{"copied", copied}, {"finite", nullptr}, {"full_rank", nullptr}, {"failure", "copy_failed"}};
    if (!copied) return result;
    const bool finite = MatrixFinite(matrix);
    result["finite"] = finite;
    if (!finite)
    {
        result["failure"] = "not_finite";
        return result;
    }
    const bool fullRank = MatrixValid(matrix);
    result["full_rank"] = fullRank;
    result["failure"] = fullRank ? "none" : "rank_deficient";
    return result;
}

struct DoubleMatrix
{
    double m[4][4];
};

inline bool MatrixInverse(const NativeMatrix& source, DoubleMatrix& inverse)
{
    if (!MatrixFinite(source)) return false;
    double work[4][8]{};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column) work[row][column] = source.m[row][column];
        work[row][row + 4] = 1.0;
    }
    for (int pivot = 0; pivot < 4; ++pivot)
    {
        int chosen = pivot;
        for (int row = pivot + 1; row < 4; ++row)
            if (std::fabs(work[row][pivot]) > std::fabs(work[chosen][pivot])) chosen = row;
        if (!std::isfinite(work[chosen][pivot]) || std::fabs(work[chosen][pivot]) < 1e-12) return false;
        for (int column = 0; column < 8; ++column)
        {
            const double temporary = work[pivot][column];
            work[pivot][column] = work[chosen][column];
            work[chosen][column] = temporary;
        }
        const double divisor = work[pivot][pivot];
        for (int column = 0; column < 8; ++column) work[pivot][column] /= divisor;
        for (int row = 0; row < 4; ++row)
        {
            if (row == pivot) continue;
            const double factor = work[row][pivot];
            for (int column = 0; column < 8; ++column) work[row][column] -= factor * work[pivot][column];
        }
    }
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            inverse.m[row][column] = work[row][column + 4];
            if (!std::isfinite(inverse.m[row][column])) return false;
        }
    return true;
}

inline Json NativeProjectionDiagnostic(const NativeMatrix& worldToScreen, const NativeMatrix& worldToView,
                                       uint32_t renderWidth = 0, uint32_t renderHeight = 0)
{
    Json result = {{"status", "unavailable"}, {"available", false}, {"diagnostics_only", true},
                   {"derived_from_native_matrices", true}, {"native_camera_ready", false},
                   {"camera_semantics_verified", false}, {"frame_timing_verified", false},
                   {"projection", nullptr}, {"origin", nullptr}, {"near_plane", nullptr}, {"far_plane", nullptr},
                   {"horizontal_fov_degrees", nullptr}, {"vertical_fov_degrees", nullptr},
                   {"projection_aspect_ratio", nullptr}, {"render_surface_aspect_matches_projection", nullptr}};
    const auto unavailable = [&result](const char* reason) -> Json
    {
        result["reason"] = reason;
        return result;
    };
    if (!MatrixFinite(worldToScreen) || !MatrixFinite(worldToView)) return unavailable("non_finite_native_matrix");
    DoubleMatrix inverse{};
    if (!MatrixInverse(worldToView, inverse) || !MatrixValid(worldToScreen)) return unavailable("singular_native_matrix");
    constexpr double affineTolerance = 1e-5;
    if (std::fabs(worldToView.m[3][0]) > affineTolerance || std::fabs(worldToView.m[3][1]) > affineTolerance ||
        std::fabs(worldToView.m[3][2]) > affineTolerance || std::fabs(worldToView.m[3][3] - 1.0) > affineTolerance)
        return unavailable("native_view_is_not_affine");
    for (int row = 0; row < 3; ++row)
        for (int other = 0; other < 3; ++other)
        {
            double dot = 0.0;
            for (int column = 0; column < 3; ++column) dot += static_cast<double>(worldToView.m[row][column]) * worldToView.m[other][column];
            if (std::fabs(dot - (row == other ? 1.0 : 0.0)) > 1e-3) return unavailable("native_view_basis_is_not_orthonormal");
        }
    DoubleMatrix projection{};
    double translationScale = 1.0;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            for (int component = 0; component < 4; ++component)
                projection.m[row][column] += static_cast<double>(worldToScreen.m[row][component]) * inverse.m[component][column];
            if (!std::isfinite(projection.m[row][column])) return unavailable("non_finite_recovered_projection");
            if (column == 3 && std::fabs(worldToScreen.m[row][column]) > translationScale)
                translationScale = std::fabs(worldToScreen.m[row][column]);
        }
    constexpr double structuralTolerance = 1e-4;
    const double translationTolerance = 2e-6 * translationScale + 1e-4;
    if (std::fabs(projection.m[3][2] + 1.0) > structuralTolerance || std::fabs(projection.m[3][3]) > translationTolerance)
        return unavailable("unsupported_orthographic_or_nonstandard_projection");
    if (std::fabs(projection.m[0][2]) > structuralTolerance || std::fabs(projection.m[1][2]) > structuralTolerance)
        return unavailable("offcenter_or_jittered_projection");
    const int zeroElements[][2] = {{0, 1}, {1, 0}, {2, 0}, {2, 1}, {3, 0}, {3, 1}};
    for (const auto& element : zeroElements)
        if (std::fabs(projection.m[element[0]][element[1]]) > structuralTolerance)
            return unavailable("nonstandard_projection_cross_terms");
    if (std::fabs(projection.m[0][3]) > translationTolerance || std::fabs(projection.m[1][3]) > translationTolerance)
        return unavailable("native_matrix_pair_has_camera_translation_residual");
    const double scaleX = projection.m[0][0];
    const double scaleY = projection.m[1][1];
    const double depthScale = projection.m[2][2];
    const double depthOffset = projection.m[2][3];
    if (!(scaleX > 0.0 && scaleY > 0.0 && depthScale < -1.0 && depthOffset < 0.0))
        return unavailable("invalid_perspective_scale_or_depth_range");
    const double nearPlane = depthOffset / depthScale;
    const double farPlane = depthOffset / (depthScale + 1.0);
    const double pi = 3.14159265358979323846;
    const double horizontalFov = 2.0 * std::atan(1.0 / scaleX) * 180.0 / pi;
    const double verticalFov = 2.0 * std::atan(1.0 / scaleY) * 180.0 / pi;
    const double aspect = scaleY / scaleX;
    if (!std::isfinite(nearPlane) || !std::isfinite(farPlane) || !std::isfinite(horizontalFov) ||
        !std::isfinite(verticalFov) || !std::isfinite(aspect) || !(nearPlane > 0.0 && farPlane > nearPlane &&
        horizontalFov > 0.0 && horizontalFov < 180.0 && verticalFov > 0.0 && verticalFov < 180.0 && aspect > 0.0))
        return unavailable("invalid_recovered_clip_planes_or_fov");
    result["projection"] = Json::array();
    for (const auto& row : projection.m) result["projection"].push_back({row[0], row[1], row[2], row[3]});
    result["origin"] = {inverse.m[0][3], inverse.m[1][3], inverse.m[2][3]};
    result["near_plane"] = nearPlane;
    result["far_plane"] = farPlane;
    result["horizontal_fov_degrees"] = horizontalFov;
    result["vertical_fov_degrees"] = verticalFov;
    result["projection_aspect_ratio"] = aspect;
    result["native_clip_depth_range"] = {0.0, 1.0};
    if (renderWidth && renderHeight)
    {
        const double surfaceAspect = static_cast<double>(renderWidth) / renderHeight;
        result["render_surface_aspect_ratio"] = surfaceAspect;
        result["render_surface_aspect_matches_projection"] = std::fabs(aspect / surfaceAspect - 1.0) <= 0.005;
    }
    result["status"] = "available";
    result["available"] = true;
    result["reason"] = "Standard native perspective recovered; main-view semantics and frame association remain unverified";
    return result;
}

#if defined(_MSC_VER) && defined(_M_IX86)
using CreateInterfaceFn = void* (__cdecl*)(const char*, int*);
using BoolQueryFn = bool (__thiscall*)(void*);
using ScreenSizeFn = void (__thiscall*)(void*, int&, int&);
using ViewAnglesFn = void (__thiscall*)(void*, NativeAngle&);
using MatrixQueryFn = const NativeMatrix& (__thiscall*)(void*);

inline bool QueryFactory(CreateInterfaceFn factory, void*& object, int& returnCode)
{
    __try
    {
        object = factory("VEngineClient013", &returnCode);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

inline bool QueryBool(std::uintptr_t address, void* object, bool& output)
{
    __try
    {
        output = reinterpret_cast<BoolQueryFn>(address)(object);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

inline bool QueryScreenSize(std::uintptr_t address, void* object, int& width, int& height)
{
    __try
    {
        reinterpret_cast<ScreenSizeFn>(address)(object, width, height);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

inline bool QueryViewAngles(std::uintptr_t address, void* object, NativeAngle& angles)
{
    __try
    {
        reinterpret_cast<ViewAnglesFn>(address)(object, angles);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

inline bool QueryMatrix(std::uintptr_t address, void* object, NativeMatrix& output)
{
    const NativeMatrix* matrix = nullptr;
    __try
    {
        matrix = &reinterpret_cast<MatrixQueryFn>(address)(object);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return SafeCopy(&output, matrix, sizeof(output));
}

inline bool QueryStableMatrixPair(std::uintptr_t screenMethod, std::uintptr_t viewMethod, void* object,
                                  NativeMatrix& worldToScreen, NativeMatrix& worldToView, Json& validation)
{
    NativeMatrix secondScreen{}, secondView{};
    const bool firstScreen = QueryMatrix(screenMethod, object, worldToScreen);
    const bool firstView = QueryMatrix(viewMethod, object, worldToView);
    const bool repeatedScreen = QueryMatrix(screenMethod, object, secondScreen);
    const bool repeatedView = QueryMatrix(viewMethod, object, secondView);
    const bool copied = firstScreen && firstView && repeatedScreen && repeatedView;
    const bool screenEqual = copied && std::memcmp(&worldToScreen, &secondScreen, sizeof(worldToScreen)) == 0;
    const bool viewEqual = copied && std::memcmp(&worldToView, &secondView, sizeof(worldToView)) == 0;
    validation = {{"paired_copy_passes", 2}, {"all_copies_completed", copied},
                  {"world_to_screen_equal_bitwise", screenEqual}, {"world_to_view_equal_bitwise", viewEqual},
                  {"stable_during_sample", screenEqual && viewEqual},
                  {"concurrent_writers_excluded", false}, {"frame_timing_verified", false},
                  {"first_screen_copied", firstScreen}, {"first_view_copied", firstView},
                  {"second_screen_copied", repeatedScreen}, {"second_view_copied", repeatedView}};
    return copied && screenEqual && viewEqual;
}
#endif
}

inline bool RetailCameraProfileMatches(HMODULE module, void* object, const std::uintptr_t* table,
                                      const std::uintptr_t* methods, Json& profile)
{
    using namespace engine_camera_detail;
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};
    if (!SafeCopy(&dos, module, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 1048576) return false;
    IMAGE_NT_HEADERS32 nt{};
    if (!SafeCopy(&nt, reinterpret_cast<const void*>(base + dos.e_lfanew), sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt.FileHeader.TimeDateStamp != 0x6a440ed6 ||
        nt.OptionalHeader.SizeOfImage != 0x73f000) return false;
    if (reinterpret_cast<std::uintptr_t>(object) != base + 0x425504 ||
        reinterpret_cast<std::uintptr_t>(table) != base + 0x359dec ||
        methods[IsInGameMethod] != base + 0x5f1a0 || methods[IsConnectedMethod] != base + 0x5f1b0 ||
        methods[WorldToScreenMatrixMethod] != base + 0x5d620 || methods[WorldToViewMatrixMethod] != base + 0x5d630) return false;
    const unsigned char expectedInGame[] = {0xe8, 0xfb, 0x3a, 0x02, 0x00, 0x33, 0xc9, 0x83, 0x78, 0x68, 0x06, 0x0f, 0x94, 0xc0, 0xc3};
    const unsigned char expectedConnected[] = {0xe8, 0xeb, 0x3a, 0x02, 0x00, 0x33, 0xc9, 0x83, 0x78, 0x68, 0x02, 0x0f, 0x9d, 0xc0, 0xc3};
    unsigned char inGameCode[sizeof(expectedInGame)]{}, connectedCode[sizeof(expectedConnected)]{}, stateAccessor[9]{};
    if (!SafeCopy(inGameCode, reinterpret_cast<const void*>(methods[IsInGameMethod]), sizeof(inGameCode)) ||
        !SafeCopy(connectedCode, reinterpret_cast<const void*>(methods[IsConnectedMethod]), sizeof(connectedCode)) ||
        !SafeCopy(stateAccessor, reinterpret_cast<const void*>(base + 0x82ca0), sizeof(stateAccessor)) ||
        std::memcmp(inGameCode, expectedInGame, sizeof(inGameCode)) || std::memcmp(connectedCode, expectedConnected, sizeof(connectedCode))) return false;
    const unsigned char stateAccessorTail[] = {0x83, 0xc0, 0x08, 0xc3};
    std::uintptr_t stateOperand{};
    std::memcpy(&stateOperand, stateAccessor + 1, sizeof(stateOperand));
    if (stateAccessor[0] != 0xa1 || stateOperand != base + 0x4268ec ||
        std::memcmp(stateAccessor + 5, stateAccessorTail, sizeof(stateAccessorTail))) return false;
    const unsigned char expectedScreen[] = {0x83, 0xb9, 0xe8, 0x00, 0x00, 0x00, 0x01, 0x7e, 0x17, 0x8b, 0x81, 0xe8, 0x00, 0x00, 0x00, 0x8b, 0x89, 0xdc, 0x00, 0x00, 0x00, 0x69, 0xc0, 0x94, 0x01, 0x00, 0x00, 0x8d, 0x44, 0x08, 0xbc, 0xc3, 0x8d, 0x81, 0x9c, 0x00, 0x00, 0x00, 0xc3};
    const unsigned char expectedView[] = {0x83, 0xb9, 0xe8, 0x00, 0x00, 0x00, 0x01, 0x7e, 0x1a, 0x8b, 0x81, 0xe8, 0x00, 0x00, 0x00, 0x8b, 0x89, 0xdc, 0x00, 0x00, 0x00, 0x69, 0xc0, 0x94, 0x01, 0x00, 0x00, 0x8d, 0x84, 0x08, 0x3c, 0xff, 0xff, 0xff, 0xc3, 0x8d, 0x41, 0x1c, 0xc3};
    unsigned char screenCode[sizeof(expectedScreen)]{}, viewCode[sizeof(expectedView)]{};
    if (!SafeCopy(screenCode, reinterpret_cast<const void*>(base + 0xc3160), sizeof(screenCode)) ||
        !SafeCopy(viewCode, reinterpret_cast<const void*>(base + 0xc3130), sizeof(viewCode)) ||
        std::memcmp(screenCode, expectedScreen, sizeof(screenCode)) || std::memcmp(viewCode, expectedView, sizeof(viewCode))) return false;
    unsigned char screenStub[13]{}, viewStub[13]{};
    if (!SafeCopy(screenStub, reinterpret_cast<const void*>(methods[WorldToScreenMatrixMethod]), sizeof(screenStub)) ||
        !SafeCopy(viewStub, reinterpret_cast<const void*>(methods[WorldToViewMatrixMethod]), sizeof(viewStub))) return false;
    const unsigned char screenTail[] = {0x8b, 0x01, 0x8b, 0x50, 0x34, 0xff, 0xe2};
    const unsigned char viewTail[] = {0x8b, 0x01, 0x8b, 0x50, 0x30, 0xff, 0xe2};
    std::uintptr_t screenOperand{}, viewOperand{}, renderer{}, rendererTable{};
    std::memcpy(&screenOperand, screenStub + 2, sizeof(screenOperand));
    std::memcpy(&viewOperand, viewStub + 2, sizeof(viewOperand));
    if (screenStub[0] != 0x8b || screenStub[1] != 0x0d || viewStub[0] != 0x8b || viewStub[1] != 0x0d ||
        screenOperand != base + 0x427bc0 || viewOperand != screenOperand ||
        std::memcmp(screenStub + 6, screenTail, sizeof(screenTail)) || std::memcmp(viewStub + 6, viewTail, sizeof(viewTail)) ||
        !SafeCopy(&renderer, reinterpret_cast<const void*>(screenOperand), sizeof(renderer)) || renderer != base + 0x601f00 ||
        !SafeCopy(&rendererTable, reinterpret_cast<const void*>(renderer), sizeof(rendererTable)) || rendererTable != base + 0x3725b4) return false;
    std::uintptr_t nativeScreen{}, nativeView{};
    if (!SafeCopy(&nativeScreen, reinterpret_cast<const void*>(rendererTable + 13 * sizeof(std::uintptr_t)), sizeof(nativeScreen)) ||
        !SafeCopy(&nativeView, reinterpret_cast<const void*>(rendererTable + 12 * sizeof(std::uintptr_t)), sizeof(nativeView)) ||
        nativeScreen != base + 0xc3160 || nativeView != base + 0xc3130) return false;
    profile = {{"mapped_binary_layout_verified", true}, {"sdk_declaration_slots_match", false},
               {"pinned_file_sha256", "8a6d468059394bf4cfce32f11dcba12ef45de88da1373084d5a2916d0bf7ba86"},
               {"timestamp", nt.FileHeader.TimeDateStamp}, {"image_bytes", nt.OptionalHeader.SizeOfImage},
               {"matrix_slots", {37, 38}}, {"matrix_getter_bodies_read_only", true},
               {"game_state_getter_bodies_read_only", true}, {"game_state_accessor_body_read_only", true},
               {"material_context_getters_called", false}};
    return true;
}

inline Json SampleEngineCamera(uint32_t renderWidth = 0, uint32_t renderHeight = 0)
{
    wchar_t cameraOption[4]{};
    const DWORD optionLength = GetEnvironmentVariableW(L"ASTERIA_NATIVE_ENGINE_CAMERA", cameraOption, 4);
    const bool cameraEnabled = optionLength == 1 && cameraOption[0] == L'1';
    Json result = {
        {"status", "unavailable"},
        {"available", false},
        {"enabled", cameraEnabled},
        {"diagnostics_only", true},
        {"native_camera_ready", false},
        {"interface", "VEngineClient013"},
        {"camera_semantics_verified", false},
        {"frame_timing_verified", false},
        {"engine_thread_safety_verified", false},
        {"native_motion_vectors_available", false},
        {"sample_thread_id", GetCurrentThreadId()},
        {"sample_tick_ms", GetTickCount64()},
        {"raw_view_angles", nullptr},
        {"raw_world_to_screen", nullptr},
        {"raw_world_to_view", nullptr},
        {"raw_engine_screen_size", nullptr},
        {"render_surface_size", {renderWidth, renderHeight}},
        {"render_surface_size_source", "ReShade effect runtime; not an engine getter"},
        {"retail_profile", {{"mapped_binary_layout_verified", false}}},
        {"matrix_pair_validation", {{"stable_during_sample", false}}},
        {"native_projection_diagnostic", nullptr},
        {"is_in_game", nullptr},
        {"is_connected", nullptr}
    };
    result["matrix_validation"] = {
        {"world_to_screen", {{"copied", false}, {"finite", nullptr}, {"full_rank", nullptr}, {"failure", "not_sampled"}}},
        {"world_to_view", {{"copied", false}, {"finite", nullptr}, {"full_rank", nullptr}, {"failure", "not_sampled"}}}
    };
    const auto unavailable = [&result](const char* reason) -> Json
    {
        result["reason"] = reason;
        return result;
    };
    if (!cameraEnabled) return unavailable("Engine camera calls are disabled; ASTERIA_NATIVE_ENGINE_CAMERA=1 opts into unverified diagnostics");
#if defined(_MSC_VER) && defined(_M_IX86)
    using namespace engine_camera_detail;
    const HMODULE engineModule = GetModuleHandleW(L"engine.dll");
    if (!engineModule) return unavailable("engine.dll is not loaded");
    const auto factoryAddress = reinterpret_cast<std::uintptr_t>(GetProcAddress(engineModule, "CreateInterface"));
    if (!ExecutableAddress(factoryAddress)) return unavailable("engine CreateInterface is not executable");
    void* engine = nullptr;
    int returnCode = -1;
    if (!QueryFactory(reinterpret_cast<CreateInterfaceFn>(factoryAddress), engine, returnCode))
        return unavailable("engine interface factory raised an exception");
    if (returnCode != 0 || !engine) return unavailable("VEngineClient013 is not available");

    std::uintptr_t* table = nullptr;
    if (!SafeCopy(&table, engine, sizeof(table))) return unavailable("engine interface object is not readable");
    std::uintptr_t methods[RequiredMethodCount]{};
    if (!SafeCopy(methods, table, sizeof(methods))) return unavailable("engine interface table is not readable");
    const EngineClient013Method required[] = {
        IsInGameMethod, IsConnectedMethod, WorldToScreenMatrixMethod, WorldToViewMatrixMethod
    };
    for (const auto method : required)
        if (!ExecutableAddress(methods[method])) return unavailable("engine camera method is not executable");
    if (!RetailCameraProfileMatches(engineModule, engine, table, methods, result["retail_profile"]))
        return unavailable("Loaded retail engine does not match the verified binary layout and pure matrix getter profile");

    bool inGame = false;
    bool connected = false;
    if (!QueryBool(methods[IsInGameMethod], engine, inGame) || !QueryBool(methods[IsConnectedMethod], engine, connected))
        return unavailable("engine game-state query raised an exception");
    result["is_in_game"] = inGame;
    result["is_connected"] = connected;
    if (!inGame || !connected) return unavailable("game is not active and connected");

    NativeMatrix worldToScreen{};
    NativeMatrix worldToView{};
    const bool stablePair = QueryStableMatrixPair(methods[WorldToScreenMatrixMethod], methods[WorldToViewMatrixMethod],
                                                 engine, worldToScreen, worldToView, result["matrix_pair_validation"]);
    const bool screenCopied = result["matrix_pair_validation"]["first_screen_copied"].get<bool>();
    const bool viewCopied = result["matrix_pair_validation"]["first_view_copied"].get<bool>();
    const Json screenCheck = MatrixDiagnostic(worldToScreen, screenCopied);
    const Json viewCheck = MatrixDiagnostic(worldToView, viewCopied);
    result["matrix_validation"]["world_to_screen"] = screenCheck;
    result["matrix_validation"]["world_to_view"] = viewCheck;
    if (screenCopied && MatrixFinite(worldToScreen)) result["raw_world_to_screen"] = MatrixJson(worldToScreen);
    if (viewCopied && MatrixFinite(worldToView)) result["raw_world_to_view"] = MatrixJson(worldToView);
    if (screenCheck["failure"] != "none" || viewCheck["failure"] != "none")
    {
        result["reason"] = "Native matrix diagnostic validation failed";
        result["failed_matrix_checks"] = {
            {"world_to_screen", screenCheck["failure"]}, {"world_to_view", viewCheck["failure"]}
        };
        return result;
    }
    if (!stablePair) return unavailable("Native camera matrices changed between paired copies or a repeated copy failed");
    result["native_projection_diagnostic"] = NativeProjectionDiagnostic(worldToScreen, worldToView, renderWidth, renderHeight);
    result["status"] = "available";
    result["available"] = true;
    result["reason"] = "Native interface reads completed; camera semantics and frame timing remain unverified";
    return result;
#else
    return unavailable("engine camera probe requires Windows x86 MSVC");
#endif
}
}
