cmake_minimum_required(VERSION 3.15)

project(mt5bridge_cpp LANGUAGES C CXX)

include(CTest)

option(MT5BRIDGE_BUILD_RUNTIME "Build the CPython-backed mt5_bridge.dll" ON)
option(BUILD_EXAMPLES "Build example programs" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Lightweight consumer target. Linking this target never searches for Python.
add_library(mt5bridge_client INTERFACE)
target_include_directories(mt5bridge_client INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
add_library(mt5bridge::client ALIAS mt5bridge_client)
set_target_properties(mt5bridge_client PROPERTIES EXPORT_NAME client)

# The concrete journal store is Windows-specific but remains independent of
# the CPython-backed runtime DLL. Consumers that need durable operation
# persistence link this small native target alongside mt5bridge::client.
add_library(mt5bridge_journal STATIC src/dispatch/file_journal_store.cpp)
target_include_directories(mt5bridge_journal PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
add_library(mt5bridge::journal ALIAS mt5bridge_journal)
set_target_properties(mt5bridge_journal PROPERTIES EXPORT_NAME journal)

# Private execution seam for the future Python/terminal adapter.  It is kept
# separate from the exported client and journal targets so no unmanaged send
# function becomes part of the consumer SDK.
add_library(mt5bridge_one_shot_backend STATIC
            src/dispatch/one_shot_backend.cpp)
target_include_directories(mt5bridge_one_shot_backend PRIVATE
                           ${PROJECT_SOURCE_DIR}/include
                           ${PROJECT_SOURCE_DIR}/src/dispatch)
add_library(mt5bridge::one_shot_backend ALIAS mt5bridge_one_shot_backend)

# Private runtime admission lanes keep trade-critical calls from waiting
# behind an unbounded queue of market-data work without changing the public ABI.
add_library(mt5bridge_runtime_lane STATIC src/runtime/runtime_lane.cpp)
target_include_directories(mt5bridge_runtime_lane PRIVATE
                           ${PROJECT_SOURCE_DIR}/src/runtime)
add_library(mt5bridge::runtime_lane ALIAS mt5bridge_runtime_lane)

if(BUILD_TESTING)
    add_executable(mt5bridge_reconciliation_graph_test tests/reconciliation_graph_test.cpp)
    target_link_libraries(mt5bridge_reconciliation_graph_test PRIVATE mt5bridge::client)
    add_test(NAME mt5bridge_reconciliation_graph
             COMMAND mt5bridge_reconciliation_graph_test)
    add_executable(mt5bridge_reconciliation_engine_test
                   tests/reconciliation_engine_test.cpp)
    target_link_libraries(mt5bridge_reconciliation_engine_test PRIVATE mt5bridge::client)
    add_test(NAME mt5bridge_reconciliation_engine
             COMMAND mt5bridge_reconciliation_engine_test)
    add_executable(mt5bridge_reconciliation_coordinator_test
                   tests/reconciliation_coordinator_test.cpp)
    target_link_libraries(mt5bridge_reconciliation_coordinator_test PRIVATE mt5bridge::client)
    add_test(NAME mt5bridge_reconciliation_coordinator
             COMMAND mt5bridge_reconciliation_coordinator_test)
    add_executable(mt5bridge_reconciliation_worker_test
                   tests/reconciliation_worker_test.cpp)
    target_link_libraries(mt5bridge_reconciliation_worker_test PRIVATE mt5bridge::client)
    add_test(NAME mt5bridge_reconciliation_worker
             COMMAND mt5bridge_reconciliation_worker_test)
    add_executable(mt5bridge_operation_reconciliation_worker_test
                   tests/operation_reconciliation_worker_test.cpp)
    target_link_libraries(mt5bridge_operation_reconciliation_worker_test PRIVATE
                          mt5bridge::client mt5bridge::journal)
    add_test(NAME mt5bridge_operation_reconciliation_worker
             COMMAND mt5bridge_operation_reconciliation_worker_test)
    add_executable(mt5bridge_environment_consistency_test
                   tests/environment_consistency_test.cpp)
    target_link_libraries(mt5bridge_environment_consistency_test PRIVATE mt5bridge::client)
    add_test(NAME mt5bridge_environment_consistency
             COMMAND mt5bridge_environment_consistency_test)
    add_executable(mt5bridge_dispatch_journal_test
                   tests/dispatch_journal_test.cpp)
    target_link_libraries(mt5bridge_dispatch_journal_test PRIVATE
                          mt5bridge::client mt5bridge::journal)
    add_test(NAME mt5bridge_dispatch_journal
             COMMAND mt5bridge_dispatch_journal_test)
    add_executable(mt5bridge_file_journal_store_test
                   tests/file_journal_store_test.cpp)
    target_link_libraries(mt5bridge_file_journal_store_test PRIVATE
                          mt5bridge::client mt5bridge::journal)
    add_test(NAME mt5bridge_file_journal_store
             COMMAND mt5bridge_file_journal_store_test)
    add_executable(mt5bridge_header_umbrellas_test
                   tests/header_umbrellas_test.cpp)
    target_link_libraries(mt5bridge_header_umbrellas_test PRIVATE
                          mt5bridge::client mt5bridge::journal)
    add_test(NAME mt5bridge_header_umbrellas
             COMMAND mt5bridge_header_umbrellas_test)
    add_executable(mt5bridge_header_self_containment_test
                   tests/header_self_containment_main.cpp
                   tests/header_self_containment_market_common.c
                   tests/header_self_containment_market_ticks.c
                   tests/header_self_containment_market_rates.c
                   tests/header_self_containment_market_realtime.c
                   tests/header_self_containment_trade_common.c
                   tests/header_self_containment_trade_account.c
                   tests/header_self_containment_trade_symbol.c
                   tests/header_self_containment_trade_order_check.c
                   tests/header_self_containment_trade_orders.c
                   tests/header_self_containment_trade_positions.c
                   tests/header_self_containment_trade_deals.c
                   tests/header_self_containment_reconciliation_graph.cpp
                   tests/header_self_containment_reconciliation_engine.cpp
                   tests/header_self_containment_reconciliation_coordinator.cpp
                   tests/header_self_containment_reconciliation_worker.cpp
                   tests/header_self_containment_dispatch_operation_worker.cpp
                   tests/header_self_containment_environment_consistency.cpp
                   tests/header_self_containment_dispatch_journal.cpp
                   tests/header_self_containment_file_journal_store.cpp)
    target_link_libraries(mt5bridge_header_self_containment_test PRIVATE
                          mt5bridge::client)
    add_test(NAME mt5bridge_header_self_containment
             COMMAND mt5bridge_header_self_containment_test)
    add_executable(mt5bridge_one_shot_backend_test
                   tests/one_shot_backend_test.cpp)
    target_include_directories(mt5bridge_one_shot_backend_test PRIVATE
                               ${PROJECT_SOURCE_DIR}/src/dispatch)
    target_link_libraries(mt5bridge_one_shot_backend_test PRIVATE
                          mt5bridge::client mt5bridge::journal
                          mt5bridge::one_shot_backend)
    add_test(NAME mt5bridge_one_shot_backend
             COMMAND mt5bridge_one_shot_backend_test)
    add_executable(mt5bridge_runtime_lane_test
                   tests/runtime_lane_test.cpp)
    target_include_directories(mt5bridge_runtime_lane_test PRIVATE
                               ${PROJECT_SOURCE_DIR}/src/runtime)
    target_link_libraries(mt5bridge_runtime_lane_test PRIVATE mt5bridge::runtime_lane)
    add_test(NAME mt5bridge_runtime_lane
             COMMAND mt5bridge_runtime_lane_test)
endif()

if(MT5BRIDGE_BUILD_RUNTIME)
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter Development)
    if(NOT Python3_VERSION_MAJOR EQUAL 3 OR
       NOT Python3_VERSION_MINOR EQUAL 11)
        message(FATAL_ERROR
            "mt5bridge runtime currently requires CPython 3.11.x; "
            "selected ${Python3_VERSION} at ${Python3_EXECUTABLE}")
    endif()

    add_library(mt5_bridge SHARED
                src/bridge/mt5_bridge.cpp
                src/trade/python_dispatch_transport.cpp)
    target_include_directories(mt5_bridge PRIVATE
                               ${PROJECT_SOURCE_DIR}/include
                               ${PROJECT_SOURCE_DIR}/src/bridge
                               ${PROJECT_SOURCE_DIR}/src/dispatch
                               ${PROJECT_SOURCE_DIR}/src/runtime
                               ${PROJECT_SOURCE_DIR}/src/trade)
    target_compile_definitions(mt5_bridge PRIVATE MT5BRIDGE_BUILD_DLL)
    target_link_libraries(mt5_bridge PRIVATE Python3::Python mt5bridge::runtime_lane)
    get_target_property(MT5BRIDGE_PYTHON_RUNTIME_RELEASE
                        Python3::Python IMPORTED_LOCATION_RELEASE)
    if(NOT MT5BRIDGE_PYTHON_RUNTIME_RELEASE)
        get_target_property(MT5BRIDGE_PYTHON_RUNTIME_RELEASE
                            Python3::Python IMPORTED_LOCATION)
    endif()
    if(MT5BRIDGE_PYTHON_RUNTIME_RELEASE)
        add_custom_command(TARGET mt5_bridge POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MT5BRIDGE_PYTHON_RUNTIME_RELEASE}"
                    "$<TARGET_FILE_DIR:mt5_bridge>"
            COMMENT "Staging the Python runtime beside mt5_bridge.dll")
    endif()

    if(BUILD_TESTING)
        target_compile_definitions(mt5_bridge PRIVATE MT5BRIDGE_INTERNAL_TESTING)
        add_executable(mt5bridge_c_header_compile tests/abi_compile_test.c)
        target_include_directories(mt5bridge_c_header_compile PRIVATE ${PROJECT_SOURCE_DIR}/include)
        add_test(NAME mt5bridge_c_header_compile COMMAND mt5bridge_c_header_compile)
        add_test(NAME mt5bridge_runtime_preparer
                 COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_prepare_py_runtime -v)
        set_tests_properties(mt5bridge_runtime_preparer PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
        add_executable(mt5bridge_owned_runtime_smoke tests/owned_runtime_smoke.cpp)
        target_link_libraries(mt5bridge_owned_runtime_smoke PRIVATE mt5bridge::client)
        add_executable(mt5bridge_owned_realtime_smoke tests/owned_realtime_smoke.cpp)
        target_link_libraries(mt5bridge_owned_realtime_smoke PRIVATE mt5bridge::client)
        set(MT5BRIDGE_PYTHON_RUNTIME_DIRS "${Python3_RUNTIME_LIBRARY_DIRS}")
        if(NOT MT5BRIDGE_PYTHON_RUNTIME_DIRS)
            get_filename_component(MT5BRIDGE_PYTHON_RUNTIME_DIRS
                                   "${Python3_EXECUTABLE}" DIRECTORY)
        endif()
        list(GET MT5BRIDGE_PYTHON_RUNTIME_DIRS 0 MT5BRIDGE_PYTHON_RUNTIME_DIR)
        set(MT5BRIDGE_TEST_PATH
            "${MT5BRIDGE_PYTHON_RUNTIME_DIR};$ENV{PATH}")
        add_test(
            NAME mt5bridge_owned_runtime
            COMMAND ${CMAKE_COMMAND} -E env
                    "PYTHONPATH=${PROJECT_SOURCE_DIR}/tests/fake_lifecycle_module"
                    "PYTHONHOME=${MT5BRIDGE_PYTHON_RUNTIME_DIR}"
                    "PATH=${MT5BRIDGE_TEST_PATH}"
                    "$<TARGET_FILE:mt5bridge_owned_runtime_smoke>"
                    "$<TARGET_FILE:mt5_bridge>"
        )
        set_tests_properties(mt5bridge_owned_runtime PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        )
        add_test(
            NAME mt5bridge_owned_realtime
            COMMAND ${CMAKE_COMMAND} -E env
                    "PYTHONPATH=${PROJECT_SOURCE_DIR}/tests/fake_mt5_module"
                    "PYTHONHOME=${MT5BRIDGE_PYTHON_RUNTIME_DIR}"
                    "PATH=${MT5BRIDGE_TEST_PATH}"
                    "$<TARGET_FILE:mt5bridge_owned_realtime_smoke>"
                    "$<TARGET_FILE:mt5_bridge>"
        )
        set_tests_properties(mt5bridge_owned_realtime PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        )
        add_test(
            NAME mt5bridge_fake_runtime
            COMMAND ${CMAKE_COMMAND} -E env
                    "MT5BRIDGE_DLL=$<TARGET_FILE:mt5_bridge>"
                    "${Python3_EXECUTABLE}" -m unittest tests.test_fake_mt5_runtime -v
        )
        set_tests_properties(mt5bridge_fake_runtime PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        )
        add_test(
            NAME mt5bridge_python_adapter
            COMMAND ${CMAKE_COMMAND} -E env
                    "PYTHONPATH=${PROJECT_SOURCE_DIR}/python"
                    "MT5BRIDGE_DLL=$<TARGET_FILE:mt5_bridge>"
                    "PATH=${MT5BRIDGE_PYTHON_RUNTIME_DIR};$<TARGET_FILE_DIR:mt5_bridge>;$ENV{PATH}"
                    "${Python3_EXECUTABLE}" -m unittest tests.test_python_adapter -v
        )
        set_tests_properties(mt5bridge_python_adapter PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        )
        add_test(
            NAME mt5bridge_trade_observation
            COMMAND ${CMAKE_COMMAND} -E env
                    "MT5BRIDGE_DLL=$<TARGET_FILE:mt5_bridge>"
                    "${Python3_EXECUTABLE}" -m unittest tests.test_trade_observation -v
        )
        set_tests_properties(mt5bridge_trade_observation PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        )
    endif()
endif()

install(TARGETS mt5bridge_client mt5bridge_journal EXPORT mt5bridgeTargets
        ARCHIVE DESTINATION lib)
if(MT5BRIDGE_BUILD_RUNTIME)
    install(TARGETS mt5_bridge RUNTIME DESTINATION bin LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
endif()
install(DIRECTORY include/ DESTINATION include FILES_MATCHING
    PATTERN "*.h" PATTERN "*.hpp")
install(EXPORT mt5bridgeTargets NAMESPACE mt5bridge:: DESTINATION lib/cmake/mt5bridge)
include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/mt5bridgeConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/mt5bridgeConfig.cmake"
    INSTALL_DESTINATION lib/cmake/mt5bridge)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/mt5bridgeConfigVersion.cmake"
    VERSION 0.1.0 COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/mt5bridgeConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/mt5bridgeConfigVersion.cmake"
    DESTINATION lib/cmake/mt5bridge)

if(BUILD_EXAMPLES)
    add_executable(usage_example examples/usage_example.cpp)
    target_link_libraries(usage_example PRIVATE mt5bridge::client)
    set_target_properties(usage_example PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

    add_executable(smoke_no_mt5 examples/smoke_no_mt5.cpp)
    target_link_libraries(smoke_no_mt5 PRIVATE mt5bridge::client)
    set_target_properties(smoke_no_mt5 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

    add_executable(live_market_smoke examples/live_market_smoke.cpp)
    target_link_libraries(live_market_smoke PRIVATE mt5bridge::client)
    set_target_properties(live_market_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

    add_executable(trade_observation_example examples/trade_observation_example.cpp)
    target_link_libraries(trade_observation_example PRIVATE mt5bridge::client)
    set_target_properties(trade_observation_example PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
endif()
