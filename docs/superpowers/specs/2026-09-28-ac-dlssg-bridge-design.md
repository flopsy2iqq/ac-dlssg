# ac-dlssg: design

Date: 2026-09-28. Status: approved by the user after an adversarial source review (39 confirmed findings applied). Milestones M0 and M1 are done; M2 is built and waits for its in-game checks (see section 11). A hybrid laptop was added as a second target system on 2026-09-28, with a standalone mode for systems without ReShade (sections 2, 3, 5, 6.10, 6.11, 11 and 12). Multi frame generation (3X and 4X) moved from section 15 into v1 on 2026-09-29, after DLSS-G 2X passed in game on both machines (sections 1, 3, 6.8, 6.10, 6.11, 8, 11 and 15). On 2026-09-29 the settings panel moved into the CSP Lua app for both modes, and the ReShade overlay panel was dropped for v1 (sections 3, 5, 6.9 and 11).

## 1. Goal

Bring NVIDIA DLSS Frame Generation (DLSS-G, 2X, and 3X or 4X where Streamline allows it) to Assetto Corsa with Custom Shaders Patch (CSP). CSP renders with DirectX 11. The goal is to let players afford heavier graphics settings: a base rate of about 45-50 fps that DLSS-G presents as about 90 fps.

The game keeps rendering in DirectX 11. Our DLL replaces CSP's swap chain with a proxy. At Present, the proxy copies the finished frame to a DirectX 12 swap chain created through NVIDIA Streamline, and DLSS-G inserts the generated frames there. Depth and motion vectors come from the DLSS upscaling pass that CSP already runs. Camera data comes from a small CSP Lua app.

RTX 40 and RTX 50 cards run DLSS-G natively. RTX 30 cards (every Ampere GeForce GPU, SM86, desktop and laptop) are supported through the third-party [dlssg_for_sm86](https://github.com/sdli1995/dlssg_for_sm86) spoof, which gates only on the SM86 architecture. Our installer downloads it from its author's repository after printing the notice of section 10 (`-NoSpoof` skips it).

Milestone M0 is already done: on the reference machine (RTX 3080, driver 616.64), the spoof enabled DLSS-G 2X, 3X and 4X in a native DLSS-G game (WheelMates) with no visible problems.

Two machines must run it: the reference desktop (RTX 3080) and a friend's hybrid laptop, an Acer Nitro 5 AN515-57 with an RTX 3050 Ti Laptop GPU (4 GB) whose internal panel is driven by the Intel iGPU. The laptop has no ReShade.

## 2. Success criteria

1. On the reference system, DLSS-G 2X runs stable for a 15-minute drive: no crash, no device removal, no freeze. The reference system is an RTX 3080 with driver 616.64, Windows 10 22H2, CSP 0.3.0-preview622 and ReShade 6.8.0. The second test system is the laptop: RTX 3050 Ti Laptop GPU (4 GB, 60-75 W) with Optimus and no MUX, i5-11400H with Intel UHD graphics, driver 610.88, Windows 11 25H2, CSP 0.3.0-preview634, no ReShade (standalone mode). Criterion 1 applies to both systems.
2. On the reference system, with settings that give a 45-50 fps base, the presented rate is 85-95 fps. On the laptop, the presented rate is at least 1.5 times the rate with DLSS-G off, and a 15-minute drive shows no stutter from video memory overflow. DLSS-G there costs an estimated 4-6 ms per real frame at 1080p (inferred from the spoof's published RTX 3080 Ti numbers), so a 50 fps base gives about 77-83 fps.
3. With DLSS-G off, the bridge costs less than 0.5 ms of GPU time per frame compared with no bridge. This includes both frame copies.
4. With DLSS-G off, the image and the presentation behaviour match the game without the bridge: same resolution, colours, UI, and VSync or tearing behaviour. In ReShade mode, ReShade effects and its overlay keep working.
5. If DLSS-G cannot run, the bridge passes through and the game runs as it would without the bridge. Reasons include no supported GPU, missing files, unsupported CSP settings, and a failed initialization. There are two exceptions:
   - On RTX 30, `acs.exe` loads the dlssg_for_sm86 proxy (`version.dll`) at process start, whatever the bridge decides. That proxy keeps its LoadLibrary hook and its NVAPI architecture redirect, and CSP's own NVAPI calls see that redirect too. Only removing the spoof gives an unmodified process.
   - The bridge cannot cover its own DLL being missing or blocked. Section 12 describes the recovery.
6. Another user can install and uninstall with one double-clickable script. The uninstaller restores every file and setting the installer changed, and a failed install leaves nothing changed.

## 3. Scope

In scope for v1:
- DLSS-G 2X (one generated frame per real frame) and NVIDIA Reflex Low Latency through Streamline.
- Multi frame generation: 3X and 4X (two or three generated frames per real frame), chosen with `fg_multiplier` or at runtime, when Streamline reports `DLSSGState::numFramesToGenerateMax` > 1. That is RTX 50 natively, and RTX 30 through dlssg_for_sm86, which reports a Blackwell-class GPU (the reference RTX 3080 ran 3X and 4X in M0). RTX 40 reports 1 and stays at 2X (6.8).
- CSP with its DLSS upscaler active (`[FSR] ACTIVE=1`, `OLD_IMPLEMENTATION=3`), in any DLSS quality mode including DLAA.
- CSP's flip-model swap chain in a borderless window (`FULLSCREEN=1`, the CSP default), at a `video.ini` resolution with the same aspect ratio as the window. CSP renders at the `video.ini` resolution and scales down into a swap chain at desktop resolution. The user's "2560x1440 on a 1080p monitor" setup is therefore CSP supersampling, not driver DSR.
- Two load modes. In ReShade mode, ReShade 6.8.0 or newer (the build with add-on support) is `dxgi.dll` and loads the bridge as its `ProxyLibrary`. In standalone mode, ReShade is absent and the bridge itself is the game folder's `dxgi.dll`.
- RTX 40 and RTX 50 natively; RTX 30 (every Ampere GeForce GPU, SM86, desktop and laptop) through dlssg_for_sm86.
- Hybrid (Optimus) laptops, where the NVIDIA GPU renders and the integrated GPU drives the display (6.11).
- A settings panel, a hotkey, a config file and a log. The panel is a window of the CSP Lua app in ReShade mode and in standalone mode alike, so there is one UI (6.9). The ReShade overlay panel is dropped for v1.
- An installer, an uninstaller, and CI-built releases on GitHub with build provenance.

Out of scope for v1. Each case is refused or ignored with a logged reason; the planned follow-ups are in section 15.
- A HUD-less colour buffer. CSP app windows and the ReShade overlay will show interpolation artifacts.
- Multi frame generation above 4X (Streamline allows up to 6X) and dynamic multi frame generation (`DLSSGMode::eDynamic`).
- RTX 20 series. The spoof has an SM75 route, but the user excluded RTX 20 and we have no card to test with.
- HDR output, VR, triple-screen mode, MSAA (`AASAMPLES>1`), `OLD_SWAPCHAIN=1`, `EXCLUSIVE_FULLSCREEN=1`, and letterboxed output (a `video.ini` aspect ratio that differs from the window while `ALLOW_STRETCHING=0`).
- Other CSP upscalers (FSR, XeSS, OptiScaler).
- Running together with `dlss5-bridge.addon64` or `renodx-dlss5.addon64`.
- Games other than Assetto Corsa.

## 4. Reference facts this design relies on

These were verified in sources or in the local installation on 2026-09-28. The implementation re-checks them at runtime and does not hard-code them.

**Swap chain.** CSP intercepts `D3D11CreateDeviceAndSwapChain` and creates the D3D11 device itself, with an explicit adapter, feature level 11_0 and no flags. It then calls `IDXGIFactory2::CreateSwapChainForHwnd` on CSP's loading worker thread. The swap chain is:
- desktop-sized, `DXGI_FORMAT_R8G8B8A8_UNORM`, 2 buffers;
- `DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT`;
- `FLIP_DISCARD`, `ALPHA_MODE_IGNORE`;
- flags `ALLOW_TEARING | FRAME_LATENCY_WAITABLE_OBJECT` (0x840);
- windowed, with `pFullscreenDesc = NULL`.

CSP's secondary windows use the legacy `IDXGIFactory::CreateSwapChain`. CSP takes buffer 0 once and creates its render target view with a NULL description.

**Present and pacing.** CSP presents with `SyncInterval=1, Flags=0` in VSync mode, and otherwise with `SyncInterval=0` and flags `0x200` (`ALLOW_TEARING`) or 0. It reads `DXGI_STATUS_OCCLUDED` from the result. With `ADVANCED_PACING=1` it waits on the frame-latency waitable object every frame. Its FPS limiter is a CPU timer (`FPS_CAP_MS` in `video.ini`).

**CSP's DLSS call.** CSP statically links an NGX client. The outermost D3D11 call is therefore `_nvngx.dll!NVSDK_NGX_D3D11_EvaluateFeature`, on the render thread, on the immediate context. What CSP passes:
- create flags `2`, which is `MVLowRes` only: depth is not inverted, motion vectors are not jittered, and the input is LDR;
- depth as `R32_TYPELESS`;
- motion vectors as `R16G16_FLOAT`, ping-ponging between two textures from frame to frame;
- `MV.Scale = -(render size)`;
- jitter as an 8-phase Halton(2,3) sequence in render pixels.

The DLSS feature is created about 30 s after the swap chain. CSP also has a separate DLSS-NR feature path.

**ReShade 6.8.0.**
- It loads `[PROXY] ProxyLibrary` from `<base>\ReShade.ini`, as a path joined to its base path, on the first DXGI export call. It resolves every DXGI export from that library.
- If the library fails to load, ReShade only logs the failure. Its `CreateDXGIFactory1` then calls a null trampoline, and the game crashes at start.
- In its default proxy mode it wraps the factory it gets from us. It then calls `_orig->CreateSwapChainForHwnd` with the unwrapped device and with its own thread-local `g_in_dxgi_runtime = true`, and wraps the swap chain returned to it.
- Its `D3D12CreateDevice` hook passes devices through unwrapped only while `g_in_dxgi_runtime` is set on the calling thread.
- It skips swap chains created on a command queue that is not its own proxy.
- Its effect runtime creates both a UNORM and a UNORM_SRGB render target view on buffer 0. If either fails, the runtime does not initialise.

**Streamline 2.14.1** supports this topology: a D3D11 game, a D3D11-facing proxy, and a D3D12 swap chain created through Streamline with manual hooking. [alandtse/open-shaders](https://github.com/alandtse/open-shaders) ships the same design for Skyrim. A swap chain created through Streamline's proxy factory is already a Streamline proxy. `slDLSSGSetOptions` takes effect at the next `Present`.

**CSP Lua apps** can write a named section with `ac.writeMemoryMappedFile(name, layout, persist)`. CSP calls `CreateFileMappingW(PAGE_READWRITE)` on `Local\<name>` and then maps it for read and write. `ac.connect` cannot be reached from native code.

## 5. Architecture

```
acs.exe (D3D11)
 └─ CSP (dwrite.dll) ── CreateSwapChainForHwnd ──> ReShade dxgi.dll (wraps factory and swap chain)
                                                     └─ real DXGI factory (vtable slots patched by us)
                                                          └─ ProxySwapChain (ours, D3D11-facing)
                                                               ├─ GetBuffer(0): buffer 0 of a hidden D3D11 swap chain (never presented)
                                                               └─ Present ──> copy to a shared texture ──> D3D12Presenter
 CSP DLSS call ──> _nvngx.dll EvaluateFeature (hooked) ──> NgxCapture: depth + MV copied to shared capture slots
 CSP Lua app ──> Local\AcDlssg.Camera.v1 ──> CameraChannel
 CSP Lua app window <── Local\AcDlssg.Status.v1 <── bootstrap, FactoryHook, D3D12Presenter
 CSP Lua app window ──> Local\AcDlssg.Control.v1 ──> D3D12Presenter (start of every Present)
 D3D12Presenter: shared-fence wait ─> copy to SL chain ─> tags + constants ─> SL proxy Present ─> DLSS-G
```

One DLL sits in the game folder. In ReShade mode it is `ac-dlssg.dll`, ReShade's `ProxyLibrary`; it registers no ReShade add-on in v1, because the panel is the CSP Lua app's window in both modes (6.9). In standalone mode the same DLL is installed as `dxgi.dll`, and the process binds `dxgi.dll` to the bridge directly, including the imports of `d3d11.dll`, `d3d12.dll` and the Streamline DLLs. The diagram above shows ReShade mode; in standalone mode the ReShade layer is absent. The bridge tells the modes apart by its own module file name and logs the mode. Runtime data lives in `<game>\ac-dlssg\`:
- `sl\` holds the pinned Streamline files;
- `logs\` holds the logs;
- `install\` holds the install manifest, backups and the uninstaller;
- `ac-dlssg.ini` is the configuration.

## 6. Components

Each unit is a separate source file pair with a narrow interface.

### 6.1 DxgiExports (`dxgi_exports.cpp`)
- **Exports.** The DXGI names ReShade resolves, plus every `dxgi.dll` export that `d3d11.dll`, `d3d12.dll` and the Streamline DLLs import by name or ordinal, because in standalone mode they bind to the bridge:
  - `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2`;
  - `DXGIGetDebugInterface1`, `DXGIDeclareAdapterRemovalSupport`, `DXGIDisableVBlankVirtualization`, `DXGIReportAdapterConfiguration`, `DXGIDumpJournal`;
  - `CompatValue`, `CompatString`;
  - the `DXGID3D10*` entry points.
- **Forwarding.** Each export forwards to `System32\dxgi.dll`, which is loaded by absolute path.
- **Forbidden:**
  - A static import of `dxgi.dll`, and PE export forwarders.
  - Any export named like a ReShade export (`ReShadeVersion`, `ReShadeRegisterAddon`, ...).
  - Any import that can fail to load. ReShade has no System32 fallback for a ProxyLibrary that fails to load, and the game then crashes on the first `CreateDXGIFactory1`. So:
    - the CRT is linked statically (`/MT`, `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`);
    - the import table names only DLLs that exist in System32 on every Windows 10/11 x64 installation (KERNEL32, USER32, ADVAPI32, OLE32 and similar);
    - it never names `VERSION.dll`, which would resolve to the spoof's `version.dll` in the game folder;
    - `d3d11`, `d3d12`, `d3dcompiler` and Streamline are resolved at runtime.
  - `DllMain` doing anything beyond storing the module handle. It always returns TRUE.
- **Re-entrancy.** A thread-local guard, `t_internal_call`, turns every export into a pure pass-through while our own code calls DXGI, D3D12 or Streamline. This is needed because Streamline's `LoadLibraryW(L"dxgi.dll")` resolves to ReShade, which calls back into us, or, in standalone mode, to the bridge itself.
- **First factory creation** (outside `DllMain`) triggers `Bootstrap` (6.10) and `FactoryHook::Install`.

### 6.2 FactoryHook (`factory_hook.cpp`)
- **What is patched.** On the first real factory, the DXGI factory class vtable slots `CreateSwapChain` (10), `CreateSwapChainForHwnd` (15), `CreateSwapChainForCoreWindow` (16) and `CreateSwapChainForComposition` (24). The patch is class-wide, so it also covers factories reached through `adapter->GetParent`.
- **What gets proxied.** `CreateSwapChainForHwnd` only, and only when all of these hold:
  1. the device is a D3D11 device;
  2. the window is AC's main window (class `acsW`);
  3. `Bootstrap` reports DLSS-G as possible;
  4. `Compatibility` (6.10) reports no refusal;
  5. Streamline has not been shut down in this process.
- **Everything else passes through unchanged:** D3D12 queues, legacy `CreateSwapChain`, secondary windows, and our own calls under `t_internal_call`.
- **Failures.** Any failure inside the hook releases partial objects, calls the original and logs.

### 6.3 ProxySwapChain (`proxy_swapchain.cpp`)
A complete `IDXGISwapChain4` COM object. It has all 41 vtable slots and its own atomic reference count. `QueryInterface` answers only `IUnknown`, `IDXGIObject`, `IDXGIDeviceSubObject` and `IDXGISwapChain` 1 to 4. All methods are serialised by one proxy lock.

- **Buffers and descriptions.**
  - `GetBuffer(0, riid)` returns buffer 0 of the hidden D3D11 swap chain (6.4). Any other index returns `DXGI_ERROR_INVALID_CALL`.
  - `GetCurrentBackBufferIndex` returns 0.
  - `GetDesc`, `GetDesc1` and `GetFullscreenDesc` report the game's values: its buffer count, format, flags and windowed state.
- **Present and Present1.**
  - **Test presents.** A call whose flags contain `DXGI_PRESENT_TEST` is not a frame. It does no copy, no fence signal, no capture pairing, no Streamline call and no marker, and it does not advance the frame counter or release the latency semaphore. It returns:
    - the last device error recorded by a real Present, if there is one;
    - otherwise `DXGI_STATUS_OCCLUDED`, if the last real Present returned it and the game window is still minimized (`IsIconic`);
    - otherwise `S_OK`.
  - **All other calls** run section 7 and return the real HRESULT, including `DXGI_STATUS_OCCLUDED`.
  - **Flags.** `DXGI_PRESENT_DO_NOT_WAIT` is removed before the D3D12 Present. `Present1` ignores dirty rectangles.
- **Frame latency.** `GetFrameLatencyWaitableObject`, `SetMaximumFrameLatency` and `GetMaximumFrameLatency` are emulated.
  - **The semaphore.** It is created with `CreateSemaphoreW(nullptr, L, 16, nullptr)`, where `L` is the current latency. `L` is DXGI's default of 1 until CSP calls `SetMaximumFrameLatency`, and `max_frame_latency` from the config replaces CSP's value only when that key is set. The initial count `L` lets CSP's wait before the first frame return at once.
  - **Changing the latency.** `SetMaximumFrameLatency(n)` clamps n to 1..16. If n > L, it releases n − L counts. If n < L, it records L − n future releases to swallow.
  - **Handles.** `GetFrameLatencyWaitableObject` returns a new `DuplicateHandle` copy on every call, because the caller owns the handle and closes it. The proxy closes its own handle in the final Release.
  - **Releases.** Every non-test Present releases one count on every return path, including failures and frames without DLSS-G. The internal Present of a resize (below) releases none.
  - **Chain without the flag.** If the game's description lacks `FRAME_LATENCY_WAITABLE_OBJECT`, `GetFrameLatencyWaitableObject` returns NULL, and the two latency setters and getters return `DXGI_ERROR_INVALID_CALL`.
  - **Streamline's pacer.** From M2 on, the D3D12 chain is never created with the waitable flag, so Streamline's pacer is never starved.
  - **M1 only (no Streamline yet).** When the game's chain has the waitable flag, the D3D12 chain gets it too, follows the game's latency, and the presenter waits on it (500 ms at most) after each D3D12 Present, so CSP keeps its own queue depth. DXGI creates that object with a count of L, which pays for the game's wait before its first frame; since the game's waits are served by the bridge's semaphore, the presenter takes that first count at creation. Without it the bridge queued one frame more than the game's own chain (one refresh of extra latency with VSync). M2 removes this when Reflex and Streamline pace frames.
- **Resizing.** `ResizeBuffers` first normalizes zero arguments, then:
  1. If DLSS-G is on, it sets `eOff` and null tags, then presents the current frame once more through section 7 steps 4 to 7, with its own frame token and the full marker sequence. The `eOff` only takes effect at a Present, and until then Streamline presents on its own thread. This internal Present is not a CSP frame: it does not release the latency semaphore and is not counted in the statistics.
  2. It flushes the D3D11 context and CPU-waits up to 500 ms until the `progress` fence (6.4) reaches the last submitted value. On timeout it takes the stall path (section 9).
  3. It releases the proxy's reference to the hidden buffer and every D3D12 back-buffer reference. It resizes the hidden D3D11 chain and the Streamline chain (with our own buffer count and flags), re-fetches the buffers, calls `GetCurrentBackBufferIndex` on the Streamline proxy, and recreates the shared back-buffer texture when the size or format changed.

  DLSS-G is switched back on by the next regular Present, never inside `ResizeBuffers`, and only if the aspect check (6.10) passes. `SetFullscreenState` and `ResizeTarget` run steps 1 and 2 before they are forwarded to the Streamline proxy. `ResizeBuffers1` returns `DXGI_ERROR_INVALID_CALL`.
- **Forwarded to the Streamline chain:** `GetFullscreenState`, `GetContainingOutput`, frame statistics, colour-space queries, HDR metadata, source size and matrix transform, rotation, and background colour.
- **Private data.** `SetPrivateData`, `GetPrivateData` and `SetPrivateDataInterface` use our own store, because ReShade keeps its wrapper pointer and `SKID_SwapChainColorSpace` there.
- **Final Release.**
  1. `slDLSSGSetOptions(eOff)` and null tags.
  2. `SetFullscreenState(FALSE)` on the Streamline proxy, if it is in fullscreen state.
  3. Signal the D3D12 queue and wait for it; CPU-signal `pendingWait` (6.4).
  4. `slShutdown()`.
  5. Release the back-buffer references, command lists and allocators, the Streamline proxy chain, the queue, the fences, the shared textures, the hidden D3D11 chain and its window, and the D3D12 device.

  After `slShutdown`, `StreamlineRuntime` is closed for the rest of the process, and `FactoryHook` passes any later `CreateSwapChainForHwnd` through and logs why.

### 6.4 D3D12Presenter (`d3d12_presenter.cpp`)
- **Where objects are created.** The D3D12 device, queue, fences, Streamline swap chain, hidden D3D11 chain and shared back buffer are all created inside `FactoryHook`'s `CreateSwapChainForHwnd`, on the calling thread. There ReShade has `g_in_dxgi_runtime` set, so its `D3D12CreateDevice` hook does not wrap our device. If a ReShade wrapper is detected anyway, the device is unwrapped with `QueryInterface(IID_UnwrappedObject)`. We declare that IID ourselves as `{7F2C9A11-3B4E-4D6A-812F-5E9CD37A1B42}`, because it lives in ReShade's `source/com_utils.hpp`, not in `include/`; our `t_internal_call` guard does not prevent ReShade's wrapping. Resources that are later opened with `OpenSharedHandle` on this unwrapped device may be created on other threads. That covers the capture slots and resized back buffers.
- **Creation order** (follows open-shaders):
  1. `D3D12CreateDevice` on the adapter with the same LUID as CSP's D3D11 device, feature level 12_0.
  2. `slSetD3DDevice(native)`.
  3. `slIsFeatureSupported(kFeatureDLSS_G, adapterInfo)`.
  4. `slUpgradeInterface(&device)`.
  5. A direct command queue created from the upgraded device.
  6. The factory from `adapter->GetParent`, passed through `slUpgradeInterface(&factory)`.
  7. `CreateSwapChainForHwnd(queue, hwnd, desc1)` on that factory. `desc1` uses `FLIP_DISCARD`, `R8G8B8A8_UNORM` and `BufferCount = 3`, adds `ALLOW_TEARING` when `IDXGIFactory5::CheckFeatureSupport` reports it, and never sets the waitable flag.
  8. No `slUpgradeInterface` on the swap chain. The chain is already a Streamline proxy, and under manual hooking `slUpgradeInterface` on it returns `eErrorInvalidIntegration`. Instead, `slGetNativeInterface(chain, &native)` checks it, and the extra reference that call adds is released. If `native == chain`, the chain is not proxied, and that counts as a creation failure.
  9. `MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER)`.
  10. Frame start for token 1 (section 7 step 1).

  Every hooked call goes through the Streamline proxy: `Present`, `Present1`, `GetBuffer`, `ResizeBuffers`, `GetCurrentBackBufferIndex` and `SetFullscreenState`. `GetCurrentBackBufferIndex` is called on the proxy every frame; it picks the copy target and the command allocator.
- **D3D11-facing back buffer.** ReShade needs both a UNORM and a UNORM_SRGB render target view on buffer 0, and CSP creates its view with a NULL description. Direct3D 11 allows both only on a real swap-chain buffer. So buffer 0 comes from a hidden D3D11 flip-model swap chain, which is never presented:
  - It is created on the device our hook received (ReShade passes the unwrapped device), through the native factory and under `t_internal_call`.
  - It uses the game's width, height, format, `BufferUsage` and `SampleDesc`.
  - Its window is created and pumped by a thread the bridge owns, and it gets `MakeWindowAssociation(DXGI_MWA_NO_WINDOW_CHANGES)`.

  If the hidden chain cannot be created, the bridge refuses (no proxy). The earlier fallback, a `R8G8B8A8_TYPELESS` texture whose views a ReShade add-on's `create_resource_view` handler would fix up, left with the ReShade panel (6.9).
- **Shared back buffer.** One `R8G8B8A8_UNORM` texture sized like the swap chain. It is created on D3D11 with `D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE`, shared with `IDXGIResource1::CreateSharedHandle`, and opened with `ID3D12Device::OpenSharedHandle`. On this machine, textures created on the D3D12 side and opened on D3D11 failed with `E_INVALIDARG`. At Present, the immediate context copies the hidden buffer into it (section 7 step 4). `ResizeBuffers` recreates only this texture. After each D3D12 copy, D3D11 waits on the shared fence before CSP renders the next frame. A ring of back buffers is a later optimization (section 15).
- **Capture slots.** A ring of 3. Each slot holds one depth texture (`R32_FLOAT`, UAV) and one motion-vector texture (the source's typed format), shared in the same way as the back buffer.
  - **Size.** Each texture is sized from its own source texture's `D3D11_TEXTURE2D_DESC` Width and Height. The back buffer and the NGX Width and Height scalars never set the size.
  - **When created.** Lazily, on the render thread, at the first counted evaluate.
  - **When recreated.** Before every copy, the bridge compares the source descriptions with the slot's. On any difference, and after every counted `CreateFeature`, it puts DLSS-G in `eOff` for that frame and sets null tags. It waits until `progress` has passed the last frame that tagged the old slots, then releases and recreates them.
  - **Tag extents** are `{0, 0, DLSS.Render.Subrect.Dimensions.Width, .Height}`, which may be smaller than the slot texture.
- **Fences.**
  - One `D3D12_FENCE_FLAG_SHARED` fence, opened on D3D11 with `ID3D11Device5::OpenSharedFence`. Its values come from one monotonic counter starting at 1. The D3D11 Present signal and the D3D12 signals take values from it. The capture does not signal it (M3 review finding): a capture value is always higher than the value D3D11 waits for at the next Present, so a capture signal would release CSP's queue before the D3D12 copy of the previous frame finished. The capture copy is recorded on the same immediate context before the Present's copy and signal, so the D3D12 queue's wait for the Present's value V also covers the capture.
  - A second, non-shared D3D12 fence, `progress`. The D3D12 queue signals it with the same value right after every signal of the shared fence.
  - Allocator retirement, the drains in `ResizeBuffers` and Release, and stall detection read `progress`, never the shared fence, because the shared fence can be advanced from the CPU.
- **Command recording.** There is one command allocator and one command list per D3D12 back buffer. A frame's allocator is reset only after `progress` passes that frame's value.
- **Stall release.**
  - The bridge keeps `pendingWait`, the highest value any `ctx4->Wait` has been issued for. It keeps `pendingWait` and the last submitted value as atomics.
  - **Watchdog.** A thread that starts with the presenter checks every 100 ms. It calls only `GetCompletedValue`, `GetDeviceRemovedReason` and `ID3D12Fence::Signal`. If `progress` has not advanced for 500 ms while `progress < pendingWait`, it CPU-signals `sharedFence12->Signal(pendingWait)` and sets an atomic `stallPending` flag. That signal releases CSP's D3D11 queue. The watchdog does nothing else, because `slDLSSGSetOptions` must be called on the presenting thread.
  - **Stalled mode.**
    - **Entry.** The next Present sees `stallPending`, logs the stall, sets DLSS-G to `eOff` and enters stalled mode. The start of every Present, and every CPU wait on the render thread (500 ms limit), run the same check and take the same path directly.
    - **Behaviour.** In stalled mode, Present issues no `queue->Wait` and `ctx4->Wait` pair, and delivers nothing through D3D12 until `progress` reaches the last submitted value. Every value allocated afterwards is greater than `pendingWait`.
    - **Giving up.** After 4 s without progress, or on device removal, the presenter stops, and Present returns `DXGI_ERROR_DEVICE_HUNG` or `DXGI_ERROR_DEVICE_REMOVED`.
  - **Before stopping.** Every path that stops the D3D12 side (disable, device removed, Release, a failed `ResizeBuffers`) first CPU-signals `pendingWait`.

### 6.5 NgxCapture (`ngx_capture.cpp`)
- **Hooked entry points.** `NVSDK_NGX_D3D11_CreateFeature`, `NVSDK_NGX_D3D11_EvaluateFeature` and `NVSDK_NGX_D3D11_EvaluateFeature_C`, in every loaded module that exports them. Modules are found by enumerating the process modules and looking the exports up with `GetProcAddress`.
- **Skipped.** The host exe, anything under `\NGX\models\`, filler stubs (0x90 or 0xCC), and modules where two exports share one address. E9 thunks are followed within the module.
- **Late-loaded and unloaded modules.** An `LdrRegisterDllNotification` callback handles snippets that load later. On load, it triggers a non-blocking rescan (`TryEnterCriticalSection`, finished from the render thread). On unload, it drops that module's hooks without writing to its memory. The callback never blocks, because it runs under the loader lock.
- **Which calls count.**
  - **Nesting.** A thread-local nesting counter treats only the outermost call as CSP's.
  - **Forwarding.** The original is always called first and its result is returned unchanged. CSP's own DLSS output is never affected.
  - **Feature filter.** `CreateFeature` records the feature ID and the create parameters for each returned `NVSDK_NGX_Handle*`. A successful create at a known address replaces the old record, because NGX reuses handle addresses. Only handles created as `NVSDK_NGX_Feature_SuperSampling` (1), whose parameter block has no denoiser keys, are captured. Evaluates on any other handle, such as CSP's DLSS-NR feature, are forwarded untouched.
  - **Unobserved creates.** A handle whose create was not observed counts only if its evaluate block returns Success for `Depth`, `MotionVectors` and `DLSS.Feature.Create.Flags`. Its sizes and flags are then read from that block.
  - **Several evaluates per frame.** If more than one qualifying evaluate happens between two Presents, the last one wins and the frame is counted as a double evaluate in the statistics.
- **Parameters read.** They are read through our own declaration of the `NVSDK_NGX_Parameter` interface, in NVIDIA's method order. There are no NVIDIA headers in the repository.
  - At create: `Width`, `Height`, `OutWidth`, `OutHeight` and `DLSS.Feature.Create.Flags`.
  - At evaluate:
    - `Depth` and `MotionVectors`;
    - `Jitter.Offset.X/Y` and `MV.Scale.X/Y`;
    - `DLSS.Render.Subrect.Dimensions.Width/Height`, falling back to `Width`/`Height`;
    - `Reset`.
  - Any result other than `NVSDK_NGX_Result_Success` counts as "not set". Resource pointers are read again on every evaluate, because CSP ping-pongs its motion-vector textures.
- **Copying into slot `N mod 3`.** This runs on the same immediate context, after the original call.
  - Motion vectors are copied with `CopyResource` into the slot's texture of the same typed format.
  - Depth is copied with a precompiled `cs_5_0` blit, from the typed SRV of the source into the slot's `R32_FLOAT` UAV. The SRV is `R32_FLOAT` for `R32_TYPELESS` or `D32_FLOAT` sources, and `R24_UNORM_X8_TYPELESS` for `R24G8_TYPELESS` sources. The compute shader, SRV slot 0 and UAV slot 0 are saved and restored.
  - No fence is signalled here (6.4 "Fences"). The slot, the evaluate parameters and the latched camera snapshot (6.6) pair with the next non-test Present, whose signal V covers the copy.
- **Deferred contexts.** If the context is not immediate, capture is disabled and logged.

### 6.6 CameraChannel (`camera_channel.cpp`) and the Lua app (`apps/lua/AcDlssg/`)
- **The section.** At bootstrap the DLL calls `CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, L"Local\\AcDlssg.Camera.v1")`.
  - It must be `PAGE_READWRITE`, because CSP opens the existing object and maps it for read and write.
  - The DLL itself maps only a `FILE_MAP_READ` view.
  - 4096 bytes is larger than the Lua layout. An existing section keeps its size when CSP opens it.
- **The Lua app.** `manifest.ini` sets `[CORE] LAZY = NONE`, so CSP loads the app with AC and runs it until AC closes, whether or not its settings window (6.9) is open. The camera writer of the main file:
  - opens the section with `ac.writeMemoryMappedFile('AcDlssg.Camera.v1', LAYOUT, true)` and keeps a global reference;
  - writes from `render.onSceneReady` (before the main render), with `script.update` as a fallback;
  - writes `pos` from `ac.getSim().cameraPosition`, which is in world space.
- **Layout.** A C struct-body string, mirrored by a C++ struct with `static_assert` on `sizeof` and `offsetof`:

  ```
  uint32_t magic; uint32_t version; uint32_t seq; uint32_t frame;
  float pos[3]; float fwd[3]; float up[3]; float side[3];
  float fovVDeg; float clipNear; float clipFar; float originShift[3];
  float renderW; float renderH; uint32_t flags; float dt; double simTimeMs;
  ```

  Flag bits: 1 camera jumped, 2 paused, 4 replay, 8 VR, 16 triple screen, 32 in main menu, 64 write failed.
- **Seqlock.** The writer and the reader both run on CSP's render thread, so the reader never waits on the writer.
  - **Writer.** It forces parity instead of incrementing blindly. It sets `seq = bit.bor(seq, 1)` (odd), calls `ac.memoryBarrier()`, writes the fields inside `pcall`, calls `ac.memoryBarrier()` again, then sets `seq = seq + 1` (even). If the `pcall` fails, it still makes `seq` even and sets flag 64.
  - **Reader.** It makes at most 2 attempts. If `seq` is odd, changes during the copy, or flag 64 is set, the camera counts as missing for this frame.
- **Freshness.** At every captured evaluate, the reader latches the snapshot and records its `frame`. The snapshot is fresh only if its `frame` is greater than the `frame` latched for the previous captured frame, which means the Lua app wrote during this frame. There is no multi-frame tolerance. A frame without a fresh snapshot is presented with DLSS-G `eOff` and null tags. The 30-frame rule in section 9 only decides when the panel reports the camera as missing.

### 6.7 FrameConstants (`frame_constants.cpp`, pure math, unit-tested)
Builds `sl::Constants` from the snapshots of bridge frames N and N−1 and the capture parameters. Every non-optional field is set, and every `sl::Boolean` field is set explicitly to `eTrue` or `eFalse`.

- **Space and precision.** Both views are built in one common world space from `pos`, `side`, `up` and `fwd`. `originShift` is logged only, so an origin re-base cannot appear as camera motion. If M3 shows that `pos` is shifted, each frame is un-shifted with its own `originShift` before use. `viewToViewPrev` is computed in double precision and camera-centred, as in Streamline's `calcCameraToPrevCamera`. The previous pose is always frame N−1's snapshot.
- **Matrices.** Row-major, row-vector convention, no jitter in the matrices.
  - `cameraViewToClip` is a standard, non-reversed D3D perspective, built from `fovY = fovVDeg·π/180`, `aspect = renderW/renderH`, `clipNear` and `clipFar`. `clipToCameraView` is its inverse.
  - `clipToPrevClip = clipToCameraView · viewToViewPrev · viewToClipPrev`, and `prevClipToClip` is its inverse.
- **Camera fields.**
  - `cameraPos`, `cameraUp`, `cameraRight` and `cameraFwd` are the world-space values, re-orthonormalized so that Streamline's scene-change detector stays enabled.
  - `cameraNear = clipNear`, `cameraFar = clipFar`.
  - `cameraFOV = fovVDeg·π/180`: vertical, in radians, as Streamline expects.
  - `cameraAspectRatio = renderW/renderH`.
- **Motion vectors and depth.**
  - `jitterOffset` comes from NGX, in pixels.
  - `mvecScale = MV.Scale / renderSize`, which is `{-1,-1}` for CSP.
  - `depthInverted` and `motionVectorsJittered` come from the create flags.
  - `cameraMotionIncluded = eTrue`, `motionVectors3D = eFalse`.
- **Reset.** `reset = eTrue` when the NGX `Reset` is set, when the camera-jumped flag is set, or when frame N−1 was not presented with DLSS-G inputs. That last case covers no capture, no fresh camera, a resize, a toggle and a stall.
- **Validation.** Handedness and the sign of `side` are validated at runtime with a debug overlay in M3. The matrix code has a single flag that flips them.

### 6.8 StreamlineRuntime (`streamline_runtime.cpp`)
- **Loading.**
  - It loads `ac-dlssg\sl\sl.interposer.dll` by absolute path, after `sl::security::verifyEmbeddedSignature` succeeds.
  - It resolves the `sl*` exports with `GetProcAddress`.
  - It resolves the feature functions with `slGetFeatureFunction` after `slSetD3DDevice`: `slDLSSGSetOptions`, `slDLSSGGetState`, `slReflexSetOptions`, `slReflexSleep` and `slPCLSetMarker`.
- **What runs.** Streamline plugins (`sl.*.dll`) load only from `ac-dlssg\sl\`, except when an override described below is active. The NGX snippet is not pinned: NGX also searches the application folder (the game root) and its model store in `%ProgramData%\NVIDIA\NGX\models`, whatever `pathsToPlugins` and the OTA flags say. On RTX 30, the spoof also redirects `nvngx_dlssg.dll` to its bundled runtime. After `slSetD3DDevice`, and again when DLSS-G first reports active, the bridge logs the full path and file version of every loaded `sl.*.dll` and `nvngx_dlssg*` module, and shows any that sit outside `ac-dlssg\sl\` in the panel.
- **`slInit`.** It runs in `Bootstrap`, never in `DllMain`.
  - Flags: `eUseManualHooking | eUseFrameBasedResourceTagging | eDisableCLStateTracking`. `eAllowOTA` and `eLoadDownloadedPlugins` are not set.
  - Features: `kFeatureDLSS_G`, `kFeatureReflex` and `kFeaturePCL`.
  - Paths: `pathsToPlugins` and `pathToLogsAndData` point at our folders.
  - `renderAPI = eD3D12` and `engine = eCustom`.
  - A fixed `projectId` GUID and `engineVersion` set to the bridge version. Both must be non-empty, or production Streamline disables NGX features.
  - Streamline's log goes to our log through the log callback.
- **Reflex.** `slReflexSetOptions({mode = eLowLatency})` is called once when `D3D12Presenter` is created, after the feature functions are resolved, and again only when the options change.
- **DLSS-G mode.** `slDLSSGSetOptions(viewport 0, {eOn or eOff, numFramesToGenerate = multiplier - 1, flags = eRetainResourcesWhenOff})` is called on the presenting thread, and only when the mode changes or, while on, the size hints or the number of frames to generate change. The hotkey toggles `eOn` and `eOff` without recreating the swap chain. Issue #598 of dlssg_for_sm86 reports crashes on an RTX 3080 after repeated DLSS-G feature re-creation.
- **Multi frame generation.** The multiplier is 2X, 3X or 4X (`fg_multiplier`, default 2; `D3D12Presenter::SetFgMultiplier` at runtime, for the panel).
  - **Streamline's maximum.** `sl.dlss_g` 2.14.1 sets `DLSSGState::numFramesToGenerateMax` at plugin startup, from NGX's `DLSSG.MultiFrameCountMax` capped at 5, and to 1 with the log line "NGX parameter indicating multi-frame support not found or invalid" when NGX does not report it (RTX 40). `slDLSSGSetOptions` refuses a `numFramesToGenerate` above the maximum, and 0. Verified in the 2.14.1 binary with radare2 and Ghidra.
  - **When it is read.** `slDLSSGGetState` without options (cheap) before the first options carry a count, which is before the first `eOn`, and again after every new request, before the next options or the next video memory check. The answer is logged: "fg: Streamline allows up to <m>X (numFramesToGenerateMax <k>)".
  - **The clamp.** A request above the maximum uses the maximum; a maximum of 0 (also a failed query) or 1 means 2X. It is logged once per request, "fg: <n>X requested, Streamline allows up to <m>X; using <m>X", and is the status reason of the multiplier.
  - **A runtime change** is applied on the presenting thread at the next frame, with the rules of a toggle: the next DLSS-G frame has `reset`, a failure status is retried, the options are re-sent only when the count changes (at once while on; while off the next `eOn` carries it), and the swap chain is never recreated. `eRetainResourcesWhenOff` stays. Whether Streamline re-creates its DLSS-G feature for a new count is not known; with issue #598 in mind the count is never changed without a request.
  - **Logs.** "fg: multiplier <a>X -> <b>X requested"; "fg: DLSS-G options: <m>X (numFramesToGenerate <k>)" at the first `eOn` of each new count; the first DLSS-G Present after it logs the raw "numFramesActuallyPresented", so that its reading is confirmed at every multiplier in game. `generated` in the statistics line stays numFramesActuallyPresented - 1 per DLSS-G Present, whatever the multiplier.
- **Status.** `slDLSSGGetState(viewport, state, nullptr)` is polled every 60 frames. Any failure status turns DLSS-G off and shows it in the panel, for example `eFailResolutionTooLow`, `eFailReflexNotDetectedAtRuntime`, `eFailCommonConstantsInvalid`, `eFailGetCurrentBackBufferIndexNotCalled` or `eFailHDRFormatNotSupported`. The poll also reads `bIsVsyncSupportAvailable` (section 7 step 6).
- **Driver profile.** Before any device exists, NVAPI DRS reads two settings of the `acs.exe` profile and of the global profile:
  - `0x10308298` (the NVIDIA App's DLSS override): when set to 1, DLSS-G silently never interpolates;
  - `0x10E41E06` (`SL_DLSS_OVERRIDE`): when on, production Streamline loads override plugins from the NGX cache ahead of ours.

  Both are logged, and the panel says how to clear them. The bridge never changes driver settings itself. DRS reads do not depend on the spoof's architecture redirect.

### 6.9 Panel: the CSP Lua app's settings window (`panel_status.cpp`, `panel_control.cpp`, `apps/lua/AcDlssg/`)
One UI in both modes: a normal CSP app window titled "AC DLSS-G", drawn by the Lua app of 6.6. The ReShade overlay panel is dropped for v1, and the bridge registers no ReShade add-on. The window is drawn into the game frame, so with DLSS-G on it can show interpolation artifacts like CSP's other app windows (section 3).

- **Two sections.** The bootstrap creates both with `CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, name)`, whether or not the bridge is enabled, so that the window can say why it does nothing:
  - `Local\AcDlssg.Status.v1`: the bridge writes it through a `FILE_MAP_WRITE` view; the Lua app reads it with `ac.readMemoryMappedFile`.
  - `Local\AcDlssg.Control.v1`: the Lua app writes it with `ac.writeMemoryMappedFile`; the bridge reads it through a `FILE_MAP_READ` view.

  Both layouts are C struct bodies in the Lua file, mirrored by C++ structs with `static_assert` on `sizeof` and every `offsetof` (`src/panel_status.h`), and a test compares the Lua strings with the structs field by field. Both records are seqlocks as in 6.6: seq odd while the writer writes, parity forced, and every counter kept below 2^31.
- **Ownership.** The section names are per logon session. A status section that already holds a stable record of another process's bridge (a test app next to the game) is never written, and that process reads no control requests; `ownerPid` names the owner.
- **Status record.**
  - `magic`, `version`, `seq`, `heartbeat` (+1 per publish) and `ownerPid`;
  - `bridgeState`: 0 not loaded (waiting for the game's swap chain), 1 pass-through, 2 proxied without DLSS-G, 3 DLSS-G available; `stateReason` (256 chars) says why it is below 3;
  - `fgOn` (the mode Streamline has), `fgUserOn` (the user's switch), `fgPaused` (the mode is on, but in the last statistics second DLSS-G was on for at least half of the Presents and Streamline generated no frame: it pauses interpolation while the game window is not focused) and `reason` (160 chars): "on", or why DLSS-G is off. The user's switch comes first ("off by the user (panel)", naming what switched it off), then the gate's reason of the last frame (7 step 5.4);
  - the mode (ReShade or standalone), the render GPU's name, whether the spoof is loaded and whether the GPU is an RTX 30 (SM86);
  - base fps, presented fps (generated frames included), the bridge's GPU time, the render adapter's video memory usage and budget in MiB, and captures, fresh camera snapshots and tagged frames per second;
  - the VSync-with-DLSS-G note (7 step 6), and the first driver-profile warning (6.8) as a flag and a text;
  - the current `camera_flip_handedness` and `camera_negate_side`, `start_with_fg`, the counter of the last request applied, the counter and the result of the last save, the hotkey text and the bridge version;
  - appended after those, so that every earlier field keeps its offset and a window and a bridge of different builds still agree on them (an older bridge leaves the new fields zero in its 4096-byte section, and the window then hides the multiplier buttons): `fgMultRequested` (`fg_multiplier`, then the panel), `fgMultUsed` (what the DLSS-G options carry), `fgMultMax` (the highest multiplier `numFramesToGenerateMax` allows, 0 until Streamline was asked) and three 160-char texts: `fgMultNote` (why a lower multiplier is used: the clamp and the video memory fallback, 6.8 and 6.11), `vramNote` (the video memory guard's note, 6.11: "video memory is tight ..." or "not enough video memory ...", empty when it fits) and `restartNote` (what applies only after a restart); then `autoFixNote` (384 chars, since the note of an `ac-dlssg.ini` that could not be saved names the file's path): a setting the bridge changed by itself so that frame generation can run, for the rest of the session (a blocking `fg_vram_headroom_mib` switched to `auto`, 6.11), empty otherwise. It is not `restartNote`, because the fix needs no restart and a later "Save as default" would replace it.
- **Who publishes.**
  - The bootstrap: the mode, the version, the config's switches and hotkey, the spoof and the driver warning, with state 0, or state 1 and the reason when the bridge is not possible.
  - `FactoryHook`: state 1 with the pass-through reason (or the failed creation's error) and the render GPU, for the main window.
  - `D3D12Presenter`: state 2 or 3 at creation, then after every statistics line (once per second), at once at the end of every frame whose DLSS-G mode or user switch changed, and after every applied request. The final release publishes state 1, "the game's swap chain was released". While a presenter runs the heartbeat advances at least once per second.
- **Control record.** `magic`, `version`, `seq`, `requestCounter` (+1 per user action, 1 to 0x7FFFFFFF; a reloaded app continues the counter it finds), the desired DLSS-G switch, the desired `camera_flip_handedness` and `camera_negate_side` (the whole desired state, not only the switch clicked), `saveAsDefault` and, appended, `desiredMultiplier` (2 to 4 asks for that multiplier; 0, which an older window leaves, keeps the current one; the window sends the multiplier it shows with every request).
- **Applying a request.** At the start of every `PresentFrame` the presenter compares the record's seq with the last one it saw, one load. When it changed, it reads the record (a torn read is retried at the next frame). A request is new when its counter differs from the last one applied; the counter found at presenter creation is the baseline and is never applied. A new request:
  - sets the DLSS-G switch exactly as the hotkey does (section 8): `fg: panel -> on|off`, the next DLSS-G frame has `reset = eTrue`, and a failure status is retried;
  - sets the camera switches, which apply from that frame on (`panel: camera_flip_handedness 0 -> 1`), with reset on the next DLSS-G frame;
  - with a `desiredMultiplier` of 2 to 4 that differs from the current request, calls `D3D12Presenter::SetFgMultiplier` and applies it in the same frame (6.8: `fg: multiplier 2X -> 3X requested`, the rules of a toggle), so that the status published for the request already shows it;
  - with `saveAsDefault`, writes `start_with_fg`, both camera switches and `fg_multiplier` into `ac-dlssg.ini`. The edit is key-level: every other byte, the line endings and trailing comments stay, every line of such a key in `[bridge]` gets the new value, and a missing key is added after the section's last key line. The file is written as `ac-dlssg.ini.new` and renamed over the old one. Success and failure are logged and published. A successful save sets `restartNote` to "Saved. start_with_fg and the other saved switches apply the next time the game starts.", and the note stays for the rest of the session.

  The hotkey and the panel share one state, and the status shows the result. `restartNote` is for any panel change that applies only at the next start; every live switch of v1 (DLSS-G, the multiplier and the camera switches) applies at once, so only the save sets it. The status is also published at the end of a frame that changed the multiplier's request, Streamline's maximum or the video memory guard's outcome or note, and of the frame that switched `fg_vram_headroom_mib` to `auto`.
- **The window.** English only, like the rest of the UI. Its layout (the owner's request of 2026-09-29: a clear, custom-drawn menu in the logo's red and black, with animations):
  - **Look.** Near-black rounded cards (`#0B0B0D`) with hairline edges on CSP's window background, racing red (`#E10600`) as the one accent, white text and two greys; Segoe UI through DirectWrite, Bahnschrift for the big numbers. The logo is `tools/brand/make_logo.py`'s mark: `logo.png` in the header and `icon.png` as the app's `ICON` in `manifest.ini`.
  - **Header.** The logo, "AC DLSS-G" and the bridge's version, and a status pill whose dot pulses: green "Running <m>X" while frame generation runs, amber "Paused", and also amber while running when video memory is tight (`vramNote` set), red "Off", "Unavailable", "Not running" or "Versions differ", amber "Waiting" before a swap chain was decided.
  - **Notes** as cards that fade in, with an accent bar: when `autoFixNote` is set, it first of all, in the "on" colour; when `restartNote` is set, "Restart the game to apply" in blue with the note below it.
  - **The switch.** A card that is one big button, with a toggle whose knob slides and whose track fades to red, a hover highlight, and the hotkey under the title. It is dimmed when DLSS-G is not available, with "Frame generation is unavailable: `stateReason`" under it; otherwise, while frame generation does not run, the status line (on but paused, or off and the reason; grey when the user switched it off, amber else) is under it.
  - **Multiplier.** "Multiplier (using <m>X)" names the multiplier used when it is lower, then 2X, 3X and 4X as one segmented control (hidden while the bridge publishes no multiplier): a red highlight slides to the one asked for; one above `fgMultMax` is dimmed and disabled when the maximum is known, with the hover text "not supported by this GPU/driver"; `fgMultNote` follows in amber.
  - **FPS card.** Real and output fps as big numbers that ease to each new value, the output in red while frames are generated, with chevrons between them that animate while frame generation runs, and a thin bar of the real and the generated share of the output.
  - **Video memory.** Used / budget in MiB and a bar that eases to the value, grey while it fits, amber from 90% of the budget and red above it, with `vramNote` under it in amber (not when it is the status line's reason already, as with `auto`'s "not enough video memory ..."). The VSync note and the driver warning follow as cards when set.
  - **"Save as default"**, a full-width button that warms on hover and sinks while pressed; a successful save shows "Saved as the default in ac-dlssg.ini" in its place for about 2 s, then fades back; a failed save stays under it in red.
  - **"Details"**, collapsed at first and opening smoothly: the GPU with the spoof state, the bridge's version and mode, the bridge's GPU time, the hotkey, the per-second counts, and the two camera switches.
  - While a request is not yet applied, the window shows what was asked for.
  - Below state 2 (pass-through, or waiting for the game's swap chain) a card says so with `stateReason`, and "Details" follows with its switches disabled.
  - With no status record, or when a presenter's heartbeat has not changed for 3 s: a "Bridge not running" card, with a hint where the log is.
  - A status record with our magic but another `version`: a "Bridge and window versions differ" card with both version numbers and to run `install.bat` of one release again; nothing of that record is read and no control is shown. The versions and the section names stay 1 and `.v1` while fields are only appended (an older window reads the fields it knows, a newer window sees zeros from an older bridge); a change that moves or changes an existing field raises both versions.
  - Per frame the window allocates nothing: it compares the status seq and draws texts that are rebuilt only when the status changes; its colours and vectors are made once and rewritten, the animations are numbers eased exponentially with the frame time, the numbers' strings are cached, and a note is measured only when its text or the width changes. Every `ui.*` function and `ui.*` enum member the app uses is checked against the installed CSP's `lua-sdk\ac_apps\lib.lua` by a unit test (skipped where no CSP is installed).
- **Hotkey.** Default Ctrl+F10, configurable, polled on the present thread while the game window has the focus.

### 6.10 Bootstrap, Compatibility, Config and Log (`bootstrap.cpp`, `compat.cpp`, `config.cpp`, `log.cpp`)
- **Bootstrap** runs once, outside the loader lock, on the first `CreateDXGIFactory*` call:
  - it reads the config and opens the log;
  - it detects the GPU from the DXGI adapter `DeviceId`. It does not use NVAPI for this, because the spoof rewrites NVAPI's architecture query;
  - it logs, per adapter, the D3DKMT hybrid type (hybrid discrete, hybrid integrated or neither), the HAGS state, the number of outputs and the driver version. It warns when an NVIDIA driver is older than 581.29 (NVIDIA's Optimus fix) or than R580 (the spoof's native kernels);
  - it detects the spoof: `version.dll` from the game folder, and later `sm86_backend.dll`;
  - it reads the driver profile (6.8), runs `slInit` and creates the camera section;
  - last, whether or not the bridge is enabled, it creates the panel's status and control sections and publishes its part of the status (6.9).
- **Compatibility refusals.** When any of these holds, the bridge does not proxy, and the game runs as without the mod:
  - **Key sources.** Each CSP key is read from `<game>\extension\config\<file>`, overridden by `Documents\Assetto Corsa\cfg\extension\<file>` when the key is present there. AC keys are read from `Documents\Assetto Corsa\cfg\video.ini`.
  - **CSP settings:** `dxgi_tweaks.ini [COMPATIBILITY] OLD_SWAPCHAIN=1` or `EXCLUSIVE_FULLSCREEN=1`; `dxgi_tweaks.ini [HDR] ENABLED=1`; `graphics_adjustments.ini [FSR] ACTIVE≠1` or `OLD_IMPLEMENTATION≠3`.
  - **AC settings:** `video.ini [VIDEO] AASAMPLES>1`, or `video.ini [CAMERA] MODE` anything other than `DEFAULT` (for example OCULUS, OPENVR or TRIPLE).
  - **Aspect mismatch:** `dxgi_tweaks.ini [COMPATIBILITY] ALLOW_STRETCHING=0` (the default) and `|(video.ini WIDTH/HEIGHT) / (swap-chain Width/Height) − 1| > 0.005`, because CSP then letterboxes.
  - **Other add-ons:** `dlss5-bridge.addon64` or `renodx-dlss5.addon64` is loaded.
  - **System:** hardware-accelerated GPU scheduling is off on the adapter of CSP's D3D11 device; CSP's device is not on an NVIDIA adapter; the GPU is unsupported; or the GPU is an RTX 30 without the spoof.
    - HAGS is read with `D3DKMTQueryAdapterInfo(KMTQAITYPE_WDDM_2_7_CAPS)` for that adapter's LUID. The registry value `HwSchMode` can be absent while HAGS is on (the laptop is such a case), so it is only the fallback when the query fails. Streamline's `slIsFeatureSupported` stays the final word for DLSS-G.
    - The non-NVIDIA refusal names the adapter and tells the user to set `acs.exe` to High performance in Windows graphics settings, or to check CSP's `SELECT_ADAPTER`.
- **Runtime-only switches.** The same aspect test is repeated with NGX `OutWidth/OutHeight` after every `ResizeBuffers` and every counted `CreateFeature`. The Lua flags (VR, triple screen) arrive after the swap chain exists. Both can only switch DLSS-G off; the proxy stays.
- **Config.** `ac-dlssg\ac-dlssg.ini`, with the keys `enabled`, `start_with_fg`, `hotkey`, `max_frame_latency` (unset by default) and `log_level`, and from multi frame generation on `fg_multiplier=2|3|4` (default 2; any other value keeps 2 with a config warning). The panel's "Save as default" rewrites `start_with_fg`, `camera_flip_handedness`, `camera_negate_side` and `fg_multiplier` key by key (6.9).
  - `fg_vram_headroom_mib=auto|<0..65536>`: `auto` (any letter case) is the default and adapts the video memory guard to the GPU (6.11); a number is a fixed headroom in MiB with the guard exactly as before `auto` existed, except that a number which keeps frame generation off where `auto` would run it is switched to `auto` and saved by the bridge itself (6.11). Any other value keeps `auto` with the warning "(expected auto or 0..65536 MiB)". The start banner prints `fg_vram_headroom_mib=auto` or the number.
  - The installer's default ini writes `fg_vram_headroom_mib=auto` under "; Video memory kept free on top of the DLSS-G estimate: auto adapts to the GPU, or a number of MiB." and `fg_multiplier=2` with a comment. An upgrade that keeps an existing ini replaces a default headroom block an earlier install wrote, exactly as written (the comment line "; Video memory (MiB) that must stay free in the budget before DLSS-G is turned on." followed by `fg_vram_headroom_mib=512`, or the 0 default of 2026-09-29 with its own comment), by today's comment and `auto`, keeping every other byte and the line endings, and logs "updated the old default fg_vram_headroom_mib=512 to auto". A value without that comment is the user's choice and stays. On the friend's laptop that ini has a hand-added `fg_vram_headroom_mib=0` above the old block; after the update the last line, `auto`, wins, and the repeat warning names both.
- **Log.** `ac-dlssg\logs\bridge.log`. It has a start banner with versions, compatibility inputs and the decision; one statistics line per second (base fps, presented fps, bridge GPU ms, DLSS-G state, double evaluates, the multiplier the DLSS-G options carry as `fg_mult`, and last the render adapter's video memory usage and budget from `IDXGIAdapter3::QueryVideoMemoryInfo`); and every state change.

### 6.11 Hybrid (Optimus) presentation
- **Render GPU.** The render GPU is the adapter of CSP's D3D11 device, found by its LUID. The bridge never uses `EnumAdapters(0)` (the iGPU on a hybrid laptop) and never takes decisions from the NVIDIA adapter's outputs, which it does not have when the internal panel is used. Monitor data comes from the swap chain's `GetContainingOutput`.
- **Presentation path.** DXGI presents the D3D12 flip chain across adapters, the way every D3D12 game on such a laptop presents: two copies through system memory, or one copy (CASO) on Windows 11 with a WDDM 3.x iGPU driver. `sl.dlss_g.dll` 2.14.1 contains its own hybrid swap-chain path; the bridge log review checks the Streamline lines `Failed to setup hybrid GPU for swapchain` and `isHybridGPU=`. NVIDIA documents DLSS-G on MS-Hybrid systems, with higher latency and Streamline enforcing VSync through its own pacing.
- **VSync.** VSync with DLSS-G only when independent flip is active. The laptop default is VSync off with CSP's FPS cap near half the panel refresh rate.
- **Video memory (M3).** Before DLSS-G is first enabled, the bridge queries `slDLSSGGetState` with `eRequestVRAMEstimate`. When the free video memory budget is below the estimate plus a headroom, DLSS-G stays off and the panel says why. The first headroom, 512 MiB, kept DLSS-G off on the 4 GB laptop (346 MiB free for a 283 MiB estimate); the default is now `auto`.
  - **`fg_vram_headroom_mib=auto` (the default).** The headroom is 0 MiB when the render adapter's budget is below 6144 MiB, else 256 MiB (`AutoVramHeadroomMib`). When the 2X check still refuses but falls short by at most max(128 MiB, a tenth of the estimate), but never by more than half of the need (a small output needs less than 128 MiB, and nothing free is not "a little short"), counted as the need rounded up minus the free memory rounded down, DLSS-G turns on anyway and the video memory is marked tight: "fg: video memory is tight (<free> MiB free for <need> MiB); frame generation on anyway (fg_vram_headroom_mib=auto)" at INFO once per change, and the same text as the panel's `vramNote`. <need> is the estimate (or its growth, below) plus the headroom. A tight pass is a pass: the guard does not check again until the multiplier changes. A larger shortfall keeps DLSS-G off with the actionable reason "not enough video memory: frame generation needs <need> MiB, <free> MiB free; lower CSP texture quality, shadows or the render resolution", which is also the `vramNote`, and the check repeats every 60 frames. Only 2X is ever tight: a higher multiplier that does not fit falls back to 2X as below, and that 2X may be tight. The whole decision is `DecideVramMultiplier` in `fg_policy`, unit-tested.
  - **A number.** A fixed headroom in MiB, never tight, with the refusal "video memory: need <x> MiB, free <y> MiB" as before, and the actionable "not enough video memory ..." text as the panel's `vramNote`.
  - **A number that blocks is switched to `auto`.** The owner's rule: the mod adjusts the value itself so that frame generation works, and tells the user. At every check where the number keeps DLSS-G off, the presenter also decides the same check with `auto` (the automatic headroom for the same estimates, budget, usage and resources held; the 2X check of a higher multiplier included). When `auto` would run DLSS-G (it fits, falls back to 2X, or is tight), the number is what keeps frame generation off (the friend's laptop: a stale 512 from an old default), and in that same check:
    - the session switches to `auto`, and the guard records `auto`'s decision, so DLSS-G turns on in that frame without a restart;
    - `fg_vram_headroom_mib=auto` is written into `ac-dlssg.ini` with the key-level edit of 6.9 (`WriteIniKeys`): every line of the key in every `[bridge]` section gets `auto`, so a repeated key cannot bring the number back;
    - the log has the number's check lines at INFO, then "fg: fg_vram_headroom_mib=<h> kept frame generation off; switched to auto and saved it to ac-dlssg.ini" at INFO, then `auto`'s check lines;
    - the panel's `autoFixNote` (6.9) says "Video memory setting fixed: fg_vram_headroom_mib was <h>, now auto (saved). Frame generation is on." for the rest of the session.

    When the file cannot be written, the session still switches to `auto`; the WARN "fg: fg_vram_headroom_mib=<h> kept frame generation off; switched to auto until the game is closed; could not save ac-dlssg.ini: <why>" and the note "Video memory setting fixed: fg_vram_headroom_mib was <h>, now auto. Frame generation is on; this applies until the game is closed; could not save ac-dlssg.ini: <why>" say so. A number that lets DLSS-G run (a 2X fallback included) is the user's choice and stays, and so does a number where `auto` would keep DLSS-G off too (its "not enough video memory ..." note stays). The switch happens at most once per session: the config says `auto` afterwards, and only one presenter per process runs Streamline (6.3, "Final Release"). The rule runs at every start, so a number the user writes back is kept only while it does not keep frame generation off. The decision is `DecideVramHeadroom` in `fg_policy`, and the texts are `VramHeadroomSwitchLog` and `VramHeadroomSwitchNote`, all unit-tested. The presenter's part has no automated test: the guard only runs while DLSS-G is supported, and the tests and the test app run without the spoof on the development machine's RTX 30, where Streamline reports DLSS-G unsupported.
  - **Multi frame generation.** The estimate is asked with the chosen `numFramesToGenerate` (`sl.dlss_g` scales its colour buffers with it), before the first `eOn` and again whenever the wanted multiplier changes. When a higher multiplier does not fit but 2X does, DLSS-G runs at 2X, "fg: video memory: <m>X needs <n> MiB, free <f> MiB; falling back to 2X" is logged and becomes the status reason; a fallback is not retried until the multiplier changes again, so that the count does not flap with the budget. When 2X does not fit either, DLSS-G stays off with 2X's numbers and the check repeats every 60 frames.
  - **Resources already held.** `eRetainResourcesWhenOff` keeps DLSS-G's resources from its last `eOn`, and the process's usage already contains them. So only the growth, the new estimate minus the estimate of the multiplier DLSS-G last ran at, plus the headroom, has to fit, and a lower or the same multiplier always fits. Without this, a change from 2X to 3X on the 4 GB laptop (346 MiB free before DLSS-G, 283 MiB for 2X) would count 2X twice and turn DLSS-G off.
- **Testing.** The laptop runs every milestone checklist from M2 on, on the internal panel. One run on an external monitor on the HDMI port, which is wired to the NVIDIA GPU, separates hybrid-path problems from bridge problems.

## 7. Per-frame data flow

Bridge frame N. Test presents do not take part (6.3).

1. **Frame start.** The frame starts at the end of the previous non-test Present. For the very first frame, it starts at the end of `D3D12Presenter` creation. The bridge calls `slGetNewFrameToken(token, &counter)` with its own incrementing counter, then `slReflexSleep(token)`, then emits `eSimulationStart`. That token object is used for every PCL marker, `slSetTagForFrame` and `slSetConstants` call of this frame.
2. **CSP renders the 3D scene.** At the first qualifying DLSS evaluate of frame N:
   - the camera snapshot is latched;
   - depth and motion vectors are copied into slot `N mod 3` (no fence signal; the Present's V covers the copy);
   - `eSimulationEnd` and `eRenderSubmitStart` are emitted.

   Later evaluates overwrite the capture but emit no markers. A per-token bitmask prevents duplicate markers.
3. **CSP finishes the frame.** It applies post-processing, scales into buffer 0 of the hidden chain and draws its UI. ReShade draws its effects and overlay, then calls our Present.
4. **Our Present, D3D11 side.**
   - If this frame had no evaluate, emit `eSimulationEnd` and `eRenderSubmitStart` now.
   - `CopyResource` from the hidden buffer 0 into the shared back buffer.
   - Signal the shared fence (value V) and emit `eRenderSubmitEnd`.
5. **Our Present, D3D12 side.**
   1. `queue->Wait(sharedFence, V)`. This also covers the frame's capture copy.
   2. `GetCurrentBackBufferIndex` on the proxy; wait until `progress` has retired that buffer's allocator.
   3. Record transitions and `CopyResource` from the shared back buffer into that buffer.
   4. **DLSS-G this frame** if all of these hold: DLSS-G is enabled; a capture paired with this Present exists; the camera snapshot is fresh (6.6) and has neither the paused nor the in-main-menu flag set; and there is no stall or aspect refusal. Then call `slSetTagForFrame` with depth and motion vectors (render-subrect extent, `eValidUntilPresent`, state `COMMON`), then `slSetConstants`. Otherwise set null tags and put DLSS-G in `eOff` for this frame.
   5. Execute the commands.
6. **Present.**
   - Emit `ePresentStart`.
   - Present on the Streamline proxy chain with CSP's `SyncInterval` clamped to 0..1 and CSP's flags.
   - Keep `DXGI_PRESENT_ALLOW_TEARING` only when CSP passed it, the effective sync interval is 0, the D3D12 chain was created with `ALLOW_TEARING`, and the chain is windowed; otherwise strip it. Never add it.
   - If DLSS-G is on, CSP asked for sync interval 1, and `bIsVsyncSupportAvailable` is not `eTrue`, present with sync interval 0 and without `ALLOW_TEARING` (DWM keeps a borderless window tear-free). Log this once and show it in the panel.
   - Emit `ePresentEnd`. With DLSS-G on, Streamline presents the interpolated frame N−½, then frame N.
7. **End of frame.**
   - `queue->Signal(sharedFence, V')` and `queue->Signal(progress, V')`, and record `pendingWait = V'`. The matching `ctx4->Wait(sharedFence, V')` is issued at the start of the next Present, just before the copy into the shared texture: that texture is the only resource the two sides share, so CSP's next frame is not held behind the D3D12 copy (as built in M1).
   - Release one count of the latency semaphore.
   - Frame start for N+1 (step 1).

## 8. Lifecycle

- **Start.** The game starts. ReShade loads our DLL as its `ProxyLibrary`, and `Bootstrap` runs on the first factory call. When CSP creates its swap chain, the hook either returns a `ProxySwapChain` or passes through, and logs the decision. `start_with_fg` sets the initial DLSS-G mode.
- **Resize, fullscreen change or target resize.** Handled by 6.3. DLSS-G is switched off, and one present is issued with it off before the change.
- **Hotkey or panel toggle.** `slDLSSGSetOptions` switches between `eOn` and `eOff` on the next Present; a panel request is read at the start of that Present (6.9). The next DLSS-G frame has `reset = eTrue`.
- **Multiplier change** (the panel, through `SetFgMultiplier`). Applied at the next Present like a toggle (6.8): the video memory guard checks the new multiplier, the options are re-sent with the new count, the next DLSS-G frame has `reset = eTrue`, and the swap chain stays.
- **Focus loss.** Streamline pauses interpolation while the game window is not focused, with the mode still `eOn` (`DLSS-G disabled: window not focused` in `sl.log`; in the M3 run the generated count stayed 0 for minutes at a time). The panel shows it as paused (6.9 `fgPaused`).
- **Shutdown.** The final `Release` of `ProxySwapChain` follows the order in 6.3. After `slShutdown`, the cached Bootstrap decision becomes "not possible (Streamline already shut down)". Every later `CreateSwapChainForHwnd` passes through, and no Streamline function is called for the rest of the process. `DLL_PROCESS_DETACH` tears nothing down, because ReShade frees the ProxyLibrary under the loader lock.

## 9. Error handling

| Situation | Behaviour |
|---|---|
| Missing Streamline files, failed signature check, `slInit` failure, unsupported GPU, RTX 30 without the spoof, or a compatibility refusal | No proxy. The game runs as without the bridge. The reason is logged and shown in the panel. |
| D3D12 or hidden-chain creation fails inside `CreateSwapChainForHwnd`, or the chain is not a Streamline proxy | Release partial objects, call the original, return the real swap chain. Same as above. |
| No DLSS evaluate for 30 frames, or no fresh camera for 30 frames | Panel reports the input as missing. DLSS-G is already off for every such frame (7.5.4). Presentation continues through D3D12, and recovery is automatic. |
| `slDLSSGGetState` failure status | DLSS-G off and the status logged. Retried after the next resize or toggle. |
| The D3D12 queue makes no progress for 500 ms (seen by the watchdog, a Present or a CPU wait) | CPU-signal the shared fence to `pendingWait` so CSP's D3D11 queue is released, turn DLSS-G off, and stop delivering until `progress` catches up (6.4). |
| No progress for 4 s, or device removal | Stop the presenter, log `GetDeviceRemovedReason`, and return `DXGI_ERROR_DEVICE_HUNG` or `DXGI_ERROR_DEVICE_REMOVED` from Present, as a real swap chain would. |
| Exception in a hook | Every hook body is `noexcept` and catches everything. It logs and forwards to the original. |
| Our DLL missing, blocked or quarantined | Not recoverable by the bridge; ReShade crashes at start. Recovery: run `<game>\ac-dlssg\install\uninstall.bat`, or set `EnableProxyLibrary=0` in `ReShade.ini`. Documented in the README under "Game crashes at start". |

## 10. RTX 30 support through dlssg_for_sm86

- **Installer.** It reads the `PNPDeviceID` (`VEN_10DE&DEV_xxxx`) of every `Win32_VideoController` and classifies the NVIDIA ones with the device-ID table of `gpu_info.cpp`. RTX 40 and RTX 50 need nothing; when one is present, the spoof is not installed. For RTX 20 it prints that frame generation is not supported there and installs no spoof. For Ampere GeForce GPUs (SM86, desktop and laptop), it prints the author, the source URL, the risk and this notice before it downloads anything: *the repository has no LICENSE file; its README says the project source is GPLv3, but no source is published; the binary embeds NVIDIA's `nvngx_dlssg.dll`, which is not relicensed; running it on RTX 30 circumvents a technical limitation, which section 4.d of the NVIDIA RTX SDKs License forbids, and you are that license's licensee.* It then continues without asking. At the owner's request (2026-09-29) the notice replaces the earlier consent prompt; `-NoSpoof` skips the spoof entirely.
- **Then,** in the installer's staging phase (section 12), it:
  - downloads exactly `version.dll` and `dlssg_sm86.ini` from commit `9621db573e07ed54f50c15bbb585ed9a7bdfac28` of `sdli1995/dlssg_for_sm86` (the files of tag `0.3.5`). The GitHub contents API lists both as plain git blobs at that commit (`version.dll` is 30021920 bytes, not a Git LFS pointer; the repository has no `.gitattributes`), so they come from `https://raw.githubusercontent.com/sdli1995/dlssg_for_sm86/<commit>/<name>`;
  - verifies their git blob SHA-1: `efd92261f2b74e0a0fb927d74bce7a1c0c2413f7` for `version.dll`, `2c97d64f2239b7d511f7d0a36c16e149dd3329f6` for the ini, and for `version.dll` also its size and SHA-256 `c3934a09399f022504227c72df0bf8c0de55f9a08880dddde898c5262cefa838`. On a mismatch it deletes the download and stops with nothing changed;
  - installs them next to `acs.exe` as journaled changes. The manifest records each file as installed, or as found when the pinned file was already there; the uninstaller removes only installed files whose hash is still the installed one.
- **An existing `version.dll`.** One with the pinned SHA-256 stays, is not downloaded again and is recorded as found. Any other one (another mod, another spoof version) stays untouched; the installer warns and installs no spoof.
- **Never downloaded:** the repository archive, because it contains `archive/0.1.0/version.dll`, which Defender flags as a trojan, and the files under `alternatives/`.
- **Load order.** `acs.exe` imports `VERSION.dll` statically, so the spoof loads at process start, before ReShade and our DLL, and is armed before `slInit`.
- **In the panel.** The bridge shows the real GPU and the spoof state. It logs the spoof's own log locations: `dlssg_sm86\logs\loader_<pid>.jsonl` and `backend_<pid>.jsonl`.
- **Configuration.** The default `dlssg_sm86.ini` is kept byte for byte.
- **Supported GPUs.** The upstream README requires only SM86 (or SM75 for RTX 20); an earlier version of this spec wrongly said RTX 3070 and up. The author validated on an RTX 3070 and an RTX 3080 Ti. Users report the RTX 3050 Laptop and the RTX 3050 Ti Laptop working at 2X, with reported flicker (upstream issue #384) and an fps drop after about 2 minutes (#607), which the laptop checks watch for.
- **Credit.** The README credits sdli1995 (the user reports the author's agreement) and Coldwood1026, whose RTX 20 work ships inside the same `version.dll`.

## 11. Testing and milestones

**Automated tests**, which run on any Windows machine with the build tools:
- `FrameConstants`:
  - known cameras give the expected matrices;
  - `clipToPrevClip` round-trips;
  - the orthonormality check holds;
  - radians are used;
  - the reset rules hold.
- MV-scale conversion, config parsing and layering, the aspect test, the GPU ID to architecture table, the latency-semaphore accounting, the seqlock reader, and the camera-layout `static_assert`s.
- The panel (6.9): the status and control layouts against the Lua layout strings, both seqlocks (including a reader racing the writer and a record of another process), the request rules (counter, switches, save), the key-level INI writer (other bytes, CRLF, missing keys under `[bridge]`), and a presenter that applies requests from the control section and publishes its status.

**Test application (`tools/testapp`).** A small D3D11 program that creates a swap chain exactly as CSP does (section 4) and renders a moving scene with depth and motion vectors. It creates its back-buffer render target view with a NULL description and also creates an sRGB view. It loads our DLL the way ReShade does. It covers:
- test presents;
- `SetMaximumFrameLatency`;
- `ResizeBuffers`;
- create, release and re-create of the main swap chain (from M2 on the second chain must be a pass-through, because the first one's release shuts Streamline down; in M1 both chains are proxied);
- a forced D3D12 stall (the watchdog must release D3D11);
- `fg_multiplier=3` in the `fg-pipeline` scenario: without DLSS-G nothing lowers the request, and every statistics line reports `fg_mult=3`;
- standalone mode: the bridge copied next to the test app as `dxgi.dll` and bound by name, so that `d3d11.dll`, `d3d12.dll` and Streamline bind to it too;
- the panel (`--fake-panel`, scenario `panel`): the test app plays the Lua app's window. The status section appears and its heartbeat advances; "off" logs `fg: panel -> off` and the status shows the switch off with the user's reason; a camera switch and "Save as default" show up in the status, and the save edits the test's `ac-dlssg.ini` key by key; "on" logs `fg: panel -> on`; the release publishes pass-through.

DLSS-G itself is exercised only in-game, by the user. The multiplier's decisions (the clamp to `numFramesToGenerateMax`, the video memory fallback, when options are sent, the request tracking) are pure functions in `fg_policy` with unit tests; the runtime setter is tested on the plain D3D12 path, and a Streamline child-process test cannot reach `slDLSSGSetOptions` without a GPU that Streamline supports.

**Milestones.** Each ends with a short in-game checklist run by the user, on both machines from M2 on (the laptop in standalone mode): launch, borderless window, Alt+Tab, a resolution change, pause menu, a replay, and a 15-minute drive. The bridge log is analysed afterwards.
- **M0 (done).** dlssg_for_sm86 was verified in WheelMates on the reference machine.
- **M1.** Proxy swap chain, hidden chain and D3D12 presentation, without Streamline. Checks:
  - the image is identical, and VSync or tearing behaves as without the bridge;
  - `ReShade.log` shows `Recreated runtime environment` for the proxy chain and never `Failed to create back buffer render targets!`;
  - F8 and CSP screenshots still save;
  - the bridge costs less than 0.5 ms;
  - one run with `enabled=0` and the spoof present is compared with a run without the spoof.

  **Result (2026-09-28, reference machine): passed.**
  - The game starts normally, and the picture is unchanged.
  - The ReShade overlay works.
  - Alt+Tab and minimize cause no freeze and no black screen.
  - A resolution change works.
  - A drive on Shutoko with traffic had no crash and no stutter.
  - NVIDIA ShadowPlay works over the D3D12 chain.
  - The bridge log shows the proxy decision for the main window, `bridge_gpu_ms` d3d11 0.025 + d3d12 0.023 ms, no stall and no warning or error, and CSP's `Present(0, 0x200)` passed through with frame latency 2.
  - With the spoof present and `enabled=0`, the game behaved the same. The user saw about 10 fps more in that configuration than in a session with the bridge enabled and no spoof, so two variables changed at once. A test-app A/B on the same machine with visible windows (GPU-bound, about 98 fps) measured the bridge at under 1% (98.5/98.3 fps without it, 97.8/97.7 with it), so the difference comes from the spoof or from the sessions, not from the bridge. The spoof's effect on CSP (it rewrites the NVAPI architecture for every caller) is to be measured in M3. Hidden-window test-app runs are too noisy for such comparisons (84.9-95.9 fps for the same bridge run).
  - VSync was not checked, because the user does not use it. The VSync queue depth is covered by the unit test `Presenter_TakesTheGamesFirstLatencyWait`.
- **M2.** Streamline init, the proxy chain, Reflex, and the full PCL marker sequence, with DLSS-G off. The Streamline log is clean and Reflex is detected. For the laptop, M2 also brings per-adapter HAGS detection, the NVIDIA-adapter check, the hybrid and video memory logging, standalone mode, and a test package with install, uninstall and log-collection scripts. The package contains no NVIDIA file; its installer downloads the Streamline DLLs from NVIDIA on the target machine, as in section 12.
- **M3.** NGX capture, the Lua camera app, and DLSS-G 2X on the RTX 3080 and on the laptop through the spoof, with the video memory guard (6.11). A debug overlay shows the motion vectors and the camera handedness. Success criteria 1 and 2 are met on both machines, and DLSS-G is off in menus and pause. Pacing is measured as displayed fps with PresentMon or FrameView, not as presents, because upstream issue #541 reports broken pacing for another DX11-to-D3D12 bridge with this spoof.
- **Multi frame generation (after M3).** 3X and 4X on the RTX 3080 through the spoof, and on the laptop as far as its 4 GB allow. Checks: the log shows "fg: Streamline allows up to 4X" and "fg: DLSS-G options: 3X" and "4X", with the raw numFramesActuallyPresented of the first DLSS-G Present of each (M3 saw 1 on the first 2X Presents, so a single value is not a verdict); over a drive `generated` is near 2 and 3 times `fg_frames`; displayed fps measured with PresentMon or FrameView; a runtime change 2X -> 4X -> 2X from the panel without a swap-chain re-creation, freeze or crash; on the laptop, a 4X request that does not fit falls back to 2X with its log line. Needs `numFramesToGenerateMax` > 1: RTX 50 natively, or the spoof.
- **M4.** Panel, hotkey, installer, uninstaller, README, CI with attestations, and release v1.0.0. The panel (6.9) was built first, on 2026-09-29, as the CSP Lua app's window for both modes.

## 12. Build, packaging and release

- **Build.** C++20, MSVC (VS 2022 Build Tools) and CMake, Windows SDK 10.0.22621 or newer, static CRT (6.1). CI fails the build if `dumpbin /dependents` on `ac-dlssg.dll` lists a DLL outside the allow-list, which forbids for example `vcruntime*`, `msvcp*`, `d3dcompiler_*`, `dxgi.dll` and `VERSION.dll`.
- **Third-party code.**
  - Vendored: nothing in v1. ReShade's `include/` and the Dear ImGui headers were only needed for the ReShade overlay panel, which v1 does not have (6.9).
  - Fetched at configure time: Streamline headers from the pinned release (MIT).
  - Never in the repository: NVIDIA binaries and NGX headers. The NGX parameter interface is our own declaration.
  - `THIRD_PARTY_NOTICES.txt` in the repository and in the release zip reproduces the Streamline and dlss5-bridge notices.
- **CI.** GitHub Actions on `windows-latest` builds and tests on every push. On a tag, it:
  - builds the zip;
  - creates GitHub artifact attestations for the zip and for `ac-dlssg.dll` with `actions/attest-build-provenance` (permissions `id-token: write, contents: write, attestations: write`);
  - writes both SHA-256 values into the release notes.

  Anyone can then check a downloaded file with `gh attestation verify <file> --repo flopsy2iqq/ac-dlssg`.
- **Release zip:**
  - `ac-dlssg.dll`;
  - `apps/lua/AcDlssg/`;
  - `install.ps1` and `install.bat`;
  - `uninstall.ps1` and `uninstall.bat`;
  - `README.md`, `LICENSE`, `EXCEPTIONS.md` and `THIRD_PARTY_NOTICES.txt`.
- **Installer.** `install.ps1` is started by `install.bat` as `powershell -NoProfile -ExecutionPolicy Bypass -File`. It refuses to run while `acs.exe` is running. It asks nothing; its only input is the final "Press Enter to exit", so that a double-clicked window stays open. When the account may not write into the game folder (a game under `C:\Program Files (x86)`), it starts itself again elevated, once (`Start-Process -Verb RunAs`), and that window continues; the UAC prompt is the only question. The package (`tools\make-test-package.ps1`, the release zip `ac-dlssg-<version>.zip`) holds only `install.bat` in its root, with `files\`, `scripts\`, `tools\` (`uninstall.bat`, `collect-logs.bat`) and `docs\` (`README.md`, `README.ru.md`, `LICENSE`, `EXCEPTIONS.md`, `THIRD_PARTY_NOTICES.txt`, `README-test.txt`) beside it (the owner's request, 2026-09-29).
  - **Phase A: checks and staging.** Nothing in the game folder changes in this phase.
    1. Find Assetto Corsa through the Steam library folders.
    2. Check the CSP version and pick the mode: ReShade mode when ReShade with add-on support is `dxgi.dll`, standalone mode when the game folder has no `dxgi.dll`. When another `dxgi.dll` is present, stop and explain.
    3. Check HAGS per adapter (as in 6.10), the driver version (warn below 581.29 on hybrid laptops and below R580 with the spoof) and Smart App Control (`HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy\VerifiedAndReputablePolicyState`). At 1 (On) or 2 (Evaluation), refuse and explain: SAC blocks the unsigned bridge DLL, which makes ReShade crash at start, and the self-signed spoof (`Bad Image 0xc0e90002`). Report the compatibility settings from 6.10.
    4. List any `nvngx_*.dll` and `sl.*.dll` in the game root and warn that NGX may load that `nvngx_dlssg.dll` instead of ours. Never move or delete them, because CSP and other packages use them.
    5. Download into `%TEMP%\ac-dlssg-setup\`, with TLS 1.2 forced, `$ProgressPreference='SilentlyContinue'`, 3 retries and a 600 MB free-space check:
       - `streamline-sdk-v2.14.1.zip` from the NVIDIA-RTX/Streamline GitHub release. Verify SHA-256 `92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b`. Print where `license.txt`, `bin/x64/nvngx_dlss.license.txt` (the NVIDIA RTX SDKs License, which covers `nvngx_dlssg.dll`) and `bin/x64/reflex.license.txt` are and that installing means accepting them, and continue; there is no prompt (the owner's request, 2026-09-29, replacing the earlier acceptance prompt). Extract only the production `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll` and `nvngx_dlssg.dll`, plus those three license files.
       - On an SM86 GPU and without `-NoSpoof`, the spoof files (section 10), after the printed notice.

       The test package (`tools\make-test-package.ps1`) downloads into its own `files\deps\` folder instead of `%TEMP%`.

       Any failure in phase A ends the installer with nothing changed.
  - **Phase B: journaled changes.** Create `<game>\ac-dlssg\install\` and write `manifest.json` before the first change. For each change:
    1. append and flush an entry: action, path relative to the game folder, SHA-256 before, SHA-256 after, backup name;
    2. copy the file being replaced into `install\backup\`;
    3. write the new file as `<name>.new`, verify its hash, and rename it over the target.

    Order: `ac-dlssg\sl\`, the spoof files, the Lua app, then in ReShade mode `ac-dlssg.dll` and `ReShade.ini` last, or in standalone mode the bridge as `dxgi.dll` last. The manifest records the mode. Finally copy `uninstall.ps1` and `uninstall.bat` into `install\`. On any failure, replay the journal in reverse and exit with an error.
  - **ReShade.ini edits are key-level only.**
    - **Which file.** Resolve ReShade's base path the way ReShade 6.8.0 does: `[INSTALL] BasePath` from `<game>\ReShade.ini`, relative to the game folder; else the `RESHADE_BASE_PATH_OVERRIDE` environment variable; else the game folder. Edit `<base>\ReShade.ini`.
    - **Another chained DLL.** If `[PROXY] ProxyLibrary` is non-empty and does not name our DLL, stop and explain that another DLL is already chained behind ReShade.
    - **The edit.** Record the old `EnableProxyLibrary` and `ProxyLibrary` values in the manifest. Replace those lines in place, and add them under `[PROXY]` only when they are missing. Never write a second line for a key. Write the file as UTF-8 and leave every other byte unchanged.
    - **The value.** `ProxyLibrary=ac-dlssg.dll` when the base path is the game folder; otherwise the absolute path of `<game>\ac-dlssg.dll`.
  - **Re-install and upgrade.** If `manifest.json` exists, the installer runs as an upgrade. It never rewrites the "before" data of existing entries or the files in `install\backup\`. For each target file:
    - if its SHA-256 equals the recorded "after" hash, it is ours: replace it without a new backup and update "after";
    - if the manifest knows the path but the hash differs, someone (or an older build) changed it: report it, keep a copy in `install\backup\`, and replace it without asking; the first backup stays the one the uninstaller restores;
    - if the manifest does not know the path, back it up as a new original and add an entry;
    - files the manifest records that the new build no longer ships are removed (a changed one keeps a copy in `install\backup\`).

    An upgrade never asks (the owner's request, 2026-09-29): it handles every older manifest schema, and when the mode has to change (ReShade was installed over the standalone bridge, or removed since a ReShade-mode install) it undoes the old mode's files and installs the new mode in the same run. It still stops for files that are clearly not ours, such as a foreign `dxgi.dll` or a foreign `apps\lua\AcDlssg`; a foreign `version.dll` is left alone with a warning (section 10).

    The `[PROXY]` values recorded at first install stay the values the uninstaller restores. If bridge files exist without a manifest, the installer stops and asks the user to remove them, or to set `EnableProxyLibrary=0`, first.
- **Uninstaller.** `<game>\ac-dlssg\install\uninstall.bat` also ships in the release zip. It runs from a copy in `%TEMP%` and refuses to run while `acs.exe` is running.
  1. In ReShade mode, it reverts the two `[PROXY]` keys first, and only if they still hold the values the installer wrote; otherwise it leaves them and reports. It never restores a whole-file copy of `ReShade.ini`, and it leaves an absent `ReShade.ini` absent.
  2. It re-reads the file, and continues only once ReShade no longer loads our DLL. In standalone mode, it first removes `<game>\dxgi.dll` when its SHA-256 is the recorded "after" hash, and otherwise stops and reports.
  3. It reverts the manifest entries in reverse. It skips entries whose file is already gone, and it leaves in place and reports any file whose SHA-256 differs from the recorded "after" hash.
  4. It removes the spoof files the manifest records as installed (each only while its hash is the installed one), and leaves found ones. It deletes `<game>\ac-dlssg\`, and, with `-RemoveData`, also `<game>\dlssg_sm86\` and `%LOCALAPPDATA%\DlssgSm86\`, but only when the installer put the spoof there and no `version.dll` is left in the game folder (the owner's request, 2026-09-29, replacing the earlier question).

## 13. Licensing

- **This project.** GPL-3.0 with the Modding Exception and the GPL-3.0 section 7 linking permission copied from open-shaders' `EXCEPTIONS.md`, shipped as `EXCEPTIONS.md`. Together they allow the DLL to be combined with `acs.exe`, CSP, ReShade, and NVIDIA's Streamline and NGX binaries. Code adapted from open-shaders, from Community Shaders (GPL-3.0 with the same exceptions) and from dlss5-bridge (MIT) keeps its notices.
- **Streamline and NGX runtime DLLs.** Never committed or re-hosted. The installer downloads them from NVIDIA's release on the user's machine, after the user accepts NVIDIA's terms.
- **The RTX 30 spoof.** It circumvents NVIDIA's RTX 40 requirement for DLSS-G, which section 4.d of the NVIDIA RTX SDKs License forbids for licensees. It is therefore optional (`-NoSpoof`), installed only on SM86 GPUs after the notice in section 10 is printed (since 2026-09-29 without a consent prompt, at the owner's request), downloaded from the author's repository on the user's machine, never re-hosted by this project, and documented with this risk in the README.

## 14. Risks and how the milestones retire them

| Risk | Retired in | Fallback |
|---|---|---|
| ReShade wraps our D3D12 device or chain despite `g_in_dxgi_runtime` | M1 | Unwrap with `IID_UnwrappedObject`, then create the queue and read the factory from the native objects |
| CSP's DirectX watchdog fires because Streamline initialization inside `CreateSwapChainForHwnd` is slow | M2 | Move more work into `Bootstrap`; create the device earlier |
| The spoof fails with our Streamline integration or with driver 616.64 | M0 (passed for a native game), M3 | Ship for RTX 40/50 only; report upstream |
| Camera convention wrong (handedness, FOV axis, origin shift) | M3 | Debug overlay plus one convention flag |
| Motion-vector sign or scale wrong | M3 | Debug overlay; an `mv_sign` config override added then |
| The hidden-chain copy plus the single shared back buffer cost too much | M1 | Ring of shared back buffers |
| UI artifacts too visible without a HUD-less buffer | M3 | HUD-less capture (v2) |
| The NGX snippet or Streamline override comes from elsewhere | M2 | Logged and shown in the panel; the user clears the override |
| Standalone mode misses a `dxgi.dll` export that `d3d11.dll`, `d3d12.dll` or Streamline imports | M2 | Export list checked against their import tables; test-app scenario |
| Hybrid presentation (cross-adapter present, or `sl.dlss_g`'s hybrid path with our proxy chain) misbehaves | M2 and M3 on the laptop | Compare with an external monitor on the NVIDIA-wired HDMI port; report upstream |
| 4 GB of video memory overflows with DLSS-G | M3 on the laptop | The video memory guard keeps DLSS-G off; lower CSP settings |
| Frame pacing with the spoof behind a DX11-to-D3D12 bridge (upstream #541) | M3 | Measure displayed fps; A/B the spoof's `SpoofArchValue` |

## 15. Planned after v1

- HUD-less colour and UI buffers.
- A ring of shared back buffers to remove the per-frame D3D11 wait.
- Letterboxed output, by tagging `kBufferTypeBackbuffer` with the letterbox rectangle.

## 16. References

- NVIDIA Streamline: https://github.com/NVIDIA-RTX/Streamline (docs/ProgrammingGuideDLSS_G.md, ProgrammingGuideManualHooking.md, ProgrammingGuideReflex.md)
- open-shaders, DLSS-G on a D3D12 proxy: https://github.com/alandtse/open-shaders (src/Features/Upscaling/DX12SwapChain.cpp, Streamline.cpp, EXCEPTIONS.md)
- Community Shaders: https://github.com/community-shaders/skyrim-community-shaders
- dlss5-bridge: https://github.com/NIGos/dlss5-bridge (v1.4.12)
- ReShade: https://github.com/crosire/reshade (v6.8.0)
- CSP Lua SDK: https://github.com/ac-custom-shaders-patch/acc-lua-sdk
- dlssg_for_sm86: https://github.com/sdli1995/dlssg_for_sm86
- NVIDIA DLSS SDK (NGX parameter names): https://github.com/NVIDIA/DLSS
