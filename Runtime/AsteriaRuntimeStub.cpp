#include "AsteriaRuntimeStub.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>

struct RuntimeContext {
    std::filesystem::path control;
    std::filesystem::path status;
    bool enabled = false;
    bool suspended = false;
    ULONGLONG last_control = 0;
    ULONGLONG last_status = 0;
    std::chrono::steady_clock::time_point last_frame{};
    double frame_ms = 0.0;
};

static void ReadControl(RuntimeContext& context) {
    std::ifstream input(context.control, std::ios::binary);
    context.enabled = false;
    if (!input) return;
    const std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const size_t key = data.find("\"enabled\"");
    if (key == std::string::npos) return;
    const size_t colon = data.find(':', key + 9);
    if (colon == std::string::npos) return;
    const size_t value = data.find_first_not_of(" \t\r\n", colon + 1);
    context.enabled = value != std::string::npos && data.compare(value, 4, "true") == 0;
}

static void WriteStatus(const RuntimeContext& context, bool connected, HRESULT result) {
    const auto temporary = context.status.wstring() + L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    const double heartbeat = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    output << std::setprecision(17)
           << "{\"protocol\":1,\"connected\":" << (connected ? "true" : "false")
           << ",\"api\":\"D3D9\",\"mode\":\"validation\",\"frame_ms\":" << context.frame_ms
           << ",\"heartbeat\":" << heartbeat << ",\"render_result\":" << result << "}";
    output.close();
    MoveFileExW(temporary.c_str(), context.status.c_str(), MOVEFILE_REPLACE_EXISTING);
}

struct Vertex { float x, y, z, rhw; DWORD color; };

static HRESULT DrawMarker(IDirect3DDevice9* device) {
    D3DVIEWPORT9 viewport{};
    HRESULT result = device->GetViewport(&viewport);
    if (FAILED(result)) return result;
    IDirect3DStateBlock9* state = nullptr;
    result = device->CreateStateBlock(D3DSBT_ALL, &state);
    if (FAILED(result)) return result;
    result = device->BeginScene();
    if (FAILED(result)) { state->Release(); return result; }
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    const DWORD color = D3DCOLOR_ARGB(220, 80, 150 + (GetTickCount64() / 8) % 100, 255);
    const float left = static_cast<float>(viewport.X) + 12;
    const float top = static_cast<float>(viewport.Y) + 12;
    const float right = static_cast<float>(viewport.X + viewport.Width) - 12;
    const float bottom = static_cast<float>(viewport.Y + viewport.Height) - 12;
    const Vertex lines[] = {
        {left, top, 0, 1, color}, {right, top, 0, 1, color},
        {right, top, 0, 1, color}, {right, bottom, 0, 1, color},
        {right, bottom, 0, 1, color}, {left, bottom, 0, 1, color},
        {left, bottom, 0, 1, color}, {left, top, 0, 1, color},
    };
    result = device->DrawPrimitiveUP(D3DPT_LINELIST, 4, lines, sizeof(Vertex));
    const HRESULT restore = state->Apply();
    const HRESULT end = device->EndScene();
    state->Release();
    if (FAILED(result)) return result;
    return FAILED(restore) ? restore : end;
}

extern "C" UINT WINAPI AsteriaGetApiVersion() { return 1; }

extern "C" HRESULT WINAPI AsteriaInitialize(const wchar_t* control, const wchar_t* status, void** handle) {
    if (!control || !*control || !status || !*status || !handle) return E_INVALIDARG;
    *handle = nullptr;
    try {
        auto context = std::make_unique<RuntimeContext>();
        context->control = control;
        context->status = status;
        ReadControl(*context);
        *handle = context.release();
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}

extern "C" HRESULT WINAPI AsteriaRenderFrame(void* handle, IDirect3DDevice9* device) {
    if (!handle || !device) return E_INVALIDARG;
    auto& context = *static_cast<RuntimeContext*>(handle);
    if (context.suspended) return D3DERR_DEVICELOST;
    const auto frame = std::chrono::steady_clock::now();
    if (context.last_frame.time_since_epoch().count())
        context.frame_ms = std::chrono::duration<double, std::milli>(frame - context.last_frame).count();
    context.last_frame = frame;
    const ULONGLONG now = GetTickCount64();
    if (!context.last_control || now - context.last_control >= 250) {
        ReadControl(context);
        context.last_control = now;
    }
    const HRESULT result = context.enabled ? DrawMarker(device) : S_FALSE;
    if (!context.last_status || now - context.last_status >= 250) {
        WriteStatus(context, SUCCEEDED(result), result);
        context.last_status = now;
    }
    return result;
}

extern "C" HRESULT WINAPI AsteriaBeforeReset(void* handle) {
    if (!handle) return E_INVALIDARG;
    auto& context = *static_cast<RuntimeContext*>(handle);
    context.suspended = true;
    context.last_frame = {};
    WriteStatus(context, false, D3DERR_DEVICELOST);
    return S_OK;
}

extern "C" HRESULT WINAPI AsteriaAfterReset(void* handle, HRESULT reset_result) {
    if (!handle) return E_INVALIDARG;
    auto& context = *static_cast<RuntimeContext*>(handle);
    context.suspended = FAILED(reset_result);
    context.last_frame = {};
    context.last_status = 0;
    return reset_result;
}

extern "C" void WINAPI AsteriaShutdown(void* handle) {
    if (!handle) return;
    auto context = static_cast<RuntimeContext*>(handle);
    WriteStatus(*context, false, S_OK);
    delete context;
}
