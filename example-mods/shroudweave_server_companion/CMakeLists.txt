cmake_minimum_required(VERSION 3.20)
project(shroudweave_server_companion LANGUAGES CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Windows x64 is required")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(SHROUDWEAVE_JSON_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../third-party/nlohmann-json/include" CACHE PATH "Pinned nlohmann/json include root")
add_library(shroudweave_inventory STATIC src/inventory.cpp)
target_include_directories(shroudweave_inventory PUBLIC include "${SHROUDWEAVE_JSON_INCLUDE}")
target_compile_options(shroudweave_inventory PRIVATE /W4 /WX /EHsc)
add_executable(shroudweave_inventory_tests tests/inventory_tests.cpp)
target_link_libraries(shroudweave_inventory_tests PRIVATE shroudweave_inventory)
target_compile_options(shroudweave_inventory_tests PRIVATE /W4 /WX /EHsc)
enable_testing()
add_test(NAME shroudweave_inventory COMMAND shroudweave_inventory_tests)

# Connection identity and epoch tracking are generic server-host facilities.
add_executable(shroudweave_connection_registry_tests
    tests/connection_registry_tests.cpp src/connection_registry.cpp)
target_include_directories(shroudweave_connection_registry_tests PRIVATE include)
target_link_libraries(shroudweave_connection_registry_tests PRIVATE shroudweave_connected_authority)
target_compile_options(shroudweave_connection_registry_tests PRIVATE /W4 /WX /EHsc)
add_test(NAME shroudweave_connection_registry COMMAND shroudweave_connection_registry_tests)

# The early-entry fixture is a source port of the private ESM Loader gate.
# The fixture exercises the gate separately from the production host.
enable_language(ASM_MASM)
add_library(shroudweave_gate_fixture SHARED entry/dedicated_entry_gate.cpp
    entry/dedicated_entry_thunk.asm entry/dedicated_dbghelp_proxy.cpp
    entry/dedicated_dbghelp_proxy.def)
set_target_properties(shroudweave_gate_fixture PROPERTIES OUTPUT_NAME dbghelp
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gate-fixture"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gate-fixture")
target_compile_definitions(shroudweave_gate_fixture PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX ESM_DEDICATED_GATE_FIXTURES)
target_compile_options(shroudweave_gate_fixture PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/GS-;/Oi;/Zl>")
target_link_options(shroudweave_gate_fixture PRIVATE /ENTRY:DllMain /NODEFAULTLIB /INCREMENTAL:NO)
target_link_libraries(shroudweave_gate_fixture PRIVATE kernel32)
add_executable(shroudweave_gate_fixture_exe tests/dedicated_entry_fixture.cpp entry/dedicated_entry_thunk.asm)
target_compile_definitions(shroudweave_gate_fixture_exe PRIVATE ESM_DEDICATED_FIXTURE_EXE)
target_compile_options(shroudweave_gate_fixture_exe PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/GS-;/Oi;/Zl>")
target_link_options(shroudweave_gate_fixture_exe PRIVATE /ENTRY:FixtureEntry /NODEFAULTLIB /INCREMENTAL:NO)
target_link_libraries(shroudweave_gate_fixture_exe PRIVATE kernel32 shroudweave_gate_fixture)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(case IN ITEMS ready failed missing mismatch foreign-byte wrong-thread forward xstate unrelated)
    add_test(NAME shroudweave_gate_${case} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/dedicated_entry_tests.py"
        "${case}" "$<TARGET_FILE:shroudweave_gate_fixture_exe>" --proxy "$<TARGET_FILE:shroudweave_gate_fixture>")
    set_tests_properties(shroudweave_gate_${case} PROPERTIES TIMEOUT 25)
endforeach()

# Synthetic bridge host tests the private startup gate against module-owned
# direct-call commitments. These fixtures are not production server inputs.
add_library(shroudweave_gate_bridge_fixture SHARED
    entry/dedicated_entry_gate.cpp entry/dedicated_entry_thunk.asm
    entry/dedicated_dbghelp_proxy.cpp entry/dedicated_dbghelp_proxy.def
    tests/dedicated_entry_host_fixture.cpp)
set_target_properties(shroudweave_gate_bridge_fixture PROPERTIES OUTPUT_NAME dbghelp
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gate-bridge-fixture"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gate-bridge-fixture")
target_include_directories(shroudweave_gate_bridge_fixture PRIVATE entry)
target_compile_definitions(shroudweave_gate_bridge_fixture PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    ESM_DEDICATED_GATE_FIXTURES ESM_DEDICATED_NATIVE_BRIDGE ESM_DEDICATED_HOST_BRIDGE_FIXTURES)
target_compile_options(shroudweave_gate_bridge_fixture PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/GS-;/Oi;/Zl>")
target_link_options(shroudweave_gate_bridge_fixture PRIVATE /ENTRY:DllMain /NODEFAULTLIB /INCREMENTAL:NO)
target_link_libraries(shroudweave_gate_bridge_fixture PRIVATE kernel32 bcrypt)

add_library(shroudweave_startup_owner_fixture SHARED tests/dedicated_entry_owner_fixture.cpp)
set_target_properties(shroudweave_startup_owner_fixture PROPERTIES OUTPUT_NAME shroudweave_startup_owner
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/startup-owner"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/startup-owner")
target_compile_definitions(shroudweave_startup_owner_fixture PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(shroudweave_startup_owner_fixture PRIVATE /W4 /WX /EHsc)
target_link_options(shroudweave_startup_owner_fixture PRIVATE /DYNAMICBASE:NO)

add_executable(shroudweave_gate_bridge_fixture_exe
    tests/dedicated_entry_fixture.cpp tests/dedicated_entry_call_sites.asm
    entry/dedicated_entry_thunk.asm)
target_compile_definitions(shroudweave_gate_bridge_fixture_exe PRIVATE
    ESM_DEDICATED_FIXTURE_EXE ESM_DEDICATED_HOST_BRIDGE_FIXTURES)
target_compile_options(shroudweave_gate_bridge_fixture_exe PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/GS-;/Oi;/Zl>")
target_link_options(shroudweave_gate_bridge_fixture_exe PRIVATE /ENTRY:FixtureEntry /NODEFAULTLIB /INCREMENTAL:NO)
target_link_options(shroudweave_gate_bridge_fixture_exe PRIVATE /DYNAMICBASE:NO)
target_link_libraries(shroudweave_gate_bridge_fixture_exe PRIVATE
    kernel32 shroudweave_gate_bridge_fixture)
add_dependencies(shroudweave_gate_bridge_fixture_exe shroudweave_startup_owner_fixture)
foreach(case IN ITEMS native-no-commit native-call native-mismatch native-foreign-owner
        native-private-site native-invalid-target native-bootstrap-site native-capacity native-relay
        native-bad-relay native-overlap native-site-tamper native-relay-tamper)
    add_test(NAME shroudweave_gate_bridge_${case}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/dedicated_entry_tests.py"
            "${case}" "$<TARGET_FILE:shroudweave_gate_bridge_fixture_exe>"
            --proxy "$<TARGET_FILE:shroudweave_gate_bridge_fixture>"
            --owner "$<TARGET_FILE:shroudweave_startup_owner_fixture>")
    set_tests_properties(shroudweave_gate_bridge_${case} PROPERTIES TIMEOUT 25)
endforeach()

# Standalone Shroudtopia-owned dbghelp proxy with the generic module host.
add_library(shroudweave_server_companion SHARED
    entry/dedicated_entry_gate.cpp entry/dedicated_entry_thunk.asm
    entry/dedicated_dbghelp_proxy.cpp entry/dedicated_dbghelp_proxy.def
    entry/dbghelp_forward.cpp entry/legacy_loader.cpp entry/server_modules.cpp
    entry/shroudweave_host.cpp)
set_target_properties(shroudweave_server_companion PROPERTIES OUTPUT_NAME dbghelp)
target_include_directories(shroudweave_server_companion PRIVATE include entry)
target_compile_definitions(shroudweave_server_companion PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX ESM_DEDICATED_NATIVE_BRIDGE)
target_compile_options(shroudweave_server_companion PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/EHsc;/guard:cf>")
target_link_options(shroudweave_server_companion PRIVATE /GUARD:CF /DYNAMICBASE /INCREMENTAL:NO)
target_link_libraries(shroudweave_server_companion PRIVATE shroudweave_inventory bcrypt)

# Import the production proxy during process start from a non-server test EXE.
add_executable(shroudweave_client_passthrough_fixture tests/client_passthrough_fixture.cpp)
target_include_directories(shroudweave_client_passthrough_fixture PRIVATE entry)
target_compile_definitions(shroudweave_client_passthrough_fixture PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(shroudweave_client_passthrough_fixture PRIVATE /W4 /WX /EHsc)
target_link_libraries(shroudweave_client_passthrough_fixture PRIVATE shroudweave_server_companion)
add_test(NAME shroudweave_client_passthrough COMMAND shroudweave_client_passthrough_fixture)

add_test(NAME shroudweave_production_refusal COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/production_refusal_tests.py"
    "$<TARGET_FILE:shroudweave_client_passthrough_fixture>"
    "$<TARGET_FILE:shroudweave_server_companion>"
    "${CMAKE_CURRENT_BINARY_DIR}/production-refusal-fixture")
set_tests_properties(shroudweave_production_refusal PROPERTIES TIMEOUT 20)

# Exercise an opt-in downstream dbghelp proxy from a non-server process. The
# production proxy is already loaded when the nested, same-name DLL is opened.
add_library(shroudweave_proxy_chain_fixture SHARED tests/proxy_chain_fixture.cpp)
set_target_properties(shroudweave_proxy_chain_fixture PROPERTIES OUTPUT_NAME dbghelp
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/chain-test-root/proxy-chain"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/chain-test-root/proxy-chain"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/chain-test-root/proxy-chain")
target_compile_definitions(shroudweave_proxy_chain_fixture PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(shroudweave_proxy_chain_fixture PRIVATE /W4 /WX /EHsc)

# Native replacement for the external Modloader Proxy binary.
add_executable(shroudweave_legacy_loader_tests tests/legacy_loader_tests.cpp
    entry/legacy_loader.cpp entry/dbghelp_forward.cpp)
target_include_directories(shroudweave_legacy_loader_tests PRIVATE entry)
target_compile_definitions(shroudweave_legacy_loader_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(shroudweave_legacy_loader_tests PRIVATE /W4 /WX /EHsc)
add_test(NAME shroudweave_legacy_loader COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_loader_tests.py"
    "$<TARGET_FILE:shroudweave_legacy_loader_tests>"
    "$<TARGET_FILE:shroudweave_proxy_chain_fixture>"
    "$<TARGET_FILE:shroudweave_proxy_chain_fixture>"
    "${CMAKE_CURRENT_BINARY_DIR}/legacy-loader-tests")
set_tests_properties(shroudweave_legacy_loader PROPERTIES TIMEOUT 40)

# Generic connection authorization primitive; modules select their own policy.
add_library(shroudweave_connected_authority STATIC src/connected_authority.cpp)
target_include_directories(shroudweave_connected_authority PUBLIC include)
target_compile_options(shroudweave_connected_authority PRIVATE /W4 /WX /EHsc)
add_executable(shroudweave_connected_authority_tests tests/connected_authority_tests.cpp)
target_link_libraries(shroudweave_connected_authority_tests PRIVATE shroudweave_connected_authority)
target_compile_options(shroudweave_connected_authority_tests PRIVATE /W4 /WX /EHsc)
add_test(NAME shroudweave_connected_authority COMMAND shroudweave_connected_authority_tests)

# Source reuse only: the standalone build does not load Shroudweave Loader.
add_library(shroudweave_settings_store STATIC shared/esm/settings_store.cpp)
target_include_directories(shroudweave_settings_store PUBLIC shared/esm/include "${SHROUDWEAVE_JSON_INCLUDE}")
target_compile_options(shroudweave_settings_store PRIVATE /W4 /WX /EHsc)
add_library(shroudweave_shared_settings STATIC src/shared_settings.cpp)
target_include_directories(shroudweave_shared_settings PUBLIC include)
target_link_libraries(shroudweave_shared_settings PUBLIC shroudweave_connected_authority shroudweave_settings_store)
target_compile_options(shroudweave_shared_settings PRIVATE /W4 /WX /EHsc)
add_library(shroudweave_server_services_runtime STATIC src/server_services_runtime.cpp)
target_include_directories(shroudweave_server_services_runtime PUBLIC include)
target_compile_definitions(shroudweave_server_services_runtime PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(shroudweave_server_services_runtime PUBLIC shroudweave_shared_settings)
target_compile_options(shroudweave_server_services_runtime PRIVATE /W4 /WX /EHsc)
add_executable(shroudweave_shared_settings_tests tests/shared_settings_tests.cpp)
target_link_libraries(shroudweave_shared_settings_tests PRIVATE shroudweave_shared_settings)
target_compile_options(shroudweave_shared_settings_tests PRIVATE /W4 /WX /EHsc)
add_test(NAME shroudweave_shared_settings COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_settings_tests.py" "$<TARGET_FILE:shroudweave_shared_settings_tests>"
    "${CMAKE_CURRENT_BINARY_DIR}/shared-settings-tests")
set_tests_properties(shroudweave_shared_settings PROPERTIES TIMEOUT 20)

enable_language(C)
add_executable(shroudweave_server_services_abi_tests tests/server_services_abi_tests.c)
target_include_directories(shroudweave_server_services_abi_tests PRIVATE include)
target_compile_features(shroudweave_server_services_abi_tests PRIVATE c_std_11)
add_test(NAME shroudweave_server_services_abi COMMAND shroudweave_server_services_abi_tests)
add_executable(shroudweave_server_services_tests tests/server_services_runtime_tests.cpp)
target_link_libraries(shroudweave_server_services_tests PRIVATE shroudweave_server_services_runtime)
target_compile_definitions(shroudweave_server_services_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(shroudweave_server_services_tests PRIVATE /W4 /WX /EHsc)
add_test(NAME shroudweave_server_services COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_settings_tests.py"
    "$<TARGET_FILE:shroudweave_server_services_tests>"
    "${CMAKE_CURRENT_BINARY_DIR}/server-services-runtime-tests")
set_tests_properties(shroudweave_server_services PROPERTIES TIMEOUT 30)
add_executable(shroudweave_startup_services_tests
    tests/startup_services_tests.cpp entry/startup_services.cpp)
target_include_directories(shroudweave_startup_services_tests PRIVATE include entry)
target_compile_definitions(shroudweave_startup_services_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(shroudweave_startup_services_tests PRIVATE /W4 /WX /EHsc)
foreach(case IN ITEMS success empty-startup validation-failure invalid-view failed-module
        service-failure retired-context foreign-thread reentrant context-capacity invalid-size
        commit-capacity)
    add_test(NAME shroudweave_startup_services_${case}
        COMMAND shroudweave_startup_services_tests "${case}")
    set_tests_properties(shroudweave_startup_services_${case} PROPERTIES TIMEOUT 10)
endforeach()

# Exercise the real generic host lifecycle with an offline module and test seams.
add_executable(shroudweave_host_integration_tests
    tests/host_integration_tests.cpp
    entry/shroudweave_host.cpp entry/server_modules.cpp entry/shared_runtime.cpp
    entry/startup_services.cpp src/connection_registry.cpp)
target_include_directories(shroudweave_host_integration_tests PRIVATE
    include entry native transport shared/esm/include
    ${SHROUDWEAVE_JSON_INCLUDE} ../../third-party/steam)
target_compile_definitions(shroudweave_host_integration_tests PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX SHROUDWEAVE_HOST_TEST_SEAMS)
target_compile_options(shroudweave_host_integration_tests PRIVATE /W4 /WX /EHsc)
target_link_libraries(shroudweave_host_integration_tests PRIVATE
    shroudweave_server_services_runtime shroudweave_inventory bcrypt)

add_library(shroudweave_host_fixture_proxy SHARED tests/host_integration_proxy.cpp)
set_target_properties(shroudweave_host_fixture_proxy PROPERTIES
    OUTPUT_NAME dbghelp
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/host-fixture-proxy"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/host-fixture-proxy")
target_compile_definitions(shroudweave_host_fixture_proxy PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(shroudweave_host_fixture_proxy PRIVATE /W4 /WX /EHsc)

add_library(shroudweave_host_smoke_module SHARED tests/host_integration_module.cpp)
set_target_properties(shroudweave_host_smoke_module PROPERTIES
    OUTPUT_NAME shroudweave_host_smoke)
target_include_directories(shroudweave_host_smoke_module PRIVATE
    include shared/esm/include ${SHROUDWEAVE_JSON_INCLUDE})
target_compile_definitions(shroudweave_host_smoke_module PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(shroudweave_host_smoke_module PRIVATE /W4 /WX /EHsc)
add_dependencies(shroudweave_host_integration_tests
    shroudweave_host_fixture_proxy shroudweave_host_smoke_module)
foreach(case IN ITEMS success optional-absent optional-failure required-failure badsha wrongfile missingrequired)
    add_test(NAME shroudweave_host_integration_${case}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/host_integration_tests.py"
            "$<TARGET_FILE:shroudweave_host_integration_tests>"
            "$<TARGET_FILE:shroudweave_host_fixture_proxy>"
            "$<TARGET_FILE:shroudweave_host_smoke_module>" "${case}")
    set_tests_properties(shroudweave_host_integration_${case} PROPERTIES TIMEOUT 40)
endforeach()

# Final-validation fixtures consume the host, proxy and smoke-module targets above.
add_subdirectory(tests/final-validation)
add_subdirectory(tests/forward-ready)

add_subdirectory(vendor/minhook-1.3.4 EXCLUDE_FROM_ALL)
target_sources(shroudweave_server_companion PRIVATE
    native/connections.cpp src/connection_registry.cpp entry/shared_runtime.cpp entry/startup_services.cpp)
target_include_directories(shroudweave_server_companion PRIVATE native transport ../../third-party/steam)
target_link_libraries(shroudweave_server_companion PRIVATE shroudweave_server_services_runtime minhook)
add_library(shroudweave_services_client SHARED transport/client.cpp)
target_include_directories(shroudweave_services_client PRIVATE include transport ../../third-party/steam)
target_compile_definitions(shroudweave_services_client PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(shroudweave_services_client PRIVATE shroudweave_shared_settings)
target_compile_options(shroudweave_services_client PRIVATE /W4 /WX /EHsc)
