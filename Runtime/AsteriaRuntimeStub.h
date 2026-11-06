#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

extern "C" {
UINT WINAPI AsteriaGetApiVersion();
HRESULT WINAPI AsteriaInitialize(const wchar_t* control, const wchar_t* status, void** handle);
HRESULT WINAPI AsteriaRenderFrame(void* handle, IDirect3DDevice9* device);
HRESULT WINAPI AsteriaBeforeReset(void* handle);
HRESULT WINAPI AsteriaAfterReset(void* handle, HRESULT reset_result);
void WINAPI AsteriaShutdown(void* handle);
}
