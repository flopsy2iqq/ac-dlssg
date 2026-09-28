// A module whose NGX D3D11 exports are PE forwarders into nvngx_dlssnr.dll.
// GetProcAddress on it resolves into that module, so NgxHook must not hook the
// same code a second time (ngx review F4).
#pragma comment(linker, "/export:NVSDK_NGX_D3D11_CreateFeature=nvngx_dlssnr.NVSDK_NGX_D3D11_CreateFeature")
#pragma comment(linker, "/export:NVSDK_NGX_D3D11_EvaluateFeature=nvngx_dlssnr.NVSDK_NGX_D3D11_EvaluateFeature")
#pragma comment(linker, "/export:NVSDK_NGX_D3D11_EvaluateFeature_C=nvngx_dlssnr.NVSDK_NGX_D3D11_EvaluateFeature_C")

extern "C" __declspec(dllexport) int __cdecl FakeNgxForwarderMarker() { return 0x46574400; }
