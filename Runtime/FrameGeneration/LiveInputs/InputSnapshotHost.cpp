#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <json.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Json = nlohmann::json;
static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{ return DefWindowProcW(window, message, wparam, lparam); }
static Json ReadJson(const fs::path &path) { std::ifstream input(path, std::ios::binary); return Json::parse(input); }
static void Require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
struct Vertex { float x, y, z, rhw; DWORD color; };
static void Draw(IDirect3DDevice9 *device, float x0, float x1, float z, DWORD color)
{
    const Vertex vertices[] = {
        {x0, 60, z, 1, color}, {x1, 60, z, 1, color}, {x1, 300, z, 1, color},
        {x0, 60, z, 1, color}, {x1, 300, z, 1, color}, {x0, 300, z, 1, color}};
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, vertices, sizeof(Vertex));
}
int wmain(int argc, wchar_t **argv)
{
    const fs::path component = fs::absolute(fs::path(argv[0])).parent_path().parent_path();
    const fs::path runtime = component.parent_path().parent_path();
    bool noDepth = false, disabled = false, invalidate = false, resize = false, unqualified = false;
    fs::path dxvkSource;
    for (int index = 1; index < argc; ++index)
    {
        if (std::wstring(argv[index]) == L"--no-depth") noDepth = true;
        else if (std::wstring(argv[index]) == L"--disabled") disabled = true;
        else if (std::wstring(argv[index]) == L"--invalidate-source") invalidate = true;
        else if (std::wstring(argv[index]) == L"--resize") resize = true;
        else if (std::wstring(argv[index]) == L"--unqualified-context") unqualified = true;
        else if (std::wstring(argv[index]) == L"--dxvk" && index + 1 < argc) dxvkSource = fs::absolute(argv[++index]);
        else { std::cerr << "Usage: --dxvk <existing x86 DLL> [--no-depth|--disabled|--invalidate-source|--resize]\n"; return 1; }
    }
    const std::wstring prefix = dxvkSource.empty() ? L"InputSnapshot" : L"InputSnapshotVulkan";
    const auto fixture = component / L"results" / (prefix + (disabled ? L"Disabled" : noDepth ? L"NoDepth" : invalidate ? L"Invalidated" : resize ? L"Resize" : unqualified ? L"Unqualified" : L""));
    const auto host = fixture / L"ReShadeHost", addons = fixture / L"TestAddons", effects = fixture / L"Effects", local = fixture / L"LocalAppData";
    const auto status = fixture / L"snapshot_status.json";
    Json phases = Json::array();
    Json report = {{"author", "Anokianx"}, {"mode", "isolated-reshade-owned-input-snapshot-host"}, {"process_bits", sizeof(void *) * 8},
        {"passed", false}, {"no_depth_control", noDepth}, {"disabled_control", disabled},
        {"dxvk_vulkan", !dxvkSource.empty()}, {"invalidate_source_fixture", invalidate}, {"resize_fixture", resize}, {"layer_registration_changed", false}, {"game_files_modified", false},
        {"main_view_confirmed", false}, {"native_fg_ready", false}, {"game_started", false}};
    HMODULE reshade = nullptr;
    HMODULE dxvk = nullptr;
    HWND window = nullptr;
    IDirect3D9 *direct3d = nullptr;
    IDirect3DDevice9 *device = nullptr;
    IDirect3DSurface9 *depth = nullptr;
    auto release = [&] {
        if (device) device->SetDepthStencilSurface(nullptr);
        if (depth) { depth->Release(); depth = nullptr; }
        if (device) { device->Release(); device = nullptr; }
        if (direct3d) { direct3d->Release(); direct3d = nullptr; }
        if (window) { DestroyWindow(window); window = nullptr; }
        if (reshade) { FreeLibrary(reshade); reshade = nullptr; }
        if (dxvk) { FreeLibrary(dxvk); dxvk = nullptr; }
    };
    try
    {
        Require(sizeof(void *) == 4 && !dxvkSource.empty(), "Input snapshot fixture requires x86 DXVK/Vulkan");
        fs::create_directories(host); fs::create_directories(addons); fs::create_directories(effects); fs::create_directories(local);
        fs::remove(status);
        fs::copy_file(component / L"bin" / (invalidate ? L"AsteriaInputSnapshotFixture.addon" : L"AsteriaInputSnapshot.addon"), addons / L"AsteriaInputSnapshot.addon", fs::copy_options::overwrite_existing);
        fs::copy_file(component / L"Shaders" / L"AsteriaInputSnapshot.fx", effects / L"AsteriaInputSnapshot.fx", fs::copy_options::overwrite_existing);
        const auto config = host / L"ReShade.ini";
        std::ofstream(config) << "[GENERAL]\nPerformanceMode=0\n[DEPTH]\nDepthCopyBeforeClears=0\n[OVERLAY]\nTutorialProgress=4\n";
        WritePrivateProfileStringW(L"ADDON", L"AddonPath", addons.c_str(), config.c_str());
        WritePrivateProfileStringW(L"GENERAL", L"EffectSearchPaths", effects.c_str(), config.c_str());
        WritePrivateProfileStringW(L"GENERAL", L"PresetPath", L"AsteriaSnapshot.ini", config.c_str());
        std::ofstream(host / L"AsteriaSnapshot.ini") << "Techniques=AsteriaInputSnapshot@AsteriaInputSnapshot.fx\nTechniqueSorting=AsteriaInputSnapshot@AsteriaInputSnapshot.fx\n";
        SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
        SetEnvironmentVariableW(L"RESHADE_BASE_PATH_OVERRIDE", host.c_str());
        SetEnvironmentVariableW(L"ASTERIA_INPUT_SNAPSHOT", disabled ? L"0" : L"1");
        SetEnvironmentVariableW(L"ASTERIA_INPUT_SNAPSHOT_CONTROLLED_HOST",unqualified ? L"0" : L"1");
        SetEnvironmentVariableW(L"ASTERIA_INPUT_SNAPSHOT_OUTPUT", status.c_str());
        SetEnvironmentVariableW(L"ASTERIA_INPUT_SNAPSHOT_TEST_INVALIDATE_SOURCE", invalidate ? L"1" : L"0");
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_INPUT_PROBE", L"0");
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_PIPELINE_PROBE", L"0");
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_UNIFORM_PROBE", L"0");
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_ENGINE_CAMERA", L"0");
        using CreateDirect3D = IDirect3D9 *(WINAPI *)(UINT);
        CreateDirect3D createDirect3D = &Direct3DCreate9;
        if (dxvkSource.empty())
        {
            SetEnvironmentVariableW(L"RESHADE_DISABLE_GRAPHICS_HOOK", nullptr);
            reshade = LoadLibraryW((runtime / L"Host" / L"ReShade32.dll").c_str());
            Require(reshade != nullptr, "ReShade LoadLibrary failed");
        }
        else
        {
            Require(fs::is_regular_file(dxvkSource), "The optional DXVK fixture needs an existing DLL");
            const auto layers = fixture / L"Layers";
            fs::create_directories(layers);
            Json manifest = ReadJson(runtime / L"Host" / L"ReShade32.json");
            Require(manifest.at("layer").at("name") == "VK_LAYER_Asteria_ReShade", "Unexpected ReShade layer identity");
            manifest["layer"]["library_path"] = (runtime / L"Host" / L"ReShade32.dll").string();
            std::ofstream(layers / L"ReShade32.json") << manifest.dump(2);
            const auto deployed = fixture / L"AsteriaFixtureDXVK.dll";
            fs::copy_file(dxvkSource, deployed, fs::copy_options::overwrite_existing);
            SetEnvironmentVariableW(L"VK_LAYER_PATH", layers.c_str());
            SetEnvironmentVariableW(L"VK_INSTANCE_LAYERS", L"VK_LAYER_Asteria_ReShade");
            SetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE", L"~implicit~");
            SetEnvironmentVariableW(L"DISABLE_VK_LAYER_reshade_1", L"1");
            SetEnvironmentVariableW(L"RESHADE_DISABLE_GRAPHICS_HOOK", L"1");
            SetEnvironmentVariableW(L"DXVK_LOG_PATH", fixture.c_str());
            SetEnvironmentVariableW(L"DXVK_STATE_CACHE_PATH", fixture.c_str());
            const auto dxvkConfig = fixture / L"dxvk.conf";
            std::ofstream(dxvkConfig) << "dxvk.enableStateCache = False\ndxvk.hud =\n";
            SetEnvironmentVariableW(L"DXVK_CONFIG_FILE", dxvkConfig.c_str());
            dxvk = LoadLibraryExW(deployed.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            Require(dxvk != nullptr, "DXVK fixture LoadLibrary failed");
            createDirect3D = reinterpret_cast<CreateDirect3D>(GetProcAddress(dxvk, "Direct3DCreate9"));
            Require(createDirect3D != nullptr, "DXVK Direct3DCreate9 entry point is absent");
            report["dxvk_source"] = dxvkSource.string();
            report["dxvk_fixture_copy"] = deployed.string();
        }
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr); klass.lpfnWndProc = WindowProc; klass.lpszClassName = L"AsteriaInputSnapshotHost";
        RegisterClassW(&klass);
        window = CreateWindowExW(0, klass.lpszClassName, L"Asteria Depth Semantic Host", WS_POPUP, 0, 0, 640, 360, nullptr, nullptr, klass.hInstance, nullptr);
        direct3d = createDirect3D(D3D_SDK_VERSION);
        Require(window && direct3d, "D3D9 host initialization failed");
        D3DPRESENT_PARAMETERS parameters{};
        parameters.Windowed = TRUE; parameters.SwapEffect = D3DSWAPEFFECT_DISCARD; parameters.hDeviceWindow = window;
        parameters.BackBufferFormat = D3DFMT_A8R8G8B8; parameters.BackBufferWidth = 640; parameters.BackBufferHeight = 360;
        parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        Require(SUCCEEDED(direct3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &parameters, &device)), "D3D9 CreateDevice failed");
        if (!noDepth)
        {
            Require(SUCCEEDED(device->CreateDepthStencilSurface(640, 360, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, nullptr)), "Depth fixture allocation failed");
            Require(SUCCEEDED(device->SetDepthStencilSurface(depth)), "Per-frame depth attachment binding failed");
        }
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_ZENABLE, noDepth ? FALSE : TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        unsigned frames = 0, phaseOffset = 0;
        bool resized = false;
        Json rendered = Json::array();
        Json observed;
    
        bool ready = false;
        std::string lastReadError;
        while (std::chrono::steady_clock::now() < deadline && !ready)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            device->SetDepthStencilSurface(depth);
            device->Clear(0, nullptr, D3DCLEAR_TARGET | (noDepth ? 0 : D3DCLEAR_ZBUFFER), D3DCOLOR_XRGB(4, 8, 16), 1, 0);
            const unsigned sample = frames + 1;
            const std::array<unsigned, 4> leftColor{80 + (sample * 37) % 160, 20 + (sample * 53) % 100, 10, 255};
            const std::array<unsigned, 4> rightColor{10, 50, 80 + (sample * 29) % 160, 255};
            const float leftDepth = .2f + .001f * (sample % 97), rightDepth = .6f + .001f * (sample % 89);
            if (SUCCEEDED(device->BeginScene()))
            {
                for (unsigned repeat = 0; repeat < 6; ++repeat)
                {
                    Draw(device, 40, 280, leftDepth, D3DCOLOR_XRGB(leftColor[0], leftColor[1], leftColor[2]));
                    Draw(device, 360, 600, rightDepth, D3DCOLOR_XRGB(rightColor[0], rightColor[1], rightColor[2]));
                }
                device->EndScene();
            }
            Require(SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr)), "D3D9 Present failed");
            ++frames;
            rendered.push_back({{"present_id", frames}, {"left_device_depth", leftDepth}, {"right_device_depth", rightDepth}, {"background_device_depth", 1.0f}, {"width", parameters.BackBufferWidth}, {"height", parameters.BackBufferHeight}, {"left_rgba8", leftColor}, {"right_rgba8", rightColor}});
            if (!disabled && !unqualified && frames - phaseOffset == 20) std::this_thread::sleep_for(std::chrono::seconds(6));
            if (frames - phaseOffset >= 100)
            {
                try
                {
                    if (disabled || unqualified) ready = !fs::exists(status);
                    else
                    {
                        observed = ReadJson(status);
                        ready = observed.at("borrow_retired_after_callback").get<bool>() &&
                            observed.at("last_issue").get<std::string>().empty() &&
                            observed.at("completed_readbacks").get<unsigned>() >= 60 && observed.at("captures").size() == 6;
                    }
                    if (ready && resize && !resized)
                    {
                        phases.push_back({{"present_id_offset", phaseOffset}, {"observation", observed}});
                        std::ofstream(fixture / L"initial_snapshot_status.json") << observed.dump(2);
                        phaseOffset = frames;
                        device->SetDepthStencilSurface(nullptr);
                        if (depth) { depth->Release(); depth = nullptr; }
                        parameters.BackBufferWidth = 512; parameters.BackBufferHeight = 288;
                        SetWindowPos(window, nullptr, 0, 0, 512, 288, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
                        Require(SUCCEEDED(device->Reset(&parameters)), "Input snapshot resize Reset failed");
                        Require(SUCCEEDED(device->CreateDepthStencilSurface(512, 288, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, nullptr)), "Resized depth allocation failed");
                        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                        device->SetRenderState(D3DRS_LIGHTING, FALSE);
                        device->SetRenderState(D3DRS_ZENABLE, TRUE); device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
                        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                        resized = true; ready = false;
                    }
                }
                catch (const std::exception &error) { lastReadError = error.what(); }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        report["frames_presented"] = frames;
        report["rendered_depth_frames"] = rendered;
        report["snapshot_status"] = status.string();
        report["timing_and_bindings_passed"] = ready;
        report["last_status_read_error"] = lastReadError;
        Require(ready, "Owned GPU snapshot fixture did not satisfy completion contract");
        if (!disabled && !unqualified)
        {
            std::ofstream(fixture / L"active_snapshot_status.json") << observed.dump(2);
            report["observation"] = observed;
            phases.push_back({{"present_id_offset", phaseOffset}, {"observation", observed}});
            report["phases"] = phases;
            report["submitted_present_callback_lag"] = frames - phaseOffset - observed.at("present_callbacks").get<unsigned>();
            Require(!observed.at("source_main_view_verified").get<bool>() && !observed.at("game_frame_generation").get<bool>(), "Owned snapshot must not promote main-view or game readiness");
        }
        report["private_snapshot_observation_saved_before_teardown"] = !disabled && !unqualified;
        report["unqualified_context_control"] = unqualified;
        report["cpu_color_uploaded"] = false;
        report["cpu_depth_uploaded"] = false;
        report["pixel_validation_is_separate"] = true;
        report["startup_overlay_wait_milliseconds_per_phase"] = disabled || unqualified ? 0 : 6000;
        report["passed"] = true;
        release();
        std::ofstream(fixture / L"validation.json") << report.dump(2);
        std::cout << "passed=true, frames=" << frames << ", private_snapshots=true, native_fg_ready=false\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        report["error"] = error.what(); release(); fs::create_directories(fixture);
        std::ofstream(fixture / L"validation.json") << report.dump(2);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
