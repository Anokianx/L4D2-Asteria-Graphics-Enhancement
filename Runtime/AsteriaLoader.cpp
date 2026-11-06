#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <d3d9.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <sstream>
#include <vector>
#include <chrono>
#include <thread>
#include "sdk/json/json.hpp"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;

using AsteriaGetApiVersionFn = unsigned int (WINAPI*)();
using AsteriaInitializeFn = HRESULT (WINAPI*)(const wchar_t*, const wchar_t*, void**);
using AsteriaRenderFrameFn = HRESULT (WINAPI*)(void*, IDirect3DDevice9*);
using AsteriaShutdownFn = void (WINAPI*)(void*);

static double Now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static bool IsRunning(const wchar_t* name, unsigned long* pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                if (pid) *pid = entry.th32ProcessID;
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

static std::wstring EnvironmentValue(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::vector<wchar_t> buffer(size);
    GetEnvironmentVariableW(name, buffer.data(), size);
    return buffer.data();
}

static bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return true;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool result = !GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) || elevation.TokenIsElevated;
    CloseHandle(token);
    return result;
}

static fs::path RegisteredLayerRoot() {
    const wchar_t* program_data = _wgetenv(L"PROGRAMDATA");
    return program_data ? fs::path(program_data) / L"Danyria" / L"Asteria" / L"Host" : fs::path();
}

static bool ConfigureRegisteredLayer(const fs::path& source, bool install) {
    const fs::path destination = RegisteredLayerRoot();
    if (destination.empty()) return false;
    const fs::path manifest = destination / L"AsteriaReShade32.json";
    try {
        if (install) {
            fs::create_directories(destination);
            const std::vector<std::wstring> names = {L"ReShade32.dll", L"ReShade32.json", L"LICENSE.md", L"origin.json"};
            for (const auto& name : names) {
                const fs::path target = name == L"ReShade32.json" ? manifest : destination / name;
                std::ifstream input(source / name, std::ios::binary);
                if (!input) return false;
                const std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
                std::ifstream previous(target, std::ios::binary);
                const std::string existing((std::istreambuf_iterator<char>(previous)), std::istreambuf_iterator<char>());
                input.close();
                previous.close();
                if (data != existing) fs::copy_file(source / name, target, fs::copy_options::overwrite_existing);
            }
        }
        HKEY key = nullptr;
        const LONG opened = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\Vulkan\\ExplicitLayers", 0, nullptr, 0,
                                           KEY_SET_VALUE | KEY_WOW64_32KEY, nullptr, &key, nullptr);
        if (opened != ERROR_SUCCESS) return false;
        LONG result = ERROR_SUCCESS;
        if (install) {
            const DWORD enabled = 0;
            result = RegSetValueExW(key, manifest.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled));
        } else {
            result = RegDeleteValueW(key, manifest.c_str());
            if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
        }
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    } catch (const std::exception&) { return false; }
}

static fs::path PrepareHost(const fs::path& source, const fs::path& state, const fs::path& game) {
    const fs::path host = state / L"ReShadeHost";
    fs::create_directories(host);
    const fs::path config = host / L"ReShade.ini";
    const fs::path game_config = game.parent_path() / L"ReShade.ini";
    fs::copy_file(fs::exists(game_config) ? game_config : source / L"ReShade.ini", config, fs::copy_options::overwrite_existing);
    const fs::path shader_root = source.parent_path().parent_path() / L"Shader";
    const fs::path addon_root = source.parent_path() / L"Addons";
    const std::wstring effects = (shader_root / L"Effects").wstring() + L"," +
        (shader_root / L"Includes").wstring() + L"," + (game.parent_path() / L"reshade-shaders" / L"Shaders").wstring();
    const std::wstring textures = (shader_root / L"Textures").wstring() + L"," +
        (game.parent_path() / L"reshade-shaders" / L"Textures").wstring();
    WritePrivateProfileStringW(L"ADDON", L"AddonPath", addon_root.c_str(), config.c_str());
    WritePrivateProfileStringW(L"GENERAL", L"EffectSearchPaths", effects.c_str(), config.c_str());
    WritePrivateProfileStringW(L"GENERAL", L"TextureSearchPaths", textures.c_str(), config.c_str());
    WritePrivateProfileStringW(L"GENERAL", L"PresetPath", L"Asteria.ini", config.c_str());
    WritePrivateProfileStringW(L"GENERAL", L"PerformanceMode", L"0", config.c_str());
    WritePrivateProfileStringW(L"INSTALL", L"Logging", L"1", config.c_str());
    wchar_t preset_path[4096] = {};
    GetPrivateProfileStringW(L"GENERAL", L"PresetPath", L"ReShadePreset.ini", preset_path, 4096, game_config.c_str());
    fs::path preset = preset_path;
    if (preset.is_relative()) preset = game.parent_path() / preset;
    if (!fs::is_regular_file(preset)) preset = source / L"Asteria.ini";
    std::ifstream input(preset, std::ios::binary);
    std::ofstream output(host / L"Asteria.ini", std::ios::binary | std::ios::trunc);
    std::string line;
    bool sorted = false;
    const std::vector<std::string> own_techniques = {
        "AsteriaDeband@AsteriaDeband.fx", "SMAA@AsteriaSMAA.fx", "ContrastAdaptiveSharpen@AsteriaCAS.fx",
        "AsteriaBloom@AsteriaBloom.fx", "AsteriaColor@AsteriaColor.fx", "AsteriaTonemap@AsteriaTonemap.fx",
        "AsteriaVignette@AsteriaVignette.fx", "AsteriaMarker@Asteria.fx"
    };
    std::string own_sort = "TechniqueSorting=";
    for (const auto &technique : own_techniques) {
        if (own_sort.back() != '=') own_sort += ',';
        own_sort += technique;
    }
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("TechniqueSorting=", 0) == 0) {
            for (const auto& technique : own_techniques)
                if (line.find(technique) == std::string::npos) line += "," + technique;
            sorted = true;
        }
        if (!sorted && !line.empty() && line.front() == '[') {
            output << own_sort << '\n';
            sorted = true;
        }
        output << line << '\n';
    }
    if (!sorted) output << own_sort << '\n';
    return host;
}

static void ApplyReShadeEnvironment(const fs::path& host, const fs::path& layer) {
    SetEnvironmentVariableW(L"RESHADE_BASE_PATH_OVERRIDE", host.c_str());
    const std::wstring existing_path = EnvironmentValue(L"VK_LAYER_PATH");
    const wchar_t* path_key = existing_path.empty() ? L"VK_ADD_LAYER_PATH" : L"VK_LAYER_PATH";
    const std::wstring previous = EnvironmentValue(path_key);
    const std::wstring paths = layer.wstring() + (previous.empty() ? L"" : L";" + previous);
    SetEnvironmentVariableW(path_key, paths.c_str());
    std::wistringstream previous_layers(EnvironmentValue(L"VK_INSTANCE_LAYERS"));
    std::wstring layers = L"VK_LAYER_Asteria_ReShade";
    std::wstring name;
    while (std::getline(previous_layers, name, L';')) {
        if (!name.empty() && name != L"VK_LAYER_reshade" && name != L"VK_LAYER_Asteria_ReShade") layers += L";" + name;
    }
    SetEnvironmentVariableW(L"VK_INSTANCE_LAYERS", layers.c_str());
    SetEnvironmentVariableW(L"DISABLE_VK_LAYER_reshade_1", L"1");
    SetEnvironmentVariableW(L"RESHADE_DISABLE_GRAPHICS_HOOK", L"1");
    SetEnvironmentVariableW(L"SteamAppId", L"550");
    SetEnvironmentVariableW(L"SteamGameId", L"550");
}

static bool HasFgLayerBinary(const fs::path& fg_layer) {
    return fs::is_regular_file(fg_layer / L"AsteriaFrameGenerationLayer.json") &&
           (fs::is_regular_file(fg_layer / L"bin" / L"AsteriaFrameGenerationLayer.dll") ||
            fs::is_regular_file(fg_layer / L"AsteriaFrameGenerationLayer.dll"));
}

static bool ConfigureRegisteredFgLayer(const fs::path& fg_layer, bool install) {
    const fs::path manifest = fg_layer / L"AsteriaFrameGenerationLayer.json";
    if (install && !HasFgLayerBinary(fg_layer))
        return false;
    HKEY key = nullptr;
    const LONG opened = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\Vulkan\\ExplicitLayers", 0, nullptr, 0,
                                       KEY_SET_VALUE | KEY_WOW64_32KEY, nullptr, &key, nullptr);
    if (opened != ERROR_SUCCESS) return false;
    LONG result = ERROR_SUCCESS;
    if (install) {
        const DWORD enabled = 0;
        result = RegSetValueExW(key, manifest.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled));
    } else {
        result = RegDeleteValueW(key, manifest.c_str());
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

static void ApplyFrameGenerationEnvironment(const fs::path& state, const fs::path& fg_layer, const fs::path& control) {
    bool enabled = true;
    bool true_depth = true;
    bool motion_reconstruction = true;
    bool optical_flow = true;
    try {
        std::ifstream input(control, std::ios::binary);
        if (input) {
            const auto cfg = nlohmann::json::parse(input);
            if (cfg.contains("frame_generation") && cfg["frame_generation"].is_object()) {
                const auto& fg = cfg["frame_generation"];
                enabled = fg.value("enabled", true);
                true_depth = fg.value("true_depth", true);
                motion_reconstruction = fg.value("motion_reconstruction", true);
                optical_flow = fg.value("optical_flow", true);
            }
        }
    } catch (...) {}
    if (!enabled || !HasFgLayerBinary(fg_layer)) {
        SetEnvironmentVariableW(L"ASTERIA_FG_ENABLE", L"0");
        return;
    }
    if (IsElevated()) {
        ConfigureRegisteredFgLayer(fg_layer, true);
    }
    const std::wstring existing_path = EnvironmentValue(L"VK_LAYER_PATH");
    const wchar_t* path_key = existing_path.empty() ? L"VK_ADD_LAYER_PATH" : L"VK_LAYER_PATH";
    const std::wstring previous = EnvironmentValue(path_key);
    const std::wstring paths = fg_layer.wstring() + (previous.empty() ? L"" : L";" + previous);
    SetEnvironmentVariableW(path_key, paths.c_str());
    std::wistringstream previous_layers(EnvironmentValue(L"VK_INSTANCE_LAYERS"));
    std::wstring layers = L"VK_LAYER_Asteria_FrameGeneration;VK_LAYER_Asteria_ReShade";
    std::wstring name;
    while (std::getline(previous_layers, name, L';')) {
        if (!name.empty() && name != L"VK_LAYER_reshade" &&
            name != L"VK_LAYER_Asteria_ReShade" && name != L"VK_LAYER_Asteria_FrameGeneration") {
            layers += L";" + name;
        }
    }
    SetEnvironmentVariableW(L"VK_INSTANCE_LAYERS", layers.c_str());
    SetEnvironmentVariableW(L"ASTERIA_FG_ENABLE", L"1");
    SetEnvironmentVariableW(L"ASTERIA_FG_ESTIMATED_INTERNAL", L"1");
    SetEnvironmentVariableW(L"ASTERIA_FG_LAYER_ORDER", L"fg-first");
    SetEnvironmentVariableW(L"ASTERIA_FG_NATIVE_PROXY_IMAGES", L"1");
    SetEnvironmentVariableW(L"ASTERIA_FG_RETURN_NATIVE_SWAPCHAIN", L"0");
    SetEnvironmentVariableW(L"ASTERIA_FG_NATIVE_PASSTHROUGH", L"0");
    SetEnvironmentVariableW(L"ASTERIA_FG_NATIVE_ACQUIRE", L"0");
    SetEnvironmentVariableW(L"ASTERIA_FG_NATIVE_BACKBUFFER", L"0");
    SetEnvironmentVariableW(L"ASTERIA_FG_TRUE_DEPTH", true_depth ? L"1" : L"0");
    SetEnvironmentVariableW(L"ASTERIA_FG_MOTION_RECONSTRUCTION", motion_reconstruction ? L"1" : L"0");
    SetEnvironmentVariableW(L"ASTERIA_FG_OPTICAL_FLOW", optical_flow ? L"1" : L"0");
    SetEnvironmentVariableW(L"ASTERIA_FG_PROBE_OUTPUT", (state / L"asteria_fg_layer_status.json").c_str());
    SetEnvironmentVariableW(L"ASTERIA_FG_MAPPED_STATUS_PATH", (state / L"asteria_fg_layer_status.json").c_str());
    const fs::path profiles = fg_layer.parent_path() / L"Research" / L"NativeShaders" / L"native-uniform-profiles.json";
    if (fs::is_regular_file(profiles)) {
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_UNIFORM_PROFILES", profiles.c_str());
    }
}

static void WriteStatus(const fs::path& path, bool game, bool stub, unsigned int api, const char* host, const std::wstring& detail) {
    std::ostringstream out;
    out << std::setprecision(17) << "{\"loader\":\"Asteria\",\"game_running\":" << (game ? "true" : "false")
        << ",\"stub_loaded\":" << (stub ? "true" : "false")
        << ",\"render_host\":\"" << host << "\",\"api\":" << api
        << ",\"heartbeat\":" << Now() << ",\"detail\":\"";
    for (wchar_t c : detail) {
        if (c == L'\\' || c == L'"') out << '\\';
        out << static_cast<char>(c < 128 ? c : '?');
    }
    out << "\"}";
    fs::path temp = path;
    temp += L".tmp";
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file << out.str();
    file.close();
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        DeleteFileW(temp.c_str());
}

static LRESULT CALLBACK SelfTestWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(window, message, wparam, lparam);
}

static bool RunSelfTest(AsteriaRenderFrameFn render_frame, void* handle) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"AsteriaSelfTestWindow";
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = SelfTestWindowProc;
    window_class.lpszClassName = class_name;
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, class_name, L"Asteria", WS_OVERLAPPEDWINDOW, 0, 0, 320, 180, nullptr, nullptr, instance, nullptr);
    if (!window) return false;
    IDirect3D9* direct3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!direct3d) {
        DestroyWindow(window);
        return false;
    }
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    parameters.BackBufferWidth = 320;
    parameters.BackBufferHeight = 180;
    IDirect3DDevice9* device = nullptr;
    HRESULT created = direct3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    bool rendered = false;
    if (SUCCEEDED(created) && device) {
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 3, 4, 6), 1.0f, 0);
        rendered = render_frame(handle, device) == S_OK;
        if (rendered) {
            IDirect3DSurface9* back_buffer = nullptr;
            IDirect3DSurface9* readback = nullptr;
            rendered = false;
            if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer)) &&
                SUCCEEDED(device->CreateOffscreenPlainSurface(320, 180, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr)) &&
                SUCCEEDED(device->GetRenderTargetData(back_buffer, readback))) {
                D3DLOCKED_RECT locked{};
                if (SUCCEEDED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
                    const auto pixels = static_cast<const BYTE*>(locked.pBits);
                    const DWORD edge = *reinterpret_cast<const DWORD*>(pixels + 12 * locked.Pitch + 16 * 4);
                    const DWORD center = *reinterpret_cast<const DWORD*>(pixels + 90 * locked.Pitch + 160 * 4);
                    rendered = (edge & 0xFF) > 128 && (center & 0xFFFFFF) == 0x030406;
                    readback->UnlockRect();
                }
            }
            if (readback) readback->Release();
            if (back_buffer) back_buffer->Release();
        }
        device->Present(nullptr, nullptr, nullptr, nullptr);
        device->Release();
    }
    direct3d->Release();
    DestroyWindow(window);
    UnregisterClassW(class_name, instance);
    return rendered;
}

static bool RunHostTest(const fs::path& status, IDirect3D9* (WINAPI* create_direct3d)(UINT) = &Direct3DCreate9, bool show = false) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"AsteriaHostTestWindow";
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = SelfTestWindowProc;
    window_class.lpszClassName = class_name;
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, class_name, L"Asteria Host Test", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr, instance, nullptr);
    if (!window) return false;
    if (show) ShowWindow(window, SW_SHOWNOACTIVATE);
    IDirect3D9* direct3d = create_direct3d(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    parameters.BackBufferWidth = 640;
    parameters.BackBufferHeight = 360;
    IDirect3DDevice9* device = nullptr;
    bool ready = false;
    const HRESULT created = direct3d ? direct3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &parameters, &device) : E_FAIL;
    if (SUCCEEDED(created)) {
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline && !ready) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 3, 4, 6), 1.0f, 0);
            if (SUCCEEDED(device->BeginScene())) {
                struct ProbeVertex { float x, y, z, rhw; DWORD color; };
                const ProbeVertex triangle[] = {
                    {80, 260, 0, 1, D3DCOLOR_XRGB(230, 230, 230)},
                    {320, 60, 0, 1, D3DCOLOR_XRGB(220, 80, 120)},
                    {550, 260, 0, 1, D3DCOLOR_XRGB(50, 180, 210)},
                };
                device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(ProbeVertex));
                device->EndScene();
            }
            const HRESULT presented = device->Present(nullptr, nullptr, nullptr, nullptr);
            if (FAILED(presented)) {
                std::ofstream(status.parent_path() / L"asteria_probe_error.txt") << "Present failed: " << std::hex << presented;
                break;
            }
            std::ifstream input(status, std::ios::binary);
            const std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            input.close();
            try {
                const auto report = nlohmann::json::parse(data);
                ready = report.value("pid", 0u) == GetCurrentProcessId() && report.value("mode", std::string()) == "host-test";
                const auto rendered = report.value("effects_rendered", nlohmann::json::object());
                for (const char *effect : {"smaa", "cas", "bloom", "color", "tonemap", "vignette", "deband", "marker"})
                    ready = ready && rendered.value(effect, false);
                ready = ready && std::chrono::steady_clock::now() - started >= std::chrono::seconds(2);
            } catch (...) { ready = false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    } else {
        std::ofstream(status.parent_path() / L"asteria_probe_error.txt") << "D3D9 CreateDevice failed: " << std::hex << created;
    }
    if (device) device->Release();
    if (direct3d) direct3d->Release();
    DestroyWindow(window);
    UnregisterClassW(class_name, instance);
    return ready;
}

int wmain(int argc, wchar_t** argv) {
    fs::path runtime = fs::absolute(fs::path(argv[0])).parent_path();
    fs::path host = runtime / L"ReShadeHost";
    if (!fs::exists(host)) host = runtime.parent_path() / L"ReShadeHost";
    const wchar_t* local_app_data = _wgetenv(L"LOCALAPPDATA");
    fs::path state = local_app_data ? fs::path(local_app_data) / L"Danyria" / L"AsteriaRuntime" : runtime;
    fs::create_directories(state);
    fs::path control = state / L"asteria_runtime_control.json";
    fs::path stub_status = state / L"asteria_self_test_status.json";
    fs::path status = state / L"asteria_loader_status.json";
    fs::path game;
    bool launch = false;
    bool watch = false;
    bool self_test = false;
    bool host_test = false;
    bool install_layer = false;
    bool uninstall_layer = false;
    fs::path d3d9_library;
    bool show_test = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--game" && i + 1 < argc) game = argv[++i];
        if (arg == L"--launch") launch = true;
        if (arg == L"--watch") watch = true;
        if (arg == L"--self-test") self_test = true;
        if (arg == L"--host-test") host_test = true;
        if (arg == L"--install-layer") install_layer = true;
        if (arg == L"--uninstall-layer") uninstall_layer = true;
        if (arg == L"--d3d9-library" && i + 1 < argc) d3d9_library = fs::absolute(fs::path(argv[++i]));
        if (arg == L"--show-test") show_test = true;
    }

    if (install_layer || uninstall_layer) {
        const bool installed = ConfigureRegisteredLayer(host.parent_path() / L"Host", !uninstall_layer);
        WriteStatus(state / L"asteria_layer_setup_status.json", false, false, 0, "layer-setup",
                    installed ? (uninstall_layer ? L"Asteria explicit Vulkan layer unregistered" : L"Asteria explicit Vulkan layer registered") : L"Vulkan layer setup failed; administrator permission and host files are required");
        return installed ? 0 : 12;
    }

    if (host_test) {
        try {
            fs::remove(state / L"asteria_probe_error.txt");
            SetEnvironmentVariableW(L"DXVK_LOG_PATH", (state / L"HostTest").c_str());
            SetEnvironmentVariableW(L"DXVK_STATE_CACHE_PATH", (state / L"HostTest").c_str());
            const fs::path active_host = PrepareHost(host, state / L"HostTest", fs::absolute(fs::path(argv[0])));
            std::ofstream(state / L"asteria_host_test_control.json", std::ios::trunc) <<
                "{\"enabled\":true,\"marker\":{\"enabled\":true,\"strength\":0.7},\"smaa\":{\"enabled\":true,\"strength\":0.42,\"quality\":\"medium\"},\"cas\":{\"enabled\":true,\"strength\":0.35},"
                "\"bloom\":{\"enabled\":true,\"strength\":0.35,\"threshold\":0.65,\"radius\":2.0},\"color\":{\"enabled\":true,\"exposure\":0.1,\"contrast\":1.05,\"saturation\":1.1,\"temperature\":0.1,\"vibrance\":0.1},"
                "\"tonemap\":{\"enabled\":true,\"strength\":0.5,\"mode\":\"filmic\"},\"vignette\":{\"enabled\":true,\"strength\":0.2,\"radius\":0.75,\"softness\":0.35},\"deband\":{\"enabled\":true,\"strength\":0.3,\"radius\":8,\"quality\":\"medium\"}}";
            HMODULE module = nullptr;
            bool rendered = false;
            if (d3d9_library.empty()) {
                SetEnvironmentVariableW(L"RESHADE_BASE_PATH_OVERRIDE", active_host.c_str());
                module = LoadLibraryW((host.parent_path() / L"Host" / L"ReShade32.dll").c_str());
                if (!module) {
                    const DWORD error = GetLastError();
                    std::ofstream(state / L"asteria_probe_error.txt") << "ReShade LoadLibrary failed: " << error;
                }
                rendered = module && RunHostTest(state / L"asteria_host_test_status.json", &Direct3DCreate9, show_test);
            } else {
                const fs::path layer = host.parent_path() / L"Host";
                if (IsElevated() && !ConfigureRegisteredLayer(layer, true)) return 12;
                ApplyReShadeEnvironment(active_host, layer);
                module = LoadLibraryExW(d3d9_library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                const auto create_direct3d = module ? reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(module, "Direct3DCreate9")) : nullptr;
                rendered = create_direct3d && RunHostTest(state / L"asteria_host_test_status.json", create_direct3d, show_test);
            }
            if (module) FreeLibrary(module);
            WriteStatus(state / L"asteria_host_test_loader_status.json", false, false, 0,
                        d3d9_library.empty() ? "d3d9-host-test" : "dxvk-host-test", rendered ? L"All eight Asteria techniques rendered" : L"host effect test failed; inspect HostTest/ReShadeHost/ReShade.log");
            return rendered ? 0 : 10;
        } catch (const std::exception&) { return 11; }
    }

    if (self_test) {
        fs::path test_state = state / L"SelfTest";
        fs::create_directories(test_state);
        control = test_state / L"control.json";
        stub_status = test_state / L"status.json";
        std::ofstream(control, std::ios::trunc) << "{\"enabled\":true}";
    }

    fs::path stub_path = runtime / L"AsteriaRuntimeStub.dll";
    HMODULE stub = LoadLibraryW(stub_path.c_str());
    auto get_api = stub ? reinterpret_cast<AsteriaGetApiVersionFn>(GetProcAddress(stub, "AsteriaGetApiVersion")) : nullptr;
    auto initialize = stub ? reinterpret_cast<AsteriaInitializeFn>(GetProcAddress(stub, "AsteriaInitialize")) : nullptr;
    auto render_frame = stub ? reinterpret_cast<AsteriaRenderFrameFn>(GetProcAddress(stub, "AsteriaRenderFrame")) : nullptr;
    auto shutdown = stub ? reinterpret_cast<AsteriaShutdownFn>(GetProcAddress(stub, "AsteriaShutdown")) : nullptr;
    unsigned int api = get_api ? get_api() : 0;
    void* handle = nullptr;
    bool stub_ready = initialize && SUCCEEDED(initialize(control.c_str(), stub_status.c_str(), &handle));

    if (self_test) {
        bool rendered = stub_ready && render_frame && RunSelfTest(render_frame, handle);
        WriteStatus(status, false, stub_ready, api, rendered ? "d3d9-self-test" : "d3d9-self-test-failed", rendered ? L"D3D9 callback verified" : L"D3D9 callback failed");
        if (shutdown && handle) shutdown(handle);
        if (stub) FreeLibrary(stub);
        return rendered ? 0 : 3;
    }

    if (launch && (game.empty() || !fs::is_regular_file(game))) {
        WriteStatus(status, false, stub_ready, api, "unbound", L"game executable not found");
        if (shutdown && handle) shutdown(handle);
        if (stub) FreeLibrary(stub);
        return 2;
    }
    if (launch) {
        if (!fs::is_regular_file(host / L"ReShade.ini")) {
            WriteStatus(status, false, stub_ready, api, "unbound", L"Asteria ReShadeHost configuration not found");
            if (shutdown && handle) shutdown(handle);
            if (stub) FreeLibrary(stub);
            return 4;
        }
        if (IsRunning(L"left4dead2.exe", nullptr)) {
            WriteStatus(status, true, stub_ready, api, "unbound", L"game is already running; restart through Asteria to apply host environment");
            if (shutdown && handle) shutdown(handle);
            if (stub) FreeLibrary(stub);
            return 5;
        }
        const fs::path layer = host.parent_path() / L"Host";
        if (!fs::is_regular_file(layer / L"ReShade32.dll") || !fs::is_regular_file(layer / L"ReShade32.json")) {
            WriteStatus(status, false, stub_ready, api, "unbound", L"ReShade Vulkan host missing; run Prepare_ReShadeHost.py");
            if (shutdown && handle) shutdown(handle);
            if (stub) FreeLibrary(stub);
            return 7;
        }
        if (IsElevated() && !ConfigureRegisteredLayer(layer, true)) {
            WriteStatus(status, false, stub_ready, api, "unbound", L"Asteria explicit Vulkan layer registration failed; use normal privileges or --install-layer");
            if (shutdown && handle) shutdown(handle);
            if (stub) FreeLibrary(stub);
            return 8;
        }
        try {
            const fs::path active_host = PrepareHost(host, state, game);
            ApplyReShadeEnvironment(active_host, layer);
            ApplyFrameGenerationEnvironment(state, host.parent_path() / L"FrameGeneration" / L"Layer", control);
        } catch (const std::exception&) {
            WriteStatus(status, false, stub_ready, api, "unbound", L"unable to prepare Asteria host configuration");
            if (shutdown && handle) shutdown(handle);
            if (stub) FreeLibrary(stub);
            return 9;
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        std::wstring command = L"\"" + game.wstring() + L"\" -vulkan -novid -console -multirun";
        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(0);
        if (!CreateProcessW(game.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, game.parent_path().c_str(), &startup, &process)) {
            WriteStatus(status, false, stub_ready, api, "unbound", L"game launch failed: " + std::to_wstring(GetLastError()));
            if (shutdown && handle) shutdown(handle);
            if (stub) FreeLibrary(stub);
            return 6;
        }
        if (process.hProcess) CloseHandle(process.hProcess);
        if (process.hThread) CloseHandle(process.hThread);
    }

    bool saw_game = false;
    for (;;) {
        unsigned long pid = 0;
        bool game_running = IsRunning(L"left4dead2.exe", &pid);
        if (game_running) saw_game = true;
        WriteStatus(status, game_running, stub_ready, api, "unbound", game_running ? L"runtime host callback pending" : L"waiting for left4dead2.exe");
        if (!watch && (!launch || saw_game) && !game_running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (shutdown && handle) shutdown(handle);
    if (stub) FreeLibrary(stub);
    if (launch && IsElevated()) {
        ConfigureRegisteredFgLayer(host.parent_path() / L"FrameGeneration" / L"Layer", false);
    }
    return 0;
}
