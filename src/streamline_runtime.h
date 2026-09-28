#pragma once
// NVIDIA Streamline for the bridge (spec 6.8). One process-wide instance.
// Streamline's headers come from the pinned SDK (deps/streamline-2.14.1/include);
// sl.interposer.dll is loaded at runtime by absolute path, never linked.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "pcl_sequencer.h"

namespace acdb {

// Fixed project id for production Streamline, which disables NGX features
// unless projectId and engineVersion are both non-empty (spec 6.8).
constexpr const char kSlProjectId[] = "3f0c9a52-7a4e-4f7e-9b0d-5f1c2a8e6d41";

// Storage that the sl::Preferences returned by BuildPreferences points into;
// it must outlive the slInit call.
struct SlPreferencesStorage {
    std::wstring plugin_dir;
    std::wstring log_dir;
    std::string engine_version;
    const wchar_t* plugin_paths[1] = {};
    sl::Feature features[3] = {};
};

// Spec 6.8:
//  flags = eUseManualHooking | eUseFrameBasedResourceTagging | eDisableCLStateTracking
//          (no eAllowOTA, no eLoadDownloadedPlugins);
//  featuresToLoad = {kFeatureDLSS_G, kFeatureReflex, kFeaturePCL};
//  pathsToPlugins = {pluginDir}; pathToLogsAndData = logDir;
//  renderAPI = eD3D12; engine = eCustom; engineVersion = engineVersion;
//  projectId = kSlProjectId; logLevel = eDefault; showConsole = false;
//  logMessageCallback = callback (may be null in tests).
sl::Preferences BuildPreferences(const std::wstring& pluginDir, const std::wstring& logDir,
                                 const std::string& engineVersion, sl::PFun_LogMessageCallback* callback,
                                 SlPreferencesStorage* storage);

// The size and format hints sl::DLSSGOptions 2.14.1 offers for the buffers the
// bridge uses (colour = the Streamline chain's back buffers, depth and motion
// vectors = the capture slots). Formats are DXGI_FORMAT values. DLSS-G uses
// them to size its resources and for the video memory estimate (spec 6.11).
// No HUD-less colour and no UI buffer in v1 (spec 3).
struct DlssgSizeHints {
    uint32_t numBackBuffers = 0;
    uint32_t mvecDepthWidth = 0, mvecDepthHeight = 0;
    uint32_t colorWidth = 0, colorHeight = 0;
    uint32_t colorBufferFormat = 0;
    uint32_t mvecBufferFormat = 0;
    uint32_t depthBufferFormat = 0;
};

// Spec 6.8 "DLSS-G mode": mode eOn or eOff, numFramesToGenerate 1 (2X),
// flags eRetainResourcesWhenOff (plus eRequestVRAMEstimate when asked), and
// every hint above. Everything else keeps Streamline's defaults.
sl::DLSSGOptions BuildDlssgOptions(bool on, const DlssgSizeHints& hints, bool requestVramEstimate);

// The sl::Result's enumerator name ("eOk", "eErrorFeatureMissing", ...).
const char* SlResultName(sl::Result r);

class StreamlineRuntime {
public:
    static StreamlineRuntime& Get();

    // Verifies sl.interposer.dll in pluginDir with sl::security::verifyEmbeddedSignature,
    // loads it with LoadLibraryW by absolute path, resolves the sl* exports
    // with GetProcAddress and calls slInit(BuildPreferences(...), sl::kSDKVersion)
    // under InternalCallScope. Streamline's log lines go to our log prefixed
    // "sl: " at the matching level. Idempotent: a second call returns the first
    // result; after Shutdown every call fails (one lifetime per process).
    // Returns false with *error when any step fails; the process then
    // runs without the bridge.
    bool Init(const std::wstring& pluginDir, const std::wstring& logDir, std::string* error);
    // Lock-free reads: FactoryHook asks from the thread creating a chain while
    // another thread may be in Shutdown.
    bool Initialized() const { return initialized_.load(); }
    bool IsShutDown() const { return shut_down_.load(); }

    // slSetD3DDevice(nativeDevice), then resolves slReflexSetOptions,
    // slReflexGetState, slReflexSleep, slPCLSetMarker, slDLSSGSetOptions and
    // slDLSSGGetState with slGetFeatureFunction. Missing DLSS-G functions are
    // not an error here (DlssgFunctionsResolved reports them and the
    // presenter decides); missing Reflex/PCL functions are.
    // The DLSS-G functions are requested only when slIsFeatureSupported
    // accepts the device's adapter (otherwise Streamline logs an error per
    // request).
    bool SetDevice(ID3D12Device* nativeDevice, std::string* error);

    // slIsFeatureSupported(kFeatureDLSS_G, AdapterInfo{luid}). *why gets the
    // sl::Result name and, when unsupported, the reason text.
    bool DlssgSupported(const LUID& luid, std::string* why) const;

    // slUpgradeInterface; true on sl::Result::eOk.
    bool Upgrade(void** iface) const;
    // True when slGetNativeInterface returns an object other than obj (the
    // extra reference it adds is released).
    bool IsProxied(IUnknown* obj) const;

    // slReflexSetOptions({mode = eLowLatency}); true on eOk.
    bool EnableReflexLowLatency();
    // slReflexGetState().lowLatencyAvailable.
    bool ReflexLowLatencyAvailable() const;

    // slGetNewFrameToken with the caller's frame index; nullptr on failure.
    sl::FrameToken* NewFrameToken(uint32_t frameIndex);
    void ReflexSleep(const sl::FrameToken& token);
    void Marker(PclMarker marker, const sl::FrameToken& token);

    // M3. The DLSS-G calls exist only when SetDevice resolved them, which it
    // does only when slIsFeatureSupported accepts the adapter.
    bool DlssgFunctionsResolved() const;
    // slDLSSGSetOptions(viewport 0, BuildDlssgOptions(on, hints, false)), on
    // the presenting thread. eErrorNotInitialized before Init or after
    // Shutdown, eErrorFeatureMissing without the DLSS-G functions.
    sl::Result SetDlssgOptions(bool on, const DlssgSizeHints& hints);
    // slDLSSGGetState(viewport 0, *state, options): options is null for a
    // plain status poll, and BuildDlssgOptions(true, hints, true) when the
    // video memory estimate is requested (expensive; spec 6.11 only). Same
    // refusals as SetDlssgOptions; *state is left alone then.
    sl::Result GetDlssgState(bool requestVramEstimate, const DlssgSizeHints& hints, sl::DLSSGState* state);
    // slSetTagForFrame(token, viewport, {depth: kBufferTypeDepth, mvec:
    // kBufferTypeMotionVectors}, 2, cmdList): both eValidUntilPresent, state
    // D3D12_RESOURCE_STATE_COMMON, extent for both. cmdList may be null:
    // Streamline 2.14.1 uses the command list only to copy tags of the other
    // lifecycles (sl.common ResourceTaggingForFrame::setTag); the presenter
    // passes the frame's open list, as open-shaders does. These three calls
    // live in sl.common and work while DLSS-G is unsupported.
    sl::Result SetTagsForFrame(const sl::FrameToken& token, uint32_t viewport, ID3D12Resource* depth,
                               ID3D12Resource* mvec, const sl::Extent& extent, ID3D12GraphicsCommandList* cmdList);
    // Both tags with a null resource: clears this token's entries. Streamline
    // keeps its references to earlier frames' resources until it recycles
    // them in a later tag call (frames older than its present frame - 2).
    sl::Result SetNullTags(const sl::FrameToken& token, uint32_t viewport);
    // slSetConstants(constants, token, viewport).
    sl::Result SetConstants(const sl::Constants& constants, const sl::FrameToken& token, uint32_t viewport);

    // Logs the full path and file version of every loaded sl.*.dll and
    // nvngx_dlssg*.dll, and a WARN for each outside the plugin directory
    // (an OTA override or another mod's copy, spec 6.8). Returns that count.
    int LogLoadedModules() const;

    // slShutdown() once; later calls do nothing. After it, Initialized()
    // stays true and IsShutDown() is true for the rest of the process.
    void Shutdown();

    // Streamline log lines seen at error and warning level (tests).
    uint32_t ErrorsLogged() const { return errors_.load(); }
    uint32_t WarningsLogged() const { return warnings_.load(); }

private:
    StreamlineRuntime() = default;
    static void OnLogMessage(sl::LogType type, const char* message);

    mutable std::mutex mu_;
    bool init_done_ = false;
    // Written under mu_; atomic for the lock-free getters above.
    std::atomic<bool> initialized_{false};
    std::atomic<bool> shut_down_{false};
    std::string init_error_;
    std::wstring plugin_dir_;
    HMODULE interposer_ = nullptr;
    SlPreferencesStorage storage_;
    std::atomic<uint32_t> errors_{0};
    std::atomic<uint32_t> warnings_{0};
    // Resolved entry points (implementation-defined function pointer types).
    struct Api;
    Api* api_ = nullptr;
};

}  // namespace acdb
