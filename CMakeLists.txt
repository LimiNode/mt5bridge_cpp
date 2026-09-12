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

if(MT5BRIDGE_BUILD_RUNTIME)
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter Development)

    add_library(mt5_bridge SHARED src/runtime/mt5_bridge.cpp)
    target_include_directories(mt5_bridge PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(mt5_bridge PRIVATE MT5BRIDGE_BUILD_DLL)
    target_link_libraries(mt5_bridge PRIVATE Python3::Python)
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
    endif()
endif()

install(TARGETS mt5bridge_client EXPORT mt5bridgeTargets)
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
endif()
