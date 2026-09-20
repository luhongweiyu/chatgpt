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
target_link_libraries(hcbyj64 PRIVATE Ole32 OleAut32)

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


# Optional private x86 reference build. The original DLL/source stay in
# reference/private and are ignored by git.
set(HCBYJ_REFERENCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/reference/private")
if(CMAKE_SIZEOF_VOID_P EQUAL 4
   AND EXISTS "${HCBYJ_REFERENCE_DIR}/legacy_dm.h"
   AND EXISTS "${HCBYJ_REFERENCE_DIR}/legacy_dm.cpp"
   AND EXISTS "${HCBYJ_REFERENCE_DIR}/hcbyj.dll")
    add_library(hcbyj_legacy_reference STATIC
        "${HCBYJ_REFERENCE_DIR}/legacy_dm.cpp"
    )
    target_include_directories(hcbyj_legacy_reference PRIVATE "${HCBYJ_REFERENCE_DIR}")
    target_compile_definitions(hcbyj_legacy_reference PRIVATE
        dmsoft=legacy_dmsoft
        LoadDm=LegacyLoadDm
        LoadDmW=LegacyLoadDmW
        FreeDm=LegacyFreeDm
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_compile_features(hcbyj_legacy_reference PRIVATE cxx_std_17)

    add_executable(hcbyj_parity tests/parity_smoke.cpp)
    target_include_directories(hcbyj_parity PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/reference"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(hcbyj_parity PRIVATE hcbyj64 hcbyj_legacy_reference)
    target_compile_features(hcbyj_parity PRIVATE cxx_std_17)

    add_custom_command(TARGET hcbyj_parity POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HCBYJ_REFERENCE_DIR}/hcbyj.dll"
            "$<TARGET_FILE_DIR:hcbyj_parity>/hcbyj.dll"
    )
endif()
