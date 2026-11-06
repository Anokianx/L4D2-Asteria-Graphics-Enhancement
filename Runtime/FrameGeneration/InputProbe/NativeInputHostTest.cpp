#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <cstring>

namespace fs = std::filesystem;
using Json = nlohmann::json;

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

static std::string Hex(uint64_t value)
{
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << value;
    return text.str();
}

static Json ReadJson(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return Json::parse(input);
}

static IDirect3DPixelShader9* CreateFixtureShader(IDirect3DDevice9* device, HMODULE compiler, const char* source, const fs::path& output)
{
    const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile"));
    if (!compile) throw std::runtime_error("System shader compiler entry point is unavailable.");
    ID3DBlob* bytecode = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT compiled = compile(source, std::strlen(source), "AsteriaPipelineFixture", nullptr, nullptr,
                                     "main", "ps_2_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors);
    std::string message;
    if (errors) { message.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()); errors->Release(); }
    if (FAILED(compiled) || !bytecode) throw std::runtime_error("Fixture shader compile failed: " + message);
    std::ofstream saved(output, std::ios::binary | std::ios::trunc);
    saved.write(static_cast<const char*>(bytecode->GetBufferPointer()), bytecode->GetBufferSize());
    if (!saved) { bytecode->Release(); throw std::runtime_error("Fixture bytecode write failed."); }
    IDirect3DPixelShader9* shader = nullptr;
    const HRESULT created = device->CreatePixelShader(static_cast<const DWORD*>(bytecode->GetBufferPointer()), &shader);
    bytecode->Release();
    if (FAILED(created)) throw std::runtime_error("Fixture pixel shader creation failed.");
    return shader;
}

struct Vertex { float x, y, z, rhw; DWORD color; float u, v; };

static void Draw(IDirect3DDevice9 *device)
{
    const Vertex vertices[] = {
        {80, 260, 0.25f, 1, D3DCOLOR_XRGB(240, 120, 64), 0, 1},
        {320, 60, 0.25f, 1, D3DCOLOR_XRGB(240, 210, 120), 0.5f, 0},
        {550, 260, 0.25f, 1, D3DCOLOR_XRGB(80, 180, 220), 1, 1},
    };
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(Vertex));
}

int wmain(int argc, wchar_t **argv)
{
    const fs::path probe = fs::absolute(fs::path(argv[0])).parent_path().parent_path();
    const fs::path runtime = probe.parent_path().parent_path();
    bool disabled = false, pipeline_probe = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring option = argv[index];
        if (option == L"--disabled") disabled = true;
        else if (option == L"--pipeline-probe") pipeline_probe = true;
        else { std::cerr << "Usage: --disabled --pipeline-probe\n"; return 1; }
    }
    const fs::path fixture = probe / L"results" / (pipeline_probe ? (disabled ? L"HostPipelineDisabled" : L"HostPipeline") : (disabled ? L"HostTestDisabled" : L"HostTest"));
    const fs::path host = fixture / L"ReShadeHost";
    const fs::path addons = fixture / L"TestAddons";
    const fs::path local = fixture / L"LocalAppData";
    const fs::path status = fixture / L"native_input_status.json";
    const fs::path summary = fixture / L"validation.json";
    Json validation = {{"author", "Anokianx"}, {"mode", "isolated-native-input-host-test"},
                       {"process_bits", sizeof(void *) * 8}, {"native_fg_ready", false}, {"passed", false}};
    HMODULE reshade = nullptr;
    HWND window = nullptr;
    IDirect3D9 *direct3d = nullptr;
    IDirect3DDevice9 *device = nullptr;
    IDirect3DTexture9 *rg = nullptr, *sampled = nullptr;
    IDirect3DSurface9 *rg_surface = nullptr, *depth = nullptr, *backbuffer = nullptr;
    HMODULE compiler = nullptr;
    IDirect3DPixelShader9* first_shader = nullptr;
    IDirect3DPixelShader9* second_shader = nullptr;
    auto release = [&] {
        if (device) { device->SetPixelShader(nullptr); device->SetTexture(0, nullptr); device->SetRenderTarget(0, backbuffer); device->SetDepthStencilSurface(nullptr); }
        if (first_shader) first_shader->Release();
        if (second_shader) second_shader->Release();
        if (backbuffer) backbuffer->Release();
        if (depth) depth->Release();
        if (rg_surface) rg_surface->Release();
        if (sampled) sampled->Release();
        if (rg) rg->Release();
        if (device) device->Release();
        if (direct3d) direct3d->Release();
        if (window) DestroyWindow(window);
        if (reshade) FreeLibrary(reshade);
        if (compiler) FreeLibrary(compiler);
    };
    try
    {
        fs::create_directories(host); fs::create_directories(addons); fs::create_directories(local);
        fs::remove(status);
        fs::copy_file(probe / L"bin" / L"AsteriaNativeInputProbe.addon", addons / L"AsteriaNativeInputProbe.addon", fs::copy_options::overwrite_existing);
        fs::copy_file(runtime / L"Addons" / L"Asteria.addon", addons / L"Asteria.addon", fs::copy_options::overwrite_existing);
        const fs::path shaders = runtime.parent_path() / L"Shader";
        const fs::path config = host / L"ReShade.ini";
        fs::copy_file(runtime / L"ReShadeHost" / L"ReShade.ini", config, fs::copy_options::overwrite_existing);
        fs::copy_file(runtime / L"ReShadeHost" / L"Asteria.ini", host / L"Asteria.ini", fs::copy_options::overwrite_existing);
        const std::wstring effects = (shaders / L"Effects").wstring() + L"," + (shaders / L"Includes").wstring();
        WritePrivateProfileStringW(L"ADDON", L"AddonPath", addons.c_str(), config.c_str());
        WritePrivateProfileStringW(L"GENERAL", L"EffectSearchPaths", effects.c_str(), config.c_str());
        WritePrivateProfileStringW(L"GENERAL", L"TextureSearchPaths", (shaders / L"Textures").c_str(), config.c_str());
        WritePrivateProfileStringW(L"GENERAL", L"PresetPath", L"Asteria.ini", config.c_str());
        SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
        SetEnvironmentVariableW(L"RESHADE_BASE_PATH_OVERRIDE", host.c_str());
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_INPUT_PROBE", disabled ? L"0" : L"1");
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_PIPELINE_PROBE", pipeline_probe ? L"1" : L"0");
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_ENGINE_CAMERA", L"0");
        SetEnvironmentVariableW(L"ASTERIA_NATIVE_INPUT_PROBE_OUTPUT", status.c_str());
        const fs::path asteria_state = local / L"Danyria" / L"AsteriaRuntime";
        fs::create_directories(asteria_state);
        std::ofstream(asteria_state / L"asteria_host_test_control.json") <<
            R"({"enabled":true,"marker":{"enabled":true,"strength":0.7},"smaa":{"enabled":true,"strength":0.42,"quality":"medium"},"cas":{"enabled":true,"strength":0.35},"bloom":{"enabled":true,"strength":0.35,"threshold":0.65,"radius":2},"color":{"enabled":true,"exposure":0.1,"contrast":1.05,"saturation":1.1,"temperature":0.1,"vibrance":0.1},"tonemap":{"enabled":true,"strength":0.5,"mode":"filmic"},"vignette":{"enabled":true,"strength":0.2,"radius":0.75,"softness":0.35},"deband":{"enabled":true,"strength":0.3,"radius":8,"quality":"medium"}})";
        reshade = LoadLibraryW((runtime / L"Host" / L"ReShade32.dll").c_str());
        if (!reshade) throw std::runtime_error("ReShade host LoadLibrary failed.");
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr); klass.lpfnWndProc = WindowProc;
        klass.lpszClassName = L"AsteriaNativeInputHostTest";
        RegisterClassW(&klass);
        window = CreateWindowExW(0, klass.lpszClassName, L"Asteria Native Input Host Test", WS_OVERLAPPEDWINDOW,
                                 0, 0, 640, 360, nullptr, nullptr, klass.hInstance, nullptr);
        direct3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (!window || !direct3d) throw std::runtime_error("D3D9 host creation failed.");
        D3DPRESENT_PARAMETERS parameters{};
        parameters.Windowed = TRUE; parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters.hDeviceWindow = window; parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
        parameters.BackBufferWidth = 640; parameters.BackBufferHeight = 360;
        parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        if (FAILED(direct3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &parameters, &device)))
            throw std::runtime_error("D3D9 CreateDevice failed.");
        if (pipeline_probe)
        {
            wchar_t system_path[MAX_PATH]{};
            const UINT length = GetSystemDirectoryW(system_path, MAX_PATH);
            if (!length || length >= MAX_PATH) throw std::runtime_error("System directory unavailable.");
            compiler = LoadLibraryW((fs::path(system_path) / L"d3dcompiler_47.dll").c_str());
            if (!compiler) throw std::runtime_error("System shader compiler unavailable.");
            first_shader = CreateFixtureShader(device, compiler, "sampler2D s : register(s0); float4 main(float2 uv : TEXCOORD0) : COLOR0 { return tex2D(s, uv); }", fixture / L"fixture_first_ps.bin");
            second_shader = CreateFixtureShader(device, compiler, "sampler2D s : register(s0); float4 main(float2 uv : TEXCOORD0) : COLOR0 { return tex2D(s, uv) * float4(0.7, 0.8, 0.9, 1); }", fixture / L"fixture_second_ps.bin");
        }
        if (FAILED(device->CreateTexture(640, 360, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G16R16F, D3DPOOL_DEFAULT, &rg, nullptr)) ||
            FAILED(device->CreateTexture(128, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &sampled, nullptr)) ||
            FAILED(device->CreateDepthStencilSurface(640, 360, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &depth, nullptr)) ||
            FAILED(rg->GetSurfaceLevel(0, &rg_surface)) || FAILED(device->GetRenderTarget(0, &backbuffer)))
            throw std::runtime_error("D3D9 fixture resource creation failed.");
        D3DLOCKED_RECT pixels{};
        if (SUCCEEDED(sampled->LockRect(0, &pixels, nullptr, 0)))
        {
            for (unsigned y = 0; y < 128; ++y)
                for (unsigned x = 0; x < 128; ++x)
                    reinterpret_cast<DWORD *>(static_cast<char *>(pixels.pBits) + y * pixels.Pitch)[x] = D3DCOLOR_XRGB(100 + x, 70 + y, 140);
            sampled->UnlockRect(0);
        }
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::seconds(disabled ? 3 : 12);
        uint64_t frames = 0;
        Json observed;
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline && !ready)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            device->SetTexture(0, sampled);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetRenderTarget(0, rg_surface); device->SetDepthStencilSurface(depth);
            if (pipeline_probe) device->SetPixelShader(first_shader);
            device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1, 0);
            if (SUCCEEDED(device->BeginScene())) { Draw(device); device->EndScene(); }
            device->SetRenderTarget(0, backbuffer);
            if (pipeline_probe) device->SetPixelShader(second_shader);
            device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(4, 8, 16), 1, 0);
            if (SUCCEEDED(device->BeginScene())) { Draw(device); device->EndScene(); }
            if (FAILED(device->Present(nullptr, nullptr, nullptr, nullptr))) throw std::runtime_error("Fixture Present failed.");
            ++frames;
            if (!disabled && std::chrono::steady_clock::now() - started >= std::chrono::seconds(3))
            {
                try
                {
                    observed = ReadJson(status);
                    bool target = false, depth_seen = false, texture = false, rg_seen = false, effects = false;
                    bool exact_draws = true;
                    unsigned candidates = 0;
                    for (const auto &state : observed.at("devices"))
                    {
                        effects = effects || state.value("effect_scopes", 0u) > 0;
                        exact_draws = exact_draws && state.at("total_draws").get<uint64_t>() == state.at("frame_id_observed").get<uint64_t>() * 2;
                        exact_draws = exact_draws && state.at("limit_hits").get<uint64_t>() == 0 &&
                                      state.at("tracking").at("binding_observation_without_tracking_losses").get<bool>() &&
                                      state.at("engine_camera").value("status", std::string()) == "unavailable";
                        for (const auto &resource : state.at("resources"))
                        {
                            target = target || resource.at("roles").at("render_target").get<bool>();
                            depth_seen = depth_seen || resource.at("roles").at("depth_view").get<bool>();
                            texture = texture || resource.at("roles").at("sampled_texture").get<bool>();
                            rg_seen = rg_seen || (resource.at("motion_candidate").at("format_only").get<bool>() &&
                                                 !resource.at("motion_candidate").at("verified").get<bool>());
                            if (resource.at("motion_candidate").at("format_only").get<bool>())
                            {
                                ++candidates;
                                exact_draws = exact_draws && resource.at("native_handle_hex") == Hex(reinterpret_cast<uintptr_t>(rg));
                            }
                        }
                    }
                    const Json effects_status = ReadJson(asteria_state / L"asteria_host_test_status.json");
                    bool all_effects = true;
                    for (const char *effect : {"smaa", "cas", "bloom", "color", "tonemap", "vignette", "deband", "marker"})
                        all_effects = all_effects && effects_status.at("effects_rendered").value(effect, false);
                    ready = target && depth_seen && texture && rg_seen && effects && exact_draws && candidates == 1 &&
                            all_effects && !observed.at("native_fg_ready").get<bool>();
                }
                catch (...) {}
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        validation["frames_presented"] = frames;
        validation["probe_disabled"] = disabled;
        validation["pipeline_probe_enabled"] = pipeline_probe;
        validation["known_fixture_handles"] = {{"rg_texture", Hex(reinterpret_cast<uintptr_t>(rg))},
                                               {"rg_surface", Hex(reinterpret_cast<uintptr_t>(rg_surface))},
                                               {"sampled_texture", Hex(reinterpret_cast<uintptr_t>(sampled))},
                                               {"depth_surface", Hex(reinterpret_cast<uintptr_t>(depth))}};
        if (pipeline_probe)
        {
            validation["known_fixture_handles"]["first_pixel_shader"] = Hex(reinterpret_cast<uintptr_t>(first_shader));
            validation["known_fixture_handles"]["second_pixel_shader"] = Hex(reinterpret_cast<uintptr_t>(second_shader));
            validation["known_fixture_handles"]["backbuffer_surface"] = Hex(reinterpret_cast<uintptr_t>(backbuffer));
        }
        validation["probe_status"] = status.string();
        validation["passed"] = disabled ? !fs::exists(status) : ready;
        if (ready) { std::ofstream(fixture / L"active_status.json") << observed.dump(2); validation["active_observation"] = observed; }
        release();
        std::ofstream(summary, std::ios::binary | std::ios::trunc) << validation.dump(2);
        std::cout << "passed=" << validation["passed"] << ", frames=" << frames << ", native_fg_ready=false\n";
        return validation["passed"].get<bool>() ? 0 : 2;
    }
    catch (const std::exception &error)
    {
        validation["error"] = error.what();
        release();
        fs::create_directories(fixture);
        std::ofstream(summary) << validation.dump(2);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
