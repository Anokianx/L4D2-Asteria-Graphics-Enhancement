#pragma once

#include <windows.h>
#include <winternl.h>
#include <dxgi1_4.h>
#include <d3dkmthk.h>
#include <psapi.h>
#include <reshade.hpp>
#include <json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <numeric>
#include <string>
#include <vector>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")

class AsteriaRuntimeMetrics
{
    using Clock = std::chrono::steady_clock;
    struct Frame { Clock::time_point time; double milliseconds; };
    std::deque<Frame> frames_;
    Clock::time_point sampled_{};
    Clock::time_point adapter_retry_{};
    Clock::time_point cpu_sampled_{};
    unsigned long long cpu_ticks_ = 0;
    IDXGIAdapter3 *adapter_ = nullptr;
    HMODULE dxgi_ = nullptr;
    bool adapter_checked_ = false;
    nlohmann::json cached_;
    nlohmann::json gpu_total_ = nullptr;
    std::string gpu_name_;
    std::string adapter_match_;
    std::string vram_status_ = "waiting-for-render-device";
    nlohmann::json vram_details_ = nlohmann::json::object();

    static nlohmann::json empty_metrics()
    {
        return {{"fps_low_1", nullptr}, {"frame_p95_ms", nullptr}, {"vram_mb", nullptr},
                {"vram_budget_mb", nullptr}, {"vram_total_mb", nullptr}, {"shared_vram_mb", nullptr},
                {"cpu_percent", nullptr}, {"ram_mb", nullptr}, {"system_ram_percent", nullptr},
                {"width", nullptr}, {"height", nullptr}, {"gpu_name", nullptr},
                {"adapter_match", nullptr}, {"vram_status", "waiting-for-render-device"},
                {"vram_details", nlohmann::json::object()}, {"metrics_available", false}};
    }

    static unsigned long long ticks(const FILETIME &value)
    {
        return (static_cast<unsigned long long>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    }

    static std::string utf8(const wchar_t *value)
    {
        const int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (count <= 1) return {};
        std::string output(count, '\0');
        WideCharToMultiByte(CP_UTF8, 0, value, -1, output.data(), count, nullptr, nullptr);
        output.resize(count - 1);
        return output;
    }

    static nlohmann::json adapter_total(const LUID &luid)
    {
        D3DKMT_OPENADAPTERFROMLUID opened{};
        opened.AdapterLuid = luid;
        if (D3DKMTOpenAdapterFromLuid(&opened) < 0) return nullptr;
        D3DKMT_SEGMENTSIZEINFO sizes{};
        D3DKMT_QUERYADAPTERINFO query{};
        query.hAdapter = opened.hAdapter;
        query.Type = KMTQAITYPE_GETSEGMENTSIZE;
        query.pPrivateDriverData = &sizes;
        query.PrivateDriverDataSize = sizeof(sizes);
        const bool succeeded = D3DKMTQueryAdapterInfo(&query) >= 0;
        D3DKMT_CLOSEADAPTER closed{};
        closed.hAdapter = opened.hAdapter;
        D3DKMTCloseAdapter(&closed);
        if (!succeeded) return nullptr;
        return static_cast<double>(sizes.DedicatedVideoMemorySize) / (1024.0 * 1024.0);
    }

    void prepare_adapter(reshade::api::effect_runtime *runtime, Clock::time_point now)
    {
        if (!runtime || adapter_ || (adapter_checked_ && now - adapter_retry_ < std::chrono::seconds(10))) return;
        adapter_checked_ = true;
        adapter_retry_ = now;
        const auto device = runtime->get_device();
        LUID target{};
        const bool luid_reported = device->get_property(reshade::api::device_properties::adapter_luid, &target);
        const bool has_luid = luid_reported && (target.HighPart || target.LowPart);
        uint32_t vendor = 0, chip = 0;
        const bool vendor_reported = device->get_property(reshade::api::device_properties::vendor_id, &vendor);
        const bool chip_reported = device->get_property(reshade::api::device_properties::device_id, &chip);
        const bool has_ids = vendor_reported && chip_reported && vendor && chip;
        vram_details_ = {{"luid_reported", luid_reported}, {"luid_high", target.HighPart},
                         {"luid_low", target.LowPart}, {"vendor_reported", vendor_reported},
                         {"device_reported", chip_reported}, {"vendor_id", vendor}, {"device_id", chip}};
        char description[256]{};
        if (device->get_property(reshade::api::device_properties::description, description))
            gpu_name_ = description;
        if (!has_luid && !has_ids) { vram_status_ = "render-adapter-id-unavailable"; return; }
        if (!dxgi_)
        {
            wchar_t system[MAX_PATH]{};
            const UINT length = GetSystemDirectoryW(system, MAX_PATH);
            if (!length || length >= MAX_PATH) { vram_status_ = "system-dxgi-path-unavailable"; return; }
            const std::wstring path = std::wstring(system) + L"\\dxgi.dll";
            dxgi_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!dxgi_) vram_details_["load_error"] = GetLastError();
        }
        if (!dxgi_) { vram_status_ = "system-dxgi-load-failed"; return; }
        wchar_t module_path[MAX_PATH]{};
        if (GetModuleFileNameW(dxgi_, module_path, MAX_PATH)) vram_details_["dxgi_path"] = utf8(module_path);
        const auto create_factory = reinterpret_cast<HRESULT(WINAPI *)(REFIID, void **)>(GetProcAddress(dxgi_, "CreateDXGIFactory1"));
        if (!create_factory) { vram_status_ = "dxgi-factory-entry-unavailable"; return; }
        IDXGIFactory1 *factory = nullptr;
        const HRESULT factory_result = create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory));
        vram_details_["factory_result"] = static_cast<unsigned long>(factory_result);
        if (FAILED(factory_result)) { vram_status_ = "dxgi-factory-create-failed"; return; }
        IDXGIAdapter1 *candidate = nullptr;
        DXGI_ADAPTER_DESC1 selected{};
        unsigned matches = 0;
        vram_details_["adapters"] = nlohmann::json::array();
        for (UINT index = 0;; ++index)
        {
            IDXGIAdapter1 *current = nullptr;
            const HRESULT enum_result = factory->EnumAdapters1(index, &current);
            if (enum_result != S_OK) { vram_details_["enum_result"] = static_cast<unsigned long>(enum_result); break; }
            DXGI_ADAPTER_DESC1 desc{};
            const HRESULT desc_result = current->GetDesc1(&desc);
            const bool read = SUCCEEDED(desc_result);
            vram_details_["adapters"].push_back({{"index", index}, {"name", utf8(desc.Description)},
                {"vendor_id", desc.VendorId}, {"device_id", desc.DeviceId}, {"luid_high", desc.AdapterLuid.HighPart},
                {"luid_low", desc.AdapterLuid.LowPart}, {"flags", desc.Flags},
                {"desc_result", static_cast<unsigned long>(desc_result)}});
            const bool match = read && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                && (has_luid ? (desc.AdapterLuid.HighPart == target.HighPart && desc.AdapterLuid.LowPart == target.LowPart)
                             : (desc.VendorId == vendor && desc.DeviceId == chip));
            if (match)
            {
                ++matches;
                if (!candidate) { candidate = current; selected = desc; current = nullptr; }
            }
            if (current) current->Release();
        }
        factory->Release();
        vram_details_["matches"] = matches;
        vram_status_ = matches ? "render-adapter-match-ambiguous" : "render-adapter-not-found";
        if (candidate)
        {
            HRESULT interface_result = E_FAIL;
            if (matches == 1) interface_result = candidate->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&adapter_));
            vram_details_["interface_result"] = static_cast<unsigned long>(interface_result);
            if (matches == 1 && SUCCEEDED(interface_result))
            {
                gpu_name_ = utf8(selected.Description);
                adapter_match_ = has_luid ? "luid" : "unique-pci-id";
                gpu_total_ = adapter_total(selected.AdapterLuid);
                vram_status_ = "adapter-ready";
            }
            else if (matches == 1) vram_status_ = "dxgi-memory-interface-unavailable";
            candidate->Release();
        }
    }

    void trim_frames(Clock::time_point now)
    {
        while (!frames_.empty() && (now - frames_.front().time > std::chrono::seconds(3) || frames_.size() > 4096))
            frames_.pop_front();
    }

public:
    AsteriaRuntimeMetrics() : cached_(empty_metrics()) {}
    AsteriaRuntimeMetrics(const AsteriaRuntimeMetrics &) = delete;
    AsteriaRuntimeMetrics &operator=(const AsteriaRuntimeMetrics &) = delete;
    ~AsteriaRuntimeMetrics()
    {
        if (adapter_) adapter_->Release();
        if (dxgi_) FreeLibrary(dxgi_);
    }

    void add_frame(double frame_ms)
    {
        const auto now = Clock::now();
        if (!std::isfinite(frame_ms) || frame_ms <= 0.0 || frame_ms >= 500.0)
        {
            frames_.clear();
            cached_["fps_low_1"] = nullptr;
            cached_["frame_p95_ms"] = nullptr;
            return;
        }
        if (!frames_.empty() && now - frames_.back().time > std::chrono::milliseconds(500)) frames_.clear();
        frames_.push_back({now, frame_ms});
        trim_frames(now);
    }

    nlohmann::json sample(reshade::api::effect_runtime *runtime)
    {
        const auto now = Clock::now();
        if (sampled_.time_since_epoch().count() && now - sampled_ < std::chrono::seconds(1)) return cached_;
        sampled_ = now;
        cached_ = empty_metrics();
        trim_frames(now);
        if (frames_.size() >= 30)
        {
            std::vector<double> values;
            values.reserve(frames_.size());
            for (const auto &frame : frames_) values.push_back(frame.milliseconds);
            std::sort(values.begin(), values.end());
            const size_t percentile = static_cast<size_t>(std::ceil(values.size() * .95)) - 1;
            cached_["frame_p95_ms"] = values[percentile];
            if (values.size() >= 100)
            {
                const size_t count = (std::max)(size_t(1), static_cast<size_t>(std::ceil(values.size() * .01)));
                const double slow = std::accumulate(values.end() - count, values.end(), 0.0) / count;
                cached_["fps_low_1"] = 1000.0 / slow;
            }
        }
        FILETIME created{}, ended{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user))
        {
            const auto total = ticks(kernel) + ticks(user);
            const DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            if (cpu_sampled_.time_since_epoch().count() && total >= cpu_ticks_ && processors)
            {
                const double elapsed = std::chrono::duration<double>(now - cpu_sampled_).count();
                cached_["cpu_percent"] = (std::clamp)(100.0 * (total - cpu_ticks_) / (elapsed * 10000000.0 * processors), 0.0, 100.0);
            }
            cpu_ticks_ = total;
            cpu_sampled_ = now;
        }
        PROCESS_MEMORY_COUNTERS memory{};
        memory.cb = sizeof(memory);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
            cached_["ram_mb"] = static_cast<double>(memory.WorkingSetSize) / (1024.0 * 1024.0);
        MEMORYSTATUSEX system{};
        system.dwLength = sizeof(system);
        if (GlobalMemoryStatusEx(&system) && system.ullTotalPhys)
            cached_["system_ram_percent"] = 100.0 * (1.0 - static_cast<double>(system.ullAvailPhys) / system.ullTotalPhys);
        if (runtime)
        {
            uint32_t width = 0, height = 0;
            runtime->get_screenshot_width_and_height(&width, &height);
            if (width && height) { cached_["width"] = width; cached_["height"] = height; }
            const auto window = static_cast<HWND>(runtime->get_hwnd());
            if (!width || !height || (window && IsIconic(window)))
            {
                frames_.clear();
                cached_["fps_low_1"] = nullptr;
                cached_["frame_p95_ms"] = nullptr;
            }
        }
        prepare_adapter(runtime, now);
        if (!gpu_name_.empty()) cached_["gpu_name"] = gpu_name_;
        if (adapter_)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
            const HRESULT local_result = adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            const HRESULT shared_result = adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
            vram_details_["local_result"] = static_cast<unsigned long>(local_result);
            vram_details_["shared_result"] = static_cast<unsigned long>(shared_result);
            vram_status_ = SUCCEEDED(local_result) ? "live-process-local-memory" : "dxgi-memory-query-failed";
            if (SUCCEEDED(local_result))
            {
                cached_["vram_mb"] = static_cast<double>(local.CurrentUsage) / (1024.0 * 1024.0);
                cached_["vram_budget_mb"] = static_cast<double>(local.Budget) / (1024.0 * 1024.0);
            }
            if (SUCCEEDED(shared_result))
                cached_["shared_vram_mb"] = static_cast<double>(shared.CurrentUsage) / (1024.0 * 1024.0);
            cached_["vram_total_mb"] = gpu_total_;
            cached_["adapter_match"] = adapter_match_;
        }
        cached_["vram_status"] = vram_status_;
        cached_["vram_details"] = vram_details_;
        cached_["metrics_available"] = !cached_["ram_mb"].is_null() || !cached_["vram_mb"].is_null();
        return cached_;
    }
};
