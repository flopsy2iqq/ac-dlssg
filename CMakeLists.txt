cmake_minimum_required(VERSION 3.24)
project(ac_dlssg_bridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT everywhere: with ReShade's ProxyLibrary set, a DLL that fails to
# load crashes the game at start, so the bridge must not depend on the VC++
# redistributable (spec 6.1).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_options(/W4 /permissive- /utf-8 /EHsc)
# Third-party and SDK headers are external: no warnings from them, /W4 stays
# clean in our own files.
add_compile_options(/external:anglebrackets /external:W0)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
                        ACDB_VERSION="${PROJECT_VERSION}")

# Third-party headers staged by tools/fetch-deps.ps1 (never committed): the
# pinned Streamline SDK headers and the NVAPI headers. The DLLs themselves are
# loaded at runtime by absolute path, never linked.
set(ACDB_DEPS_DIR "${CMAKE_SOURCE_DIR}/deps" CACHE PATH
    "Folder with streamline-2.14.1/include and nvapi, staged by tools/fetch-deps.ps1")
set(ACDB_SL_INCLUDE_DIR "${ACDB_DEPS_DIR}/streamline-2.14.1/include")
set(ACDB_NVAPI_INCLUDE_DIR "${ACDB_DEPS_DIR}/nvapi")
# Every header the build includes, so a partial staging fails here and not
# in the compiler.
set(ACDB_REQUIRED_HEADERS)
foreach(h sl.h sl_dlss_g.h sl_pcl.h sl_reflex.h sl_security.h
          sl_struct.h sl_consts.h sl_version.h sl_result.h sl_appidentity.h sl_device_wrappers.h
          sl_core_api.h sl_core_types.h)
    list(APPEND ACDB_REQUIRED_HEADERS "${ACDB_SL_INCLUDE_DIR}/${h}")
endforeach()
foreach(h nvapi.h NvApiDriverSettings.h nvapi_lite_common.h nvapi_lite_salstart.h nvapi_lite_salend.h
          nvapi_lite_sli.h nvapi_lite_surround.h nvapi_lite_stereo.h nvapi_lite_d3dext.h)
    list(APPEND ACDB_REQUIRED_HEADERS "${ACDB_NVAPI_INCLUDE_DIR}/${h}")
endforeach()
foreach(required ${ACDB_REQUIRED_HEADERS})
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing ${required}.\n"
            "Stage the third-party headers first:\n"
            "  powershell -NoProfile -ExecutionPolicy Bypass -File tools\\fetch-deps.ps1\n"
            "or point ACDB_DEPS_DIR at a folder that holds streamline-2.14.1/include and nvapi.")
    endif()
endforeach()

# Shaders are compiled at build time with fxc from the Windows SDK into headers
# (fxc /Fh), so the bridge needs no shader compiler at runtime. fxc is looked
# up in the SDK version this build targets first, then in the newest SDK
# present, then on PATH.
set(ACDB_PF86 "ProgramFiles(x86)")
set(ACDB_KITS_BIN "$ENV{${ACDB_PF86}}/Windows Kits/10/bin")
set(ACDB_FXC_HINTS)
if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    list(APPEND ACDB_FXC_HINTS "${ACDB_KITS_BIN}/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
endif()
file(GLOB ACDB_KITS_VERSIONS LIST_DIRECTORIES true "${ACDB_KITS_BIN}/10.*")
list(SORT ACDB_KITS_VERSIONS COMPARE NATURAL ORDER DESCENDING)
foreach(v ${ACDB_KITS_VERSIONS})
    list(APPEND ACDB_FXC_HINTS "${v}/x64")
endforeach()
find_program(ACDB_FXC fxc HINTS ${ACDB_FXC_HINTS})
if(NOT ACDB_FXC)
    message(FATAL_ERROR "fxc.exe not found. Install the Windows 10/11 SDK, or set ACDB_FXC to its path.")
endif()
set(ACDB_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated")
# acdb_compile_cs(<name> <source.hlsl> <array name>): cs_5_0, entry point main,
# into ${ACDB_SHADER_OUT}/<name>.h as "const BYTE <array name>[]".
function(acdb_compile_cs name source var)
    set(out "${ACDB_SHADER_OUT}/${name}.h")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ACDB_SHADER_OUT}"
        COMMAND "${ACDB_FXC}" /nologo /T cs_5_0 /E main /O3 /WX /Qstrip_reflect /Qstrip_debug
                /Vn ${var} /Fh "${out}" "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        COMMENT "fxc cs_5_0 ${source}"
        VERBATIM)
    set(ACDB_SHADER_HEADERS ${ACDB_SHADER_HEADERS} "${out}" PARENT_SCOPE)
endfunction()
set(ACDB_SHADER_HEADERS)
acdb_compile_cs(depth_blit_cs shaders/depth_blit.hlsl kDepthBlitCs)

# Everything except the DLL entry points lives in a static library so that the
# tests can link it directly. It must not import d3d11/d3d12/dxgi: those are
# resolved at runtime (spec 6.1).
add_library(acdb_core STATIC
    src/log.cpp
    src/ini.cpp
    src/config.cpp
    src/compat.cpp
    src/gpu_info.cpp
    src/adapter_caps.cpp
    src/module_version.cpp
    src/present_flags.cpp
    src/latency_semaphore.cpp
    src/internal_call.cpp
    src/system_dxgi.cpp
    src/bootstrap.cpp
    src/vtable_patch.cpp
    src/inline_hook.cpp
    src/ngx_hook.cpp
    src/factory_hook.cpp
    src/hidden_chain.cpp
    src/fence_pair.cpp
    src/stall_watchdog.cpp
    src/gpu_timers.cpp
    src/d3d12_presenter.cpp
    src/proxy_swapchain.cpp
    src/pcl_sequencer.cpp
    src/spoof_loader.cpp
    src/streamline_runtime.cpp
    src/driver_profile.cpp
    src/camera_channel.cpp
    src/camera_layout.h
    src/frame_constants.cpp
    src/capture_slots.cpp
    src/fg_policy.cpp
    src/capture_coordinator.cpp
    ${ACDB_SHADER_HEADERS}
)
target_include_directories(acdb_core PUBLIC src)
# The generated shader headers; the tests include them too.
target_include_directories(acdb_core PUBLIC "${ACDB_SHADER_OUT}")
# Streamline types appear in streamline_runtime.h; NVAPI stays inside driver_profile.cpp.
target_include_directories(acdb_core SYSTEM PUBLIC "${ACDB_SL_INCLUDE_DIR}")
target_include_directories(acdb_core SYSTEM PRIVATE "${ACDB_NVAPI_INCLUDE_DIR}")

add_library(ac-dlssg SHARED
    src/dllmain.cpp
    src/dxgi_exports.cpp
    src/exports.def
)
target_link_libraries(ac-dlssg PRIVATE acdb_core)

enable_testing()
add_executable(acdb_tests
    tests/test_main.cpp
    tests/test_ini_config.cpp
    tests/test_compat.cpp
    tests/test_gpu_info.cpp
    tests/test_adapter_caps.cpp
    tests/test_present_flags.cpp
    tests/test_latency_semaphore.cpp
    tests/test_vtable_patch.cpp
    tests/test_inline_hook.cpp
    tests/test_ngx_hook.cpp
    tests/test_ngx_set_sink.cpp
    tests/test_testapp_fakes.cpp
    tests/test_testapp_logs.cpp
    tests/test_factory_hook.cpp
    tests/test_hidden_chain.cpp
    tests/test_fence_pair.cpp
    tests/test_stall_watchdog.cpp
    tests/test_presenter.cpp
    tests/test_proxy_swapchain.cpp
    tests/test_module_version.cpp
    tests/test_pcl_sequencer.cpp
    tests/test_spoof_loader.cpp
    tests/test_streamline_prefs.cpp
    tests/test_streamline_runtime.cpp
    tests/test_driver_profile.cpp
    tests/test_camera_channel.cpp
    tests/test_frame_constants.cpp
    tests/test_capture_slots.cpp
    tests/test_fg_policy.cpp
    tests/test_capture_coordinator.cpp
    tests/test_bridge_mode.cpp
    tests/test_dxgi_export_coverage.cpp
    tests/test_version.rc
    tests/gpu_test_devices.cpp
)
# version: only the tests compare against VERSION.dll; the bridge never imports it.
target_link_libraries(acdb_tests PRIVATE acdb_core d3d11 d3d12 dxgi version)
# The DLL-level test loads the built bridge DLL.
target_compile_definitions(acdb_tests PRIVATE ACDB_DLL_PATH="$<TARGET_FILE:ac-dlssg>")
# The camera tests read the CSP Lua app (apps/lua/AcDlssg) from the source tree.
target_compile_definitions(acdb_tests PRIVATE ACDB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# The test app's CSP stand-ins (tools/testapp/fake_csp.h) are tested here too.
target_include_directories(acdb_tests PRIVATE tests tools/testapp)
add_dependencies(acdb_tests ac-dlssg)
# An unsigned, loadable DLL for StreamlineRuntime's signature check: its
# DllMain leaves a mark, so a test can prove it was never loaded.
add_library(acdb_unsigned_fixture SHARED tests/unsigned_fixture.cpp)
target_compile_definitions(acdb_tests PRIVATE
    ACDB_UNSIGNED_FIXTURE_PATH="$<TARGET_FILE:acdb_unsigned_fixture>")
add_dependencies(acdb_tests acdb_unsigned_fixture)
# A harmless DLL the spoof-loader tests copy as version.dll; its DllMain
# leaves a mark so a test can prove it was loaded by full path.
add_library(acdb_spoof_fixture SHARED tests/spoof_fixture.cpp)
target_compile_definitions(acdb_tests PRIVATE
    ACDB_SPOOF_FIXTURE_PATH="$<TARGET_FILE:acdb_spoof_fixture>")
add_dependencies(acdb_tests acdb_spoof_fixture)

# A fake NGX module for the NgxHook tests: it exports the three D3D11 entry
# points with NVIDIA's signatures and records what its "original" was called
# with. A second module aliases two of those exports to one address so the
# skip rule can be tested against a real module.
add_library(fake_nvngx SHARED tests/fake_nvngx/fake_nvngx.cpp)
target_include_directories(fake_nvngx PRIVATE src)
add_library(fake_nvngx_alias SHARED
    tests/fake_nvngx/fake_nvngx_alias.cpp tests/fake_nvngx/fake_nvngx_alias.def)
target_compile_definitions(acdb_tests PRIVATE
    FAKE_NVNGX_PATH=L"$<TARGET_FILE:fake_nvngx>"
    FAKE_NVNGX_ALIAS_PATH=L"$<TARGET_FILE:fake_nvngx_alias>")
add_dependencies(acdb_tests fake_nvngx fake_nvngx_alias)
# Two builds of the fake with one fixed preferred base (no ASLR), the second
# with different code, so a test can put a different module at the address of
# a hooked one that was unloaded (ngx review F6).
foreach(variant a b)
    add_library(fake_nvngx_reuse_${variant} SHARED tests/fake_nvngx/fake_nvngx.cpp)
    target_include_directories(fake_nvngx_reuse_${variant} PRIVATE src)
    target_link_options(fake_nvngx_reuse_${variant} PRIVATE /DYNAMICBASE:NO /BASE:0x6E0000000000)
endforeach()
target_compile_definitions(fake_nvngx_reuse_b PRIVATE FAKE_NVNGX_VARIANT_B=1)
# The fake under the name of NVIDIA's DLSS NR snippet (a subfolder, so nothing
# finds it by search path), and a module whose NGX exports forward into it
# (ngx review F4).
add_library(fake_nvngx_dlssnr SHARED tests/fake_nvngx/fake_nvngx.cpp)
target_include_directories(fake_nvngx_dlssnr PRIVATE src)
set_target_properties(fake_nvngx_dlssnr PROPERTIES OUTPUT_NAME nvngx_dlssnr
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/fake_nr")
add_library(fake_nvngx_fwd SHARED tests/fake_nvngx/fake_nvngx_fwd.cpp)
target_compile_definitions(acdb_tests PRIVATE
    FAKE_NVNGX_DLSSNR_PATH=L"$<TARGET_FILE:fake_nvngx_dlssnr>"
    FAKE_NVNGX_FWD_PATH=L"$<TARGET_FILE:fake_nvngx_fwd>")
add_dependencies(acdb_tests fake_nvngx_dlssnr fake_nvngx_fwd)
target_compile_definitions(acdb_tests PRIVATE
    FAKE_NVNGX_REUSE_A_PATH=L"$<TARGET_FILE:fake_nvngx_reuse_a>"
    FAKE_NVNGX_REUSE_B_PATH=L"$<TARGET_FILE:fake_nvngx_reuse_b>")
add_dependencies(acdb_tests fake_nvngx_reuse_a fake_nvngx_reuse_b)
# The default location, set explicitly so the payload below can name it
# without a generator expression that names acdb_tests (see tools/testapp).
set(ACDB_TESTS_DIR "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
set_target_properties(acdb_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ACDB_TESTS_DIR}")
# With the Streamline DLLs staged too, a test also runs Init's signature check
# and pinned load on a real Streamline-signed file, and the runtime tests run
# Streamline itself from <exe dir>\sl, the layout of <game>\ac-dlssg\sl.
set(ACDB_SL_BIN_DIR "${ACDB_DEPS_DIR}/streamline-2.14.1/bin/x64")
if(EXISTS "${ACDB_SL_BIN_DIR}/sl.common.dll")
    target_compile_definitions(acdb_tests PRIVATE ACDB_SL_BIN_DIR="${ACDB_SL_BIN_DIR}")
    file(GLOB ACDB_SL_DLLS CONFIGURE_DEPENDS "${ACDB_SL_BIN_DIR}/*.dll")
    # Always run, like testapp_payload, so re-staged DLLs are copied even when
    # acdb_tests.exe is up to date.
    add_custom_target(acdb_tests_sl_payload ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ACDB_TESTS_DIR}/sl"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${ACDB_SL_DLLS} "${ACDB_TESTS_DIR}/sl"
        COMMENT "Copying the Streamline DLLs to <acdb_tests dir>/sl"
        VERBATIM)
    add_dependencies(acdb_tests acdb_tests_sl_payload)
endif()
add_test(NAME unit COMMAND acdb_tests)
# The import allow-list of spec 6.1 and 12.
add_test(NAME imports COMMAND powershell -NoProfile -ExecutionPolicy Bypass
         -File "${CMAKE_SOURCE_DIR}/tools/check-imports.ps1" -Dll "$<TARGET_FILE:ac-dlssg>")
# The D3D runtime keeps a shader-cache folder per test executable under
# %LOCALAPPDATA%\D3DSCache; it is removed after the GPU tests.
add_test(NAME clean_shader_cache COMMAND powershell -NoProfile -ExecutionPolicy Bypass
         -File "${CMAKE_SOURCE_DIR}/tools/clean-shader-cache.ps1")
set_tests_properties(clean_shader_cache PROPERTIES FIXTURES_CLEANUP d3d_shader_cache)
set_tests_properties(unit PROPERTIES FIXTURES_REQUIRED d3d_shader_cache)

add_subdirectory(tools/testapp)
