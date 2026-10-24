cmake_minimum_required(VERSION 3.24)
project(shroudweave_forward_ready_checks LANGUAGES CXX)
enable_testing()
add_executable(shroudweave_forward_ready_tests loader_tests.cpp)
target_compile_features(shroudweave_forward_ready_tests PRIVATE cxx_std_20)
target_compile_definitions(shroudweave_forward_ready_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(shroudweave_forward_ready_tests PRIVATE /W4 /WX /EHsc)
foreach(case IN ITEMS ready delayed failed missing-export foreign-export non-executable
        exception timeout late-success bad-timeout zero-timeout excessive-timeout
        missing-export-key missing-timeout-key duplicate-export invalid-export
        deferred-ready final-deferred missing-config missing-module retry-after-timeout missing-dependent
        unreadable-config nul-config nul-disappears)
 add_test(NAME shroudweave_forward_ready_${case}
  COMMAND shroudweave_forward_ready_tests "${case}" "${CMAKE_CURRENT_BINARY_DIR}/owned-fixtures")
 set_tests_properties(shroudweave_forward_ready_${case} PROPERTIES TIMEOUT 10)
endforeach()
