#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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
    bool noDepth = false, disabled = false;
    fs::path dxvkSource;
    for (int index = 1; index < argc; ++index)
    {
        if (std::wstring(argv[index]) == L"--no-depth") noDepth = true;
        else if (std::wstring(argv[index]) == L"--disabled") disabled = true;
        else if (std::wstring(argv[index]) == L"--dxvk" && index + 1 < argc) dxvkSource = fs::absolute(argv[++index]);
        else { std::cerr << "Usage: --no-depth --disabled --dxvk <existing x86 DLL>\n"; return 1; }
    }
    const std::wstring prefix = dxvkSource.empty() ? L"SemanticLive" : L"SemanticLiveVulkan";
    const auto fixture = component / L"results" / (prefix + (disabled ? L"Disabled" : noDepth ? L"NoDepth" : L""));
    const auto host = fixture / L"ReShadeHost", addons = fixture / L"TestAddons", effects = fixture / L"Effects", local = fixture / L"LocalAppData";
    const auto status = fixture / L"semantic_status.json", inputStatus = fixture / L"input_status.json";
    Json report = {{"author", "Anokianx"}, {"mode", "isolated-live-depth-semantic-host"}, {"process_bits", sizeof(void *) * 8},
        {"passed", false}, {"no_depth_control", noDepth}, {"disabled_control", disabled},
        {"dxvk_vulkan", !dxvkSource.empty()}, {"layer_registration_changed", false}, {"game_files_modified", false},
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
        Require(sizeof(void *) == 4, "D3D9 semantic fixture must use x86");
        fs::create_directories(host); fs::create_directories(addons); fs::create_directories(effects); fs::create_directories(local);
        fs::remove(status); fs::remove(inputStatus);
        fs::copy_file(component / L"bin" / L"AsteriaDepthSemanticLiveProbe.addon", addons / L"AsteriaDepthSemanticLiveProbe.addon", fs::copy_options::overwrite_existing);
        fs::copy_file(component.parent_path() / L"InputProbe" / L"bin" / L"AsteriaNativeInputProbe.addon", addons / L"AsteriaNativeInputProbe.addon", fs::copy_options::overwrite_existing);
        fs::copy_file(component / L"Shaders" / L"AsteriaDepthSemanticBridge.fx", effects / L"AsteriaDepthSemanticBridge.fx", fs::copy_options::overwrite_existing);
        const auto config = host / L"ReShade.ini";
        std::ofstream(config) << "[GENERAL]\nPerformanceMode=0\n[DEPTH]\nDepthCopyBeforeClears=0\n";
        WritePrivateProfileStringW(L"ADDON", L"AddonPath", addons.c_str(), config.c_str());
        WritePrivateProfileStringW(L"GENERAL", L"EffectSearchPaths", effects.c_str(), config.c_str());
        WritePrivateProfileStringW(L"GENERAL", L"PresetPath", L"AsteriaDepth.ini", config.c_str());
        std::ofstream(host / L"AsteriaDepth.ini") << "Techniques=AsteriaDepthSemanticProbe@AsteriaDepthSemanticBridge.fx\nTechniqueSorting=AsteriaDepthSemanticProbe@AsteriaDepthSemanticBridge.fx\n";
        SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
        SetEnvironmentVariableW(L"RESHADE_BASE_PATH_OVERRIDE", host.c_str());
        SetEnvironmentVariableW(L"ASTERIA_DEPTH_SEMANTIC_LIVE_PROBE", disabled ? L"0" : L"1");
        SetEnvironmentVariableW(L"ASTERIA_DEPTH_SEMANTIC_LIVE_OUTPUT", status.c_str());
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_INPUT_PROBE", L"1");
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_INPUT_PROBE_OUTPUT", inputStatus.c_str());
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
        klass.hInstance = GetModuleHandleW(nullptr); klass.lpfnWndProc = WindowProc; klass.lpszClassName = L"AsteriaDepthSemanticHost";
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        unsigned frames = 0;
        Json rendered = Json::array();
        Json observed;
        Json activeInput;
        bool ready = false;
        std::string lastReadError;
        while (std::chrono::steady_clock::now() < deadline && !ready)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            device->SetDepthStencilSurface(depth);
            device->Clear(0, nullptr, D3DCLEAR_TARGET | (noDepth ? 0 : D3DCLEAR_ZBUFFER), D3DCOLOR_XRGB(4, 8, 16), 1, 0);
            const bool alternate = ((frames + 1) / 10) % 2 != 0;
            const float leftDepth = alternate ? .4f : .25f, rightDepth = alternate ? .8f : .65f;
            if (SUCCEEDED(device->BeginScene()))
            {
                for (unsigned repeat = 0; repeat < 6; ++repeat)
                {
                    Draw(device, 40, 280, leftDepth, D3DCOLOR_XRGB(230, 30, 10));
                    Draw(device, 360, 600, rightDepth, D3DCOLOR_XRGB(10, 50, 230));
                }
                device->EndScene();
            }
            Require(SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr)), "D3D9 Present failed");
            ++frames;
            rendered.push_back({{"present_id", frames}, {"left_device_depth", leftDepth}, {"right_device_depth", rightDepth}, {"background_device_depth", 1.0f}});
            if (frames >= 100)
            {
                try
                {
                    const auto input = ReadJson(inputStatus);
                    activeInput = input;
                    const bool inputSeen = !input.at("native_fg_ready").get<bool>() && !input.at("devices").empty();
                    if (disabled) ready = inputSeen && !fs::exists(status);
                    else
                    {
                        observed = ReadJson(status);
                        ready = inputSeen && observed.at("sequence_valid").get<bool>() && observed.at("last_borrow_retired_after_callback").get<bool>() &&
                            observed.at("begin_effects").get<unsigned>() >= 60 &&
                            (noDepth ? observed.at("ready_observations").get<unsigned>() == 0 : observed.at("screenshots").size() == 4);
                    }
                }
                catch (const std::exception &error) { lastReadError = error.what(); }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        report["frames_presented"] = frames;
        report["rendered_depth_frames"] = rendered;
        report["semantic_status"] = status.string(); report["input_status"] = inputStatus.string();
        report["timing_and_bindings_passed"] = ready;
        report["last_status_read_error"] = lastReadError;
        Require(ready, "Semantic bridge or independent InputProbe observation did not satisfy fixture contract");
        std::ofstream(fixture / L"active_input_status.json") << activeInput.dump(2);
        report["input_observation_saved_before_teardown"] = true;
        if (!disabled)
        {
            std::ofstream(fixture / L"active_semantic_status.json") << observed.dump(2);
            report["observation"] = observed;
            report["submitted_present_callback_lag"] = frames - observed.at("present_callbacks").get<unsigned>();
            Require(!observed.at("main_view_confirmed").get<bool>() && !observed.at("native_fg_ready").get<bool>(), "Live binding must not promote game readiness");
        }
        unsigned pixelChecks = 0;
        if (!disabled && !noDepth)
        {
            for (const auto &image : observed.at("screenshots"))
            {
                const auto width = image.at("width").get<unsigned>(), height = image.at("height").get<unsigned>();
                Require(width == 640 && height == 360, "Readback dimensions mismatch");
                std::ifstream file(fixture / image.at("file").get<std::string>(), std::ios::binary);
                std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
                file.read(reinterpret_cast<char *>(pixels.data()), pixels.size());
                Require(file.gcount() == static_cast<std::streamsize>(pixels.size()), "Readback image truncated");
                const auto presentID = image.at("present_id").get<unsigned>();
                Require(presentID > 0 && presentID <= rendered.size(), "Captured image present identity not observed by fixture");
                const auto &expected = rendered.at(presentID - 1);
                const auto leftByte = static_cast<unsigned>(std::lround(expected.at("left_device_depth").get<float>() * 255));
                const auto rightByte = static_cast<unsigned>(std::lround(expected.at("right_device_depth").get<float>() * 255));
                for (const auto &point : std::array<std::array<unsigned, 3>, 3>{{{{100, 240, leftByte}}, {{500, 240, rightByte}}, {{320, 340, 255}}}})
                    for (unsigned channel = 0; channel < 3; ++channel)
                    {
                        ++pixelChecks;
                        Require(std::abs(static_cast<int>(pixels[(point[1] * width + point[0]) * 4 + channel]) - static_cast<int>(point[2])) <= 2,
                            "Sampled DEPTH semantic pixels differ from actual GPU depth fixture");
                    }
            }
        }
        report["gpu_depth_pixel_checks"] = pixelChecks;
        report["passed"] = true;
        release();
        std::ofstream(fixture / L"validation.json") << report.dump(2);
        std::cout << "passed=true, frames=" << frames << ", depth_pixel_checks=" << pixelChecks << ", native_fg_ready=false\n";
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
