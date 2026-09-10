cmake_minimum_required(VERSION 3.15)

project(mt5bridge_cpp LANGUAGES CXX)

option(MT5BRIDGE_BUILD_RUNTIME "Build the CPython-backed mt5_bridge.dll" ON)
option(BUILD_EXAMPLES "Build example programs" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Lightweight consumer target. Linking this target never searches for Python.
add_library(mt5bridge_client INTERFACE)
target_include_directories(mt5bridge_client INTERFACE ${PROJECT_SOURCE_DIR}/include)
add_library(mt5bridge::client ALIAS mt5bridge_client)

if(MT5BRIDGE_BUILD_RUNTIME)
    find_package(Python3 REQUIRED COMPONENTS Development)

    add_library(mt5_bridge SHARED src/runtime/mt5_bridge.cpp)
    target_include_directories(mt5_bridge PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(mt5_bridge PRIVATE MT5BRIDGE_BUILD_DLL)
    target_link_libraries(mt5_bridge PRIVATE Python3::Python)
endif()

if(BUILD_EXAMPLES)
    add_executable(usage_example examples/usage_example.cpp)
    target_link_libraries(usage_example PRIVATE mt5bridge::client)
    set_target_properties(usage_example PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

    add_executable(smoke_no_mt5 examples/smoke_no_mt5.cpp)
    target_link_libraries(smoke_no_mt5 PRIVATE mt5bridge::client)
    set_target_properties(smoke_no_mt5 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
endif()
