cmake_minimum_required(VERSION 3.24)
project(ThumbGen VERSION 0.1.0 LANGUAGES CXX)

find_package(OpenImageIO 3.2 CONFIG REQUIRED)
find_package(nlohmann_json 3.11 CONFIG REQUIRED)

add_library(thumbgen_core STATIC src/core.cpp)
target_compile_features(thumbgen_core PUBLIC cxx_std_17)
target_include_directories(thumbgen_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(thumbgen_core PUBLIC nlohmann_json::nlohmann_json PRIVATE OpenImageIO::OpenImageIO)
add_executable(thumbgen src/main.cpp)
target_link_libraries(thumbgen PRIVATE thumbgen_core)
if(MSVC)
    target_compile_options(thumbgen PRIVATE /W4 /utf-8 /permissive-)
    target_compile_options(thumbgen_core PRIVATE /W4 /utf-8 /permissive-)
    target_compile_definitions(thumbgen_core PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
else()
    target_compile_options(thumbgen PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(thumbgen_core PRIVATE -Wall -Wextra -Wpedantic)
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
        alpha_exr_associated alpha_png alpha_webp alpha_tiff alpha_edge alpha_zero)
    foreach(image_case IN LISTS THUMBGEN_IMAGE_CASES)
        add_test(NAME image_${image_case} COMMAND thumbgen_image_matrix ${image_case})
        set_tests_properties(image_${image_case} PROPERTIES LABELS "image-matrix" TIMEOUT 30
            ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:OpenImageIO::OpenImageIO>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:OpenImageIO::OpenImageIO>")
    endforeach()
    if(MSVC)
        target_compile_options(thumbgen_image_matrix PRIVATE /W4 /utf-8 /permissive-)
    endif()
    find_package(Python3 3.9 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME public_cli COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py --exe $<TARGET_FILE:thumbgen>
            --runtime-dir $<TARGET_FILE_DIR:OpenImageIO::OpenImageIO>)
    endif()
endif()
