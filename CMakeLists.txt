cmake_minimum_required(VERSION 3.20)
project(hcbyj64 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "hcbyj64 is a Windows-only project.")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(HCBYJ64_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(HCBYJ64_GENERATED_WRAPPERS "${HCBYJ64_GENERATED_DIR}/legacy_dm_generated.inc")
file(MAKE_DIRECTORY "${HCBYJ64_GENERATED_DIR}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_wrappers.py"
            "${HCBYJ64_GENERATED_WRAPPERS}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE HCBYJ64_GENERATOR_RESULT
)
if(NOT HCBYJ64_GENERATOR_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate legacy compatibility wrappers.")
endif()

add_library(hcbyj64 SHARED
    src/legacy_dm_x64.cpp
    src/op_dispatch.cpp
)

target_include_directories(hcbyj64
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${HCBYJ64_GENERATED_DIR}
)

target_compile_features(hcbyj64 PRIVATE cxx_std_17)
target_compile_definitions(hcbyj64 PRIVATE HCBYJ64_BUILD NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(hcbyj64 PRIVATE Ole32 OleAut32 Advapi32 Shell32 Comdlg32 Wininet User32 Winmm Dwmapi)

if(MSVC)
    target_compile_options(hcbyj64 PRIVATE /W4 /permissive- /EHsc)
    set_property(TARGET hcbyj64 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(HCBYJ_RECOVERED_OUTPUT "hcbyj64")
else()
    set(HCBYJ_RECOVERED_OUTPUT "hcbyj32_recovered")
endif()

set_target_properties(hcbyj64 PROPERTIES
    OUTPUT_NAME "${HCBYJ_RECOVERED_OUTPUT}"
)

add_executable(hcbyj64_smoke tests/smoke_test.cpp)
target_include_directories(hcbyj64_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hcbyj64_smoke PRIVATE hcbyj64)
target_compile_features(hcbyj64_smoke PRIVATE cxx_std_17)

# Win32 differential probe: the original hcbyj.dll is supplied only at runtime.
# No proprietary reference binary/source is committed to the repository.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(hcbyj_parity tests/parity_smoke.cpp)
    target_include_directories(hcbyj_parity PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(hcbyj_parity PRIVATE hcbyj64)
    target_compile_features(hcbyj_parity PRIVATE cxx_std_17)
endif()
