# Extra lifecycle cases for the real host/coordinator integration fixture.
# Include this directory after the host integration executable and fixtures exist.
foreach(case IN ITEMS final-callback-failure final-callback-throw
        final-duplicate-registration final-foreign-thread-registration
        final-foreign-root-registration final-reentrant-registration
        final-two-module-order)
    add_test(NAME shroudweave_${case}
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/../host_integration_tests.py"
            "$<TARGET_FILE:shroudweave_host_integration_tests>"
            "$<TARGET_FILE:shroudweave_host_fixture_proxy>"
            "$<TARGET_FILE:shroudweave_host_smoke_module>" "${case}")
    set_tests_properties(shroudweave_${case} PROPERTIES TIMEOUT 40)
endforeach()
add_dependencies(shroudweave_startup_services_tests shroudweave_startup_owner_fixture)
add_test(NAME shroudweave_final_registered_count_poison
    COMMAND "$<TARGET_FILE:shroudweave_startup_services_tests>"
        final-registered-count-poison "$<TARGET_FILE:shroudweave_startup_owner_fixture>")
set_tests_properties(shroudweave_final_registered_count_poison PROPERTIES TIMEOUT 10)
add_test(NAME shroudweave_final_owner_stale_registration
    COMMAND "$<TARGET_FILE:shroudweave_startup_services_tests>"
        final-owner-stale-registration "$<TARGET_FILE:shroudweave_startup_owner_fixture>")
set_tests_properties(shroudweave_final_owner_stale_registration PROPERTIES TIMEOUT 10)
