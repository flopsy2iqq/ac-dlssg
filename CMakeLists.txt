cmake_minimum_required(VERSION 3.24)
project(ac_dlssg_bridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT everywhere: with ReShade's ProxyLibrary set, a DLL that fails to
# load crashes the game at start, so the bridge must not depend on the VC++
# redistributable (spec 6.1).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_options(/W4 /permissive- /utf-8 /EHsc)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
                        ACDB_VERSION="${PROJECT_VERSION}")

# Everything except the DLL entry points lives in a static library so that the
# tests can link it directly. It must not import d3d11/d3d12/dxgi: those are
# resolved at runtime (spec 6.1).
add_library(acdb_core STATIC
    src/log.cpp
    src/ini.cpp
    src/config.cpp
    src/compat.cpp
    src/gpu_info.cpp
    src/present_flags.cpp
    src/latency_semaphore.cpp
    src/internal_call.cpp
    src/system_dxgi.cpp
    src/vtable_patch.cpp
    src/factory_hook.cpp
    src/hidden_chain.cpp
    src/fence_pair.cpp
    src/stall_watchdog.cpp
    src/gpu_timers.cpp
    src/d3d12_presenter.cpp
)
target_include_directories(acdb_core PUBLIC src)

enable_testing()
add_executable(acdb_tests
    tests/test_main.cpp
    tests/test_ini_config.cpp
    tests/test_compat.cpp
    tests/test_gpu_info.cpp
    tests/test_present_flags.cpp
    tests/test_latency_semaphore.cpp
    tests/test_vtable_patch.cpp
    tests/test_hidden_chain.cpp
    tests/test_fence_pair.cpp
    tests/test_stall_watchdog.cpp
    tests/test_presenter.cpp
    tests/gpu_test_devices.cpp
)
# version: only the tests compare against VERSION.dll; the bridge never imports it.
target_link_libraries(acdb_tests PRIVATE acdb_core d3d11 d3d12 dxgi version)
add_test(NAME unit COMMAND acdb_tests)
# The D3D runtime keeps a shader-cache folder per test executable under
# %LOCALAPPDATA%\D3DSCache; it is removed after the GPU tests.
add_test(NAME clean_shader_cache COMMAND powershell -NoProfile -ExecutionPolicy Bypass
         -File "${CMAKE_SOURCE_DIR}/tools/clean-shader-cache.ps1")
set_tests_properties(clean_shader_cache PROPERTIES FIXTURES_CLEANUP d3d_shader_cache)
set_tests_properties(unit PROPERTIES FIXTURES_REQUIRED d3d_shader_cache)
