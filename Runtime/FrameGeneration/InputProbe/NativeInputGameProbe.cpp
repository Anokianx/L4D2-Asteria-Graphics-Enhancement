#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Json = nlohmann::json;

static std::wstring Environment(const wchar_t *key)
{
    DWORD size = GetEnvironmentVariableW(key, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    DWORD length = GetEnvironmentVariableW(key, value.data(), size);
    if (!length || length >= size) return {};
    value.resize(length);
    return value;
}

static bool Running(const wchar_t *name)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Process inventory failed.");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry))
        do { if (!_wcsicmp(entry.szExeFile, name)) { found = true; break; } } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return found;
}

static bool Elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("Token query failed.");
    TOKEN_ELEVATION elevation{}; DWORD size = 0;
    bool queried = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    if (!queried) throw std::runtime_error("Token elevation query failed.");
    return elevation.TokenIsElevated != 0;
}

static bool RegisteredLayer()
{
    for (HKEY hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(hive, L"SOFTWARE\\Khronos\\Vulkan\\ExplicitLayers", 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS) continue;
        for (DWORD index = 0;; ++index)
        {
            wchar_t name[32768]{}; DWORD length = 32768, type = 0, enabled = 1, bytes = sizeof(enabled);
            LONG result = RegEnumValueW(key, index, name, &length, nullptr, &type, reinterpret_cast<BYTE *>(&enabled), &bytes);
            if (result == ERROR_NO_MORE_ITEMS) break;
            if (result != ERROR_SUCCESS || type != REG_DWORD || enabled != 0) continue;
            try
            {
                std::ifstream file(fs::path(name), std::ios::binary);
                Json manifest = Json::parse(file);
                if (manifest.at("layer").value("name", std::string()) != "VK_LAYER_Asteria_ReShade") continue;
                fs::path library = fs::u8path(manifest.at("layer").at("library_path").get<std::string>());
                if (library.is_relative()) library = fs::path(name).parent_path() / library;
                if (fs::is_regular_file(library)) { RegCloseKey(key); return true; }
            }
            catch (...) {}
        }
        RegCloseKey(key);
    }
    return false;
}

static bool X86(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    char mz[2]{}; uint32_t offset = 0, signature = 0; uint16_t machine = 0;
    file.read(mz, 2); file.seekg(0x3C); file.read(reinterpret_cast<char *>(&offset), sizeof(offset));
    if (!file || mz[0] != 'M' || mz[1] != 'Z' || offset > 16 * 1024 * 1024) return false;
    file.seekg(offset); file.read(reinterpret_cast<char *>(&signature), sizeof(signature));
    file.read(reinterpret_cast<char *>(&machine), sizeof(machine));
    return file && signature == 0x4550 && machine == 0x14C;
}

static std::wstring IniString(const fs::path &config, const wchar_t *section, const wchar_t *key, const wchar_t *fallback = L"")
{
    std::wstring value(32768, L'\0');
    DWORD length = GetPrivateProfileStringW(section, key, fallback, value.data(), static_cast<DWORD>(value.size()), config.c_str());
    value.resize(length);
    return value;
}

static std::wstring SearchPaths(const std::wstring &existing, const fs::path &base, const std::vector<fs::path> &required)
{
    std::vector<std::wstring> paths;
    auto add = [&](fs::path path) {
        if (path.empty()) return;
        if (path.is_relative()) path = base / path;
        const std::wstring value = path.lexically_normal().wstring();
        if (std::find(paths.begin(), paths.end(), value) == paths.end()) paths.push_back(value);
    };
    for (const auto &path : required) add(path);
    std::wistringstream source(existing); std::wstring token;
    while (std::getline(source, token, L','))
    {
        const auto first = token.find_first_not_of(L" \t\r\n\"");
        if (first == std::wstring::npos) continue;
        const auto last = token.find_last_not_of(L" \t\r\n\"");
        add(fs::path(token.substr(first, last - first + 1)));
    }
    std::wstring result;
    for (const auto &path : paths) { if (!result.empty()) result += L","; result += path; }
    return result;
}

static void Write(const fs::path &path, const Json &data)
{
    fs::create_directories(path.parent_path());
    auto temporary = path; temporary += L".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << data.dump(2); file.close();
    if (!file || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Private report write failed.");
    }
}

static fs::path Prepare(const fs::path &probe, const fs::path &runtime, const fs::path &game, const fs::path &state)
{
    const fs::path host = state / L"ReShadeHost", addons = state / L"TestAddons";
    const fs::path config = host / L"ReShade.ini", game_config = game.parent_path() / L"ReShade.ini";
    const fs::path source = runtime / L"ReShadeHost";
    fs::create_directories(host); fs::create_directories(addons);
    fs::copy_file(fs::is_regular_file(game_config) ? game_config : source / L"ReShade.ini", config, fs::copy_options::overwrite_existing);
    fs::copy_file(runtime / L"Addons" / L"Asteria.addon", addons / L"Asteria.addon", fs::copy_options::overwrite_existing);
    fs::copy_file(probe / L"bin" / L"AsteriaNativeInputProbe.addon", addons / L"AsteriaNativeInputProbe.addon", fs::copy_options::overwrite_existing);
    const fs::path shader = runtime.parent_path() / L"Shader";
    const auto effects = SearchPaths(IniString(game_config, L"GENERAL", L"EffectSearchPaths"), game.parent_path(),
                                     {shader / L"Effects", shader / L"Includes", game.parent_path() / L"reshade-shaders" / L"Shaders"});
    const auto textures = SearchPaths(IniString(game_config, L"GENERAL", L"TextureSearchPaths"), game.parent_path(),
                                      {shader / L"Textures", game.parent_path() / L"reshade-shaders" / L"Textures"});
    fs::path preset = IniString(game_config, L"GENERAL", L"PresetPath", L"ReShadePreset.ini");
    if (preset.is_relative()) preset = game.parent_path() / preset;
    if (!fs::is_regular_file(preset)) preset = source / L"Asteria.ini";
    fs::copy_file(preset, host / L"Asteria.ini", fs::copy_options::overwrite_existing);
    WritePrivateProfileStringW(L"ADDON", L"AddonPath", addons.c_str(), config.c_str());
    WritePrivateProfileStringW(L"GENERAL", L"EffectSearchPaths", effects.c_str(), config.c_str());
    WritePrivateProfileStringW(L"GENERAL", L"TextureSearchPaths", textures.c_str(), config.c_str());
    WritePrivateProfileStringW(L"GENERAL", L"PresetPath", L"Asteria.ini", config.c_str());
    WritePrivateProfileStringW(L"GENERAL", L"PerformanceMode", L"0", config.c_str());
    WritePrivateProfileStringW(L"INSTALL", L"Logging", L"1", config.c_str());
    const fs::path local = state / L"LocalAppData";
    const fs::path controls = local / L"Danyria" / L"AsteriaRuntime";
    fs::create_directories(controls);
    const fs::path existing_control = fs::path(Environment(L"LOCALAPPDATA")) / L"Danyria" / L"AsteriaRuntime" / L"asteria_runtime_control.json";
    if (fs::is_regular_file(existing_control)) fs::copy_file(existing_control, controls / L"asteria_runtime_control.json", fs::copy_options::overwrite_existing);
    else Write(controls / L"asteria_runtime_control.json", {{"enabled", true}, {"smaa", {{"enabled", true}, {"strength", 0.42}, {"quality", "medium"}}},
                                                           {"cas", {{"enabled", true}, {"strength", 0.35}}}});
    return host;
}

static std::vector<wchar_t> ChildEnvironment(const std::vector<std::pair<std::wstring, std::wstring>> &overrides)
{
    LPWCH source = GetEnvironmentStringsW();
    if (!source) throw std::runtime_error("Environment inventory failed.");
    std::vector<std::wstring> values;
    for (const wchar_t *item = source; *item; item += wcslen(item) + 1) values.emplace_back(item);
    FreeEnvironmentStringsW(source);
    for (const auto &entry : overrides)
    {
        const std::wstring prefix = entry.first + L"=";
        values.erase(std::remove_if(values.begin(), values.end(), [&](const std::wstring &value) {
            return value.size() >= prefix.size() && !_wcsnicmp(value.c_str(), prefix.c_str(), prefix.size());
        }), values.end());
        values.push_back(prefix + entry.second);
    }
    std::sort(values.begin(), values.end(), [](const auto &a, const auto &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> result;
    for (const auto &value : values) { result.insert(result.end(), value.begin(), value.end()); result.push_back(0); }
    result.push_back(0);
    return result;
}

static bool ProcessPathMatches(DWORD pid, const fs::path &game)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t name[32768]{}; DWORD size = 32768;
    const bool queried = QueryFullProcessImageNameW(process, 0, name, &size) != FALSE;
    CloseHandle(process);
    return queried && !_wcsicmp(fs::path(name).lexically_normal().c_str(), game.lexically_normal().c_str());
}

int wmain(int argc, wchar_t **argv)
{
    const fs::path probe = fs::absolute(fs::path(argv[0])).parent_path().parent_path();
    const fs::path runtime = probe.parent_path().parent_path();
    fs::path state = probe / L"results" / L"GameProbe";
    fs::path report_path = state / L"launch_status.json", status = state / L"native_input_status.json";
    fs::path game = L"H:/SteamLibrary/steamapps/common/Left 4 Dead 2";
    bool prepare_only = false, pipeline_probe = false, camera_probe = false, uniform_probe = false, enable_fg = false, request_quit = false;
    bool recording_association = false;
    uint32_t duration = 60;
    std::wstring map, custom_dir;
    Json report = {{"author", "Anokianx"}, {"schema_version", 1}, {"mode", "native-input-game-probe"},
                   {"native_fg_ready", false}, {"started", false}, {"layer_registration_changed", false}, {"game_files_modified", false}};
    PROCESS_INFORMATION process{};
    try
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::wstring argument = argv[index];
            if (argument == L"--prepare-only") prepare_only = true;
            else if (argument == L"--pipeline-probe") pipeline_probe = true;
            else if (argument == L"--camera-probe") camera_probe = true;
            else if (argument == L"--uniform-probe") uniform_probe = true;
            else if (argument == L"--recording-association") recording_association = true;
            else if (argument == L"--enable-fg") { enable_fg = true; uniform_probe = true; }
            else if (argument == L"--request-quit") request_quit = true;
            else if (argument == L"--output-dir" && index + 1 < argc) custom_dir = argv[++index];
            else if (argument == L"--game" && index + 1 < argc) game = argv[++index];
            else if (argument == L"--map" && index + 1 < argc) map = argv[++index];
            else if (argument == L"--duration" && index + 1 < argc)
            {
                const std::wstring value = argv[++index]; size_t used = 0;
                unsigned long seconds = std::stoul(value, &used);
                if (used != value.size() || seconds < 1 || seconds > 3600) throw std::runtime_error("Duration must be 1 through 3600 seconds.");
                duration = static_cast<uint32_t>(seconds);
            }
            else throw std::runtime_error("Usage: --game <directory or exe> --prepare-only --duration <seconds> --map <local map name> --pipeline-probe --camera-probe --uniform-probe --recording-association --enable-fg --request-quit --output-dir <name>.");
        }
        if (recording_association) { uniform_probe = true; pipeline_probe = true; }
        else if (uniform_probe) { pipeline_probe = true; camera_probe = true; }
        if (!custom_dir.empty())
            state = probe / L"results" / custom_dir;
        else if (recording_association)
            state = probe / L"results" / L"GameRecordingAssociation";
        else if (enable_fg)
            state = probe / L"results" / L"GameFrameGeneration";
        else if (pipeline_probe || camera_probe)
            state = probe / L"results" / (uniform_probe ? L"GameNativeUniform" : (pipeline_probe ? (camera_probe ? L"GameNativeProvenance" : L"GamePipeline") : L"GameCamera"));
        report_path = state / L"launch_status.json";
        status = state / L"native_input_status.json";
        report["pipeline_probe_enabled"] = pipeline_probe;
        report["camera_probe_enabled"] = camera_probe;
        report["uniform_probe_enabled"] = uniform_probe;
        report["recording_association_probe_enabled"] = recording_association;
        report["frame_generation_enabled"] = enable_fg;
        report["request_quit_enabled"] = request_quit;
        if (!map.empty() && (map.size() > 128 || map.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::wstring::npos))
            throw std::runtime_error("Map must be a plain local map name.");
        game = fs::absolute(game).lexically_normal();
        if (fs::is_directory(game)) game /= L"left4dead2.exe";
        if (_wcsicmp(game.filename().c_str(), L"left4dead2.exe")) throw std::runtime_error("Only left4dead2.exe is supported.");
        if (!fs::is_regular_file(game) || !X86(game)) throw std::runtime_error("A valid x86 L4D2 executable is required.");
        for (const fs::path &required : {game.parent_path() / L"d3d9.dll", game.parent_path() / L"dxvk_d3d9.dll",
                                       runtime / L"Host" / L"ReShade32.dll", probe / L"bin" / L"AsteriaNativeInputProbe.addon", runtime / L"Addons" / L"Asteria.addon"})
            if (!fs::is_regular_file(required) || !X86(required)) throw std::runtime_error("An existing x86 DXVK chain and built add-ons are required.");
        std::ifstream manifest_file(runtime / L"Host" / L"ReShade32.json");
        const Json manifest = Json::parse(manifest_file);
        if (manifest.at("layer").value("name", std::string()) != "VK_LAYER_Asteria_ReShade") throw std::runtime_error("Unexpected ReShade layer identity.");
        const bool elevated = Elevated(), registered = RegisteredLayer(), steam = Running(L"steam.exe"), running = Running(L"left4dead2.exe");
        report["elevated"] = elevated; report["registered_layer_available"] = registered;
        report["steam_running"] = steam; report["game_already_running"] = running;
        report["game_executable"] = game.u8string(); report["prepare_only"] = prepare_only;
        report["duration_seconds"] = duration;
        report["can_launch"] = !running && steam && (!elevated || registered);
        if (running) throw std::runtime_error("L4D2 is already running. The probe will not close or attach to it.");
        const fs::path host = Prepare(probe, runtime, game, state);
        const fs::path layer = runtime / L"Host";
        const fs::path frame_layer = probe.parent_path() / L"Layer";
        const fs::path uniform_profiles = probe.parent_path() / L"Research" / L"NativeShaders" / L"native-uniform-profiles.json";
        if (uniform_probe && (!fs::is_regular_file(frame_layer / L"bin" / L"AsteriaFrameGenerationLayer.dll") || !fs::is_regular_file(uniform_profiles)))
            throw std::runtime_error("The isolated mapped-buffer layer and verified native uniform profiles must be built first.");
        const bool frame_layer_first = Environment(L"ASTERIA_FG_LAYER_ORDER") == L"fg-first";
        const bool frame_layer_only = Environment(L"ASTERIA_FG_LAYER_ORDER") == L"fg-only";
        const bool disable_reshade_layer = Environment(L"ASTERIA_FG_DISABLE_RESHADE_LAYER") == L"1";
        std::wstring layers = disable_reshade_layer ? L"VK_LAYER_Asteria_FrameGeneration" :
            (frame_layer_only && uniform_probe ? L"VK_LAYER_Asteria_FrameGeneration" :
            (frame_layer_first && uniform_probe ? L"VK_LAYER_Asteria_FrameGeneration;VK_LAYER_Asteria_ReShade" : L"VK_LAYER_Asteria_ReShade"));
        if (!disable_reshade_layer && uniform_probe && !frame_layer_first && !frame_layer_only) layers += L";VK_LAYER_Asteria_FrameGeneration";
        std::wistringstream previous(Environment(L"VK_INSTANCE_LAYERS")); std::wstring name;
        while (std::getline(previous, name, L';'))
            if (!name.empty() && name != L"VK_LAYER_Asteria_ReShade" && name != L"VK_LAYER_reshade" &&
                (!uniform_probe || name != L"VK_LAYER_Asteria_FrameGeneration")) layers += L";" + name;
        const bool existing_explicit_path = !Environment(L"VK_LAYER_PATH").empty();
        const std::wstring path_key = existing_explicit_path ? L"VK_LAYER_PATH" : L"VK_ADD_LAYER_PATH";
        const std::wstring prior_path = Environment(path_key.c_str());
        const std::wstring paths = layer.wstring() + (uniform_probe ? L";" + frame_layer.wstring() : L"") + (prior_path.empty() ? L"" : L";" + prior_path);
        std::vector<std::pair<std::wstring, std::wstring>> environment = {
            {L"RESHADE_BASE_PATH_OVERRIDE", host.wstring()}, {path_key, paths}, {L"VK_INSTANCE_LAYERS", layers},
            {L"DISABLE_VK_LAYER_reshade_1", L"1"}, {L"RESHADE_DISABLE_GRAPHICS_HOOK", L"1"},
            {L"ASTERIA_NATIVE_INPUT_PROBE", L"1"}, {L"ASTERIA_NATIVE_INPUT_PROBE_OUTPUT", status.wstring()},
            {L"ASTERIA_NATIVE_ENGINE_CAMERA", camera_probe ? L"1" : L"0"},
            {L"ASTERIA_NATIVE_PIPELINE_PROBE", pipeline_probe ? L"1" : L"0"},
            {L"ASTERIA_NATIVE_UNIFORM_PROBE", uniform_probe ? L"1" : L"0"},
            {L"ASTERIA_FG_DRAW_VIEWPORT_DIAGNOSTICS",recording_association ? L"1" : L"0"},
            {L"ASTERIA_NATIVE_UNIFORM_PROFILES", uniform_probe ? uniform_profiles.wstring() : L""},
            {L"ASTERIA_FG_ENABLE", enable_fg ? L"1" : L"0"},
            {L"ASTERIA_FG_PROVIDER_REGISTRATION", Environment(L"ASTERIA_FG_PROVIDER_REGISTRATION")},
            {L"ASTERIA_FG_PROVIDER_REGISTRATION_EARLY", Environment(L"ASTERIA_FG_PROVIDER_REGISTRATION_EARLY")},
            {L"ASTERIA_FG_ESTIMATED_INTERNAL", Environment(L"ASTERIA_FG_ESTIMATED_INTERNAL")},
            {L"ASTERIA_FG_NATIVE_PROXY_IMAGES", Environment(L"ASTERIA_FG_NATIVE_PROXY_IMAGES")},
            {L"ASTERIA_FG_NATIVE_ACQUIRE", Environment(L"ASTERIA_FG_NATIVE_ACQUIRE")},
            {L"ASTERIA_FG_NATIVE_PASSTHROUGH", Environment(L"ASTERIA_FG_NATIVE_PASSTHROUGH")},
            {L"ASTERIA_FG_NATIVE_BACKBUFFER", Environment(L"ASTERIA_FG_NATIVE_BACKBUFFER")},
            {L"ASTERIA_FG_RETURN_NATIVE_SWAPCHAIN", Environment(L"ASTERIA_FG_RETURN_NATIVE_SWAPCHAIN")},
            {L"ASTERIA_FG_DISABLE_RESHADE_LAYER", Environment(L"ASTERIA_FG_DISABLE_RESHADE_LAYER")},
            {L"ASTERIA_FG_PROBE_OUTPUT", (state / L"mapped_layer_status.json").wstring()},
            {L"LOCALAPPDATA", (state / L"LocalAppData").wstring()}, {L"DXVK_LOG_PATH", state.wstring()},
            {L"DXVK_STATE_CACHE_PATH", state.wstring()}, {L"SteamAppId", L"550"}, {L"SteamGameId", L"550"}
        };
        const std::wstring command = L"\"" + game.wstring() + L"\" -vulkan -novid -multirun" + (map.empty() ? L"" : L" +map " + map);
        report["command_line"] = fs::path(command).u8string();
        report["working_directory"] = game.parent_path().u8string();
        report["private_host"] = host.u8string(); report["probe_status"] = status.u8string();
        report["child_environment"] = Json::object();
        for (const auto &entry : environment) report["child_environment"][fs::path(entry.first).u8string()] = fs::path(entry.second).u8string();
        report["prepared"] = true;
        Write(report_path, report);
        if (prepare_only)
        {
            std::cout << "prepared=true, can_launch=" << report["can_launch"] << ", game_started=false\n";
            return 0;
        }
        if (!steam) throw std::runtime_error("Steam must already be running; this launcher does not invoke a Steam bridge.");
        if (elevated && !registered) throw std::runtime_error("Run with normal privileges or use an already registered Asteria layer. This probe never registers or elevates.");
        if (Running(L"left4dead2.exe")) throw std::runtime_error("L4D2 started after preparation. Refusing a second process.");
        fs::remove(status);
        std::vector<wchar_t> buffer(command.begin(), command.end()); buffer.push_back(0);
        auto child_environment = ChildEnvironment(environment);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        if (!CreateProcessW(game.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                            child_environment.data(), game.parent_path().c_str(), &startup, &process))
            throw std::runtime_error("CreateProcess failed: " + std::to_string(GetLastError()));
        CloseHandle(process.hThread); process.hThread = nullptr;
        report["started"] = true; report["created_pid"] = process.dwProcessId;
        Write(report_path, report);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
        bool observed = false, application_resources = false, exited = false, frame_progress_fresh = false;
        Json last_observation = Json::object();
        while (std::chrono::steady_clock::now() < deadline)
        {
            try
            {
                std::ifstream file(status); Json data = Json::parse(file);
                const DWORD pid = data.value("pid", 0u);
                if (pid && ProcessPathMatches(pid, game))
                {
                    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
                    const double age = now - data.value("heartbeat", 0.0);
                    const bool fresh = age >= -1.0 && age <= 5.0;
                    observed = true; report["probe_pid"] = pid;
                    report["native_fg_ready"] = false;
                    report["probe_process_identity_matches"] = true;
                    report["probe_heartbeat_age_seconds"] = age;
                    report["probe_heartbeat_fresh"] = fresh;
                    report["probe_session_active"] = fresh && data.value("session_active", false);
                    report["probe_resources_are_live"] = fresh && data.value("resources_are_live", false);
                    frame_progress_fresh = false;
                    for (const auto &device : data.at("devices"))
                    {
                        if (!device.at("resources").empty()) application_resources = true;
                        const uint64_t presented = device.value("last_present_tick_ms", uint64_t(0));
                        const uint64_t tick = GetTickCount64();
                        frame_progress_fresh = frame_progress_fresh || (presented && tick >= presented && tick - presented <= 5000);
                    }
                    report["render_observation_live"] = fresh && frame_progress_fresh;
                    last_observation = data;
                    Write(state / L"latest_observation.json", data);
                    if (application_resources) Write(state / L"active_status.json", data);
                }
            }
            catch (...) {}
            report["probe_observed"] = observed; report["application_resources_observed"] = application_resources;
            report["heartbeat"] = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            Write(report_path, report);
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) { exited = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        report["auxiliary_collection_observations"] = Json::array();
        if (uniform_probe)
            for (const auto &relative : {L"NativeUniform/status.json", L"Provenance/status.json", L"mapped_layer_status.json"})
                try
                {
                    const fs::path source = state / relative;
                    std::ifstream file(source); const Json data = Json::parse(file);
                    if (!data.is_object() || data.value("pid", 0u) != process.dwProcessId) continue;
                    const fs::path target = source.parent_path() / (source.filename() == L"status.json" ? L"collection_observation.json" : L"mapped_layer_collection_observation.json");
                    Write(target, data);
                    report["auxiliary_collection_observations"].push_back(target.u8string());
                }
                catch (...) {}
        if (request_quit && !exited)
        {
            report["quit_requested"] = true;
            const DWORD target_pid = process.dwProcessId;
            EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
                DWORD win_pid = 0;
                GetWindowThreadProcessId(hwnd, &win_pid);
                if (win_pid == static_cast<DWORD>(lparam) && IsWindowVisible(hwnd))
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return TRUE;
            }, static_cast<LPARAM>(target_pid));
            if (WaitForSingleObject(process.hProcess, 15000) == WAIT_OBJECT_0)
                exited = true;
            report["exited_after_normal_quit"] = exited;
            try
            {
                const fs::path source = state / L"mapped_layer_status.json";
                std::ifstream file(source); const Json data = Json::parse(file);
                if (data.is_object() && data.value("pid", 0u) == process.dwProcessId)
                    Write(state / L"mapped_layer_final_observation.json", data);
            }
            catch (...) {}
        }
        DWORD exit_code = STILL_ACTIVE; GetExitCodeProcess(process.hProcess, &exit_code);
        CloseHandle(process.hProcess); process.hProcess = nullptr;
        report["collection_complete"] = true; report["game_exited"] = exited; report["game_exit_code"] = exit_code;
        report["game_left_running"] = !exited; report["native_fg_ready"] = false;
        if (exited)
        {
            report["probe_session_active"] = false; report["probe_resources_are_live"] = false;
            const DWORD probe_pid = last_observation.value("pid", 0u);
            if (probe_pid && probe_pid == process.dwProcessId)
            {
                last_observation["session_active"] = false; last_observation["resources_are_live"] = false;
                last_observation["native_fg_ready"] = false;
                last_observation["finalization"] = {{"method", "external_process_identity_check"}, {"addon_uninit_verified", false},
                                                     {"resource_liveness", "historical"}};
                Write(state / L"final_observation.json", last_observation);
            }
        }
        report["detail"] = application_resources && frame_progress_fresh && !exited ?
                               "Fresh graphics presentation observed; native motion inputs remain unverified." :
                               application_resources ? "Only historical graphics observations are available; writer liveness is not render progress." :
                                                       "No usable native resource observation; inspect the private ReShade/DXVK logs.";
        Write(report_path, report);
        std::cout << "probe_observed=" << observed << ", application_resources_observed=" << application_resources
                  << ", game_left_running=" << !exited << ", native_fg_ready=false\n";
        return application_resources ? 0 : 2;
    }
    catch (const std::exception &error)
    {
        if (process.hThread) CloseHandle(process.hThread);
        if (process.hProcess) CloseHandle(process.hProcess);
        report["error"] = error.what();
        try { Write(report_path, report); } catch (...) {}
        std::cerr << error.what() << '\n';
        return 1;
    }
}
