# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(RyGTEnhancementTests LANGUAGES CXX)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(SHADGT_ROOT "../.." ABSOLUTE)
set(SHADGT_FMT_INCLUDE "${SHADGT_ROOT}/externals/fmt/include" CACHE PATH "fmt headers")
set(SHADGT_MAGIC_ENUM_INCLUDE "${SHADGT_ROOT}/externals/magic_enum/include" CACHE PATH "magic_enum headers")

foreach(name IN ITEMS allocation_pressure_retry dlssg_options dlssg_state ghost_smoothing texture_gc video_out_source tiling_specialization)
    add_executable(${name}_test ${name}_test.cpp)
    target_compile_features(${name}_test PRIVATE cxx_std_23)
    target_include_directories(${name}_test PRIVATE
        "${SHADGT_ROOT}" "${SHADGT_ROOT}/src"
        "${SHADGT_FMT_INCLUDE}" "${SHADGT_MAGIC_ENUM_INCLUDE}"
        "${CMAKE_CURRENT_BINARY_DIR}")
    if(name STREQUAL "video_out_source" OR name STREQUAL "tiling_specialization")
        target_include_directories(${name}_test PRIVATE
            "${SHADGT_ROOT}/externals/vulkan-headers/include")
    endif()
    if(MSVC)
        target_compile_options(${name}_test PRIVATE /EHsc)
        target_compile_definitions(${name}_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()

target_sources(tiling_specialization_test PRIVATE "${SHADGT_ROOT}/src/video_core/amdgpu/tiling.cpp")
target_compile_definitions(tiling_specialization_test PRIVATE FMT_HEADER_ONLY)

add_test(NAME ghost_smoothing_disabled COMMAND ghost_smoothing_test disabled)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gc-function-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_gc.py"
        "${SHADGT_ROOT}/src/video_core/texture_cache/texture_cache.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/gc-function-under-test.inc"
    DEPENDS extract_gc.py "${SHADGT_ROOT}/src/video_core/texture_cache/texture_cache.cpp"
    VERBATIM)
target_sources(texture_gc_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gc-function-under-test.inc")

# Recorder/compiler resource lifetime and reuse regression coverage.
foreach(name IN ITEMS cleanup_worker upload_chunk_cache upload_tail pipeline_binding_state
        pipeline_retention draw_prep compiler_library_budget fragment_descriptor_policy worker_keys)
    add_executable(${name}_test ${name}_test.cpp)
    target_compile_features(${name}_test PRIVATE cxx_std_23)
    target_include_directories(${name}_test PRIVATE "${SHADGT_ROOT}" "${SHADGT_ROOT}/src"
        "${SHADGT_FMT_INCLUDE}" "${SHADGT_MAGIC_ENUM_INCLUDE}"
        "${SHADGT_ROOT}/externals/xxhash/include"
        "${SHADGT_ROOT}/externals/vulkan-headers/include")
    if(MSVC)
        target_compile_options(${name}_test PRIVATE /EHsc /UNDEBUG)
    endif()
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()
if(WIN32)
    add_executable(immutable_upload_test immutable_upload_test.cpp)
    target_compile_features(immutable_upload_test PRIVATE cxx_std_23)
    target_include_directories(immutable_upload_test PRIVATE "${SHADGT_ROOT}" "${SHADGT_ROOT}/src")
    if(MSVC)
        target_compile_options(immutable_upload_test PRIVATE /EHsc /UNDEBUG)
    endif()
    add_test(NAME immutable_upload COMMAND immutable_upload_test)
endif()

find_package(Threads REQUIRED)
foreach(name IN ITEMS cleanup_worker draw_prep compiler_library_budget)
    target_link_libraries(${name}_test PRIVATE Threads::Threads)
endforeach()
