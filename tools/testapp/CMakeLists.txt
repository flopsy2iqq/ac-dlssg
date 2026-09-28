# Test application (spec 11): a stand-in for Assetto Corsa with CSP that loads
# ac-dlssg.dll the way ReShade loads its ProxyLibrary. It deliberately
# does not link acdb_core: like the game, it only sees the DLL's exports.
add_executable(testapp main.cpp)
target_link_libraries(testapp PRIVATE d3d11)

# An explicit directory, so that the copy step below needs no generator
# expression naming testapp (which would make it depend on testapp). With a
# generator expression in the path, multi-config generators add no
# per-configuration subdirectory of their own.
set(ACDB_TESTAPP_DIR "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
set_target_properties(testapp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ACDB_TESTAPP_DIR}")

# The bridge DLL and the fixtures sit next to testapp.exe, as the DLL and the
# CSP config sit next to acs.exe. A separate always-run target rather than a
# POST_BUILD step, so that a rebuilt DLL is copied even when testapp.exe is up
# to date. The staged Streamline DLLs (ACDB_SL_DLLS, top-level CMakeLists)
# go to ac-dlssg\sl, where the bootstrap loads them as from <game>\ac-dlssg\sl;
# without them every chain passes through.
set(ACDB_TESTAPP_SL_COMMANDS)
if(ACDB_SL_DLLS)
    set(ACDB_TESTAPP_SL_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ACDB_TESTAPP_DIR}/ac-dlssg/sl"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${ACDB_SL_DLLS} "${ACDB_TESTAPP_DIR}/ac-dlssg/sl")
endif()
add_custom_target(testapp_payload ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ACDB_TESTAPP_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:ac-dlssg>" "${ACDB_TESTAPP_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/fixtures" "${ACDB_TESTAPP_DIR}"
    ${ACDB_TESTAPP_SL_COMMANDS}
    COMMENT "Copying ac-dlssg.dll, the Streamline DLLs and the test fixtures next to testapp.exe"
    VERBATIM)
add_dependencies(testapp_payload ac-dlssg)
add_dependencies(testapp testapp_payload)
