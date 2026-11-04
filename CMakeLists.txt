cmake_minimum_required(VERSION 3.24)
project(ThumbGen VERSION 0.1.0 LANGUAGES CXX)

find_package(OpenImageIO 3.2 CONFIG REQUIRED)
find_package(nlohmann_json 3.11 CONFIG REQUIRED)
set(THUMBGEN_SOURCE_REVISION "unknown")
set(THUMBGEN_SOURCE_DIRTY false)
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE THUMBGEN_SOURCE_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=normal
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE THUMBGEN_GIT_CHANGES)
    if(THUMBGEN_GIT_CHANGES)
        set(THUMBGEN_SOURCE_DIRTY true)
    endif()
endif()
configure_file(src/build_info.h.in generated/build_info.h @ONLY)

add_library(thumbgen_core STATIC src/core.cpp src/selection.cpp src/files.cpp)
target_compile_features(thumbgen_core PUBLIC cxx_std_17)
target_include_directories(thumbgen_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(thumbgen_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(thumbgen_core PUBLIC nlohmann_json::nlohmann_json PRIVATE OpenImageIO::OpenImageIO)
add_executable(thumbgen src/main.cpp)
target_link_libraries(thumbgen PRIVATE thumbgen_core)
find_package(Threads REQUIRED)
add_library(thumbgen_schedule STATIC src/budget.cpp src/child_process.cpp)
target_compile_features(thumbgen_schedule PUBLIC cxx_std_17)
target_include_directories(thumbgen_schedule PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(thumbgen_schedule PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(thumbgen PRIVATE /W4 /utf-8 /permissive-)
    target_compile_options(thumbgen_core PRIVATE /W4 /utf-8 /permissive-)
    target_compile_options(thumbgen_schedule PRIVATE /W4 /utf-8 /permissive-)
    target_compile_definitions(thumbgen_core PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_compile_definitions(thumbgen_schedule PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
else()
    target_compile_options(thumbgen PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(thumbgen_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(thumbgen_schedule PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(THUMBGEN_BUILD_PROBES "Build archived selection probes" OFF)
if(THUMBGEN_BUILD_PROBES)
    add_executable(thumbgen_probe benchmarks/native_probe.cpp)
    target_compile_features(thumbgen_probe PRIVATE cxx_std_17)
    target_link_libraries(thumbgen_probe PRIVATE OpenImageIO::OpenImageIO nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(thumbgen_probe PRIVATE /W4 /utf-8 /permissive-)
    endif()
endif()

option(THUMBGEN_BUILD_VIPS "Build the separate libvips selection probe" OFF)
set(VIPS_ROOT "" CACHE PATH "libvips SDK prefix (Library directory on Windows conda)")
if(THUMBGEN_BUILD_VIPS)
    find_path(VIPS_INCLUDE_DIR vips/vips.h HINTS "${VIPS_ROOT}/include" REQUIRED)
    find_path(GLIB_INCLUDE_DIR glib.h HINTS "${VIPS_ROOT}/include/glib-2.0" REQUIRED)
    find_path(GLIB_CONFIG_DIR glibconfig.h HINTS "${VIPS_ROOT}/lib/glib-2.0/include" REQUIRED)
    find_library(VIPS_LIBRARY NAMES vips libvips HINTS "${VIPS_ROOT}/lib" REQUIRED)
    find_library(GOBJECT_LIBRARY NAMES gobject-2.0 HINTS "${VIPS_ROOT}/lib" REQUIRED)
    find_library(GLIB_LIBRARY NAMES glib-2.0 HINTS "${VIPS_ROOT}/lib" REQUIRED)
    add_executable(thumbgen_vips benchmarks/vips_probe.cpp)
    target_compile_features(thumbgen_vips PRIVATE cxx_std_17)
    target_include_directories(thumbgen_vips PRIVATE ${VIPS_INCLUDE_DIR} ${GLIB_INCLUDE_DIR} ${GLIB_CONFIG_DIR})
    target_link_libraries(thumbgen_vips PRIVATE ${VIPS_LIBRARY} ${GOBJECT_LIBRARY} ${GLIB_LIBRARY} nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(thumbgen_vips PRIVATE /W4 /utf-8 /permissive-)
    endif()
endif()

install(TARGETS thumbgen RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(thumbgen_process_tests tests/test_process.cpp)
    target_link_libraries(thumbgen_process_tests PRIVATE thumbgen_schedule)
    add_test(NAME scheduler_process COMMAND thumbgen_process_tests)
    set_tests_properties(scheduler_process PROPERTIES TIMEOUT 30 LABELS "scheduler")
    add_executable(thumbgen_budget_tests tests/test_budget.cpp)
    target_link_libraries(thumbgen_budget_tests PRIVATE thumbgen_schedule)
    add_test(NAME scheduler_budget COMMAND thumbgen_budget_tests)
    set_tests_properties(scheduler_budget PROPERTIES TIMEOUT 10 LABELS "scheduler")
    if(MSVC)
        target_compile_options(thumbgen_budget_tests PRIVATE /W4 /utf-8 /permissive-)
        target_compile_options(thumbgen_process_tests PRIVATE /W4 /utf-8 /permissive-)
    endif()
    add_executable(thumbgen_core_tests tests/test_core.cpp)
    target_link_libraries(thumbgen_core_tests PRIVATE thumbgen_core OpenImageIO::OpenImageIO)
    add_test(NAME core_images COMMAND thumbgen_core_tests)
    set_tests_properties(core_images PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:OpenImageIO::OpenImageIO>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:OpenImageIO::OpenImageIO>")
    add_executable(thumbgen_image_matrix tests/test_image_matrix.cpp)
    target_link_libraries(thumbgen_image_matrix PRIVATE thumbgen_core OpenImageIO::OpenImageIO)
    set(THUMBGEN_IMAGE_CASES
        jpeg_rgb8 jpeg_gray8 png_rgb8 png_rgb16 png_rgba16 png_gray8 png_grayalpha16
        tiff_rgb8 tiff_rgb16 tiff_rgbfloat tiff_rgba16 tga_rgb8 tga_rgba8 bmp_rgb8
        hdr_rgbfloat dpx_rgb10 exr_float exr_mixed
        exr_half_none exr_half_rle exr_half_zip exr_half_zips exr_half_piz
        exr_half_pxr24 exr_half_b44 exr_half_b44a exr_half_dwaa exr_half_dwab
        orientation_1 orientation_2 orientation_3 orientation_4
        orientation_5 orientation_6 orientation_7 orientation_8
        output_jpeg output_png output_webp
        window_negative window_outside window_negative_rotated geometry_portrait geometry_ultrawide
        colour_builtin colour_custom colour_environment colour_explicit_priority colour_metadata colour_override
        colour_display colour_missing_role colour_missing_space colour_missing_display colour_missing_view colour_invalid_repeat
        colour_nonlinear_role colour_invalid_environment
        alpha_exr_associated alpha_png alpha_webp alpha_tiff alpha_edge alpha_zero
        data_z data_normal data_normal_part data_id data_crypto data_explicit_data data_unknown data_grey data_lowercase
        data_beauty_z data_beauty_normal data_beauty_crypto data_root_priority data_ambiguous data_explicit
        data_missing_part data_missing_group data_black data_visible
        primary_black_beauty primary_black_root primary_root_strategy primary_combined primary_tie primary_denoised
        sequence_missing sequence_no_upgrade sequence_conflict sequence_alpha_changed sequence_independent
        io_error io_skip io_replace io_partial io_hardlink io_race_error io_race_skip io_race_replace
        io_source_decode io_source_identity io_source_commit io_source_partial io_cleanup_foreign)
    if(UNIX)
        list(APPEND THUMBGEN_IMAGE_CASES io_symlink io_write_limit io_permission)
    endif()
    foreach(image_case IN LISTS THUMBGEN_IMAGE_CASES)
        add_test(NAME image_${image_case} COMMAND thumbgen_image_matrix ${image_case})
        set_tests_properties(image_${image_case} PROPERTIES LABELS "image-matrix" TIMEOUT 30
            ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:OpenImageIO::OpenImageIO>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:OpenImageIO::OpenImageIO>")
    endforeach()
    if(UNIX)
        set_tests_properties(image_io_permission PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    if(MSVC)
        target_compile_options(thumbgen_image_matrix PRIVATE /W4 /utf-8 /permissive-)
    endif()
    find_package(Python3 3.9 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME public_cli COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py --exe $<TARGET_FILE:thumbgen>
            --runtime-dir $<TARGET_FILE_DIR:OpenImageIO::OpenImageIO>)
        option(THUMBGEN_TEST_PROTOCOL "Validate public records using jsonschema (test dependency)" ON)
        if(THUMBGEN_TEST_PROTOCOL)
            execute_process(COMMAND ${Python3_EXECUTABLE} -c "import jsonschema" RESULT_VARIABLE THUMBGEN_SCHEMA_IMPORT)
            if(NOT THUMBGEN_SCHEMA_IMPORT EQUAL 0)
                message(FATAL_ERROR "Protocol tests require tests/requirements.txt in Python3_EXECUTABLE; or explicitly set THUMBGEN_TEST_PROTOCOL=OFF")
            endif()
            add_test(NAME public_protocol COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_protocol.py --exe $<TARGET_FILE:thumbgen>
                --matrix-exe $<TARGET_FILE:thumbgen_image_matrix> --schema-dir ${CMAKE_CURRENT_SOURCE_DIR}/schemas
                --runtime-dir $<TARGET_FILE_DIR:OpenImageIO::OpenImageIO>)
            set_tests_properties(public_protocol PROPERTIES TIMEOUT 120 LABELS "protocol")
        endif()
    endif()
endif()
