cmake_minimum_required(VERSION 3.25)

project(RagbagSubtitle
    VERSION 0.1.0
    DESCRIPTION "Dynamic subtitle plugin experiments for Aegisub"
    LANGUAGES C CXX)

option(RAGBAG_BUILD_FFMPEG_PROVIDER "Build the FFmpeg/libavformat subtitle provider plugin" ON)
option(RAGBAG_BUILD_PROBE "Build a tiny plugin loader probe executable" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "static")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(GNUInstallDirs)

add_library(ragbag_subtitle_plugin_api INTERFACE)
target_include_directories(ragbag_subtitle_plugin_api INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

if(RAGBAG_BUILD_FFMPEG_PROVIDER)
    set(RAGBAG_FFMPEG_LINK_TARGET "")

    if(WIN32 AND VCPKG_TARGET_TRIPLET MATCHES "static" AND DEFINED VCPKG_INSTALLED_DIR)
        set(RAGBAG_VCPKG_TARGET_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
        if(EXISTS "${RAGBAG_VCPKG_TARGET_DIR}/include/libavcodec/avcodec.h")
            add_library(ragbag_ffmpeg_static INTERFACE)
            target_include_directories(ragbag_ffmpeg_static INTERFACE
                "${RAGBAG_VCPKG_TARGET_DIR}/include")
            target_link_directories(ragbag_ffmpeg_static INTERFACE
                "${RAGBAG_VCPKG_TARGET_DIR}/lib")
            foreach(RAGBAG_FFMPEG_LIB avformat avcodec avutil)
                target_link_libraries(ragbag_ffmpeg_static INTERFACE
                    optimized "${RAGBAG_VCPKG_TARGET_DIR}/lib/${RAGBAG_FFMPEG_LIB}.lib")
                if(EXISTS "${RAGBAG_VCPKG_TARGET_DIR}/debug/lib/${RAGBAG_FFMPEG_LIB}.lib")
                    target_link_libraries(ragbag_ffmpeg_static INTERFACE
                        debug "${RAGBAG_VCPKG_TARGET_DIR}/debug/lib/${RAGBAG_FFMPEG_LIB}.lib")
                endif()
            endforeach()
            target_link_libraries(ragbag_ffmpeg_static INTERFACE
                secur32
                ncrypt
                crypt32
                ws2_32
                mfuuid
                strmiids
                ole32
                user32
                bcrypt)
            set(RAGBAG_FFMPEG_LINK_TARGET ragbag_ffmpeg_static)
        endif()
    endif()

    if(NOT RAGBAG_FFMPEG_LINK_TARGET)
        find_package(PkgConfig QUIET)
    endif()
    if(NOT RAGBAG_FFMPEG_LINK_TARGET AND PkgConfig_FOUND)
        set(RAGBAG_OLD_PKG_CONFIG_ARGN "${PKG_CONFIG_ARGN}")
        if(WIN32 AND VCPKG_TARGET_TRIPLET MATCHES "static")
            list(APPEND PKG_CONFIG_ARGN --static)
        endif()
        pkg_check_modules(RAGBAG_FFMPEG QUIET IMPORTED_TARGET
            libavformat
            libavcodec
            libavutil)
        set(PKG_CONFIG_ARGN "${RAGBAG_OLD_PKG_CONFIG_ARGN}")
        if(RAGBAG_FFMPEG_FOUND)
            set(RAGBAG_FFMPEG_LINK_TARGET PkgConfig::RAGBAG_FFMPEG)
        endif()
    endif()

    if(NOT RAGBAG_FFMPEG_LINK_TARGET)
        find_package(FFMPEG QUIET)
    endif()

    if(NOT RAGBAG_FFMPEG_LINK_TARGET AND NOT FFMPEG_FOUND)
        message(FATAL_ERROR "FFmpeg development libraries were not found. With vcpkg, configure using -DCMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake so the ffmpeg[avcodec,avformat] manifest dependency is installed.")
    endif()

    add_library(ragbag_subtitle_ffmpeg SHARED
        src/ffmpeg_subtitle_provider.cpp
        src/plugin_entry.cpp)
    add_library(RagbagSubtitle::ffmpeg ALIAS ragbag_subtitle_ffmpeg)

    target_link_libraries(ragbag_subtitle_ffmpeg PRIVATE ragbag_subtitle_plugin_api)
    target_compile_definitions(ragbag_subtitle_ffmpeg PRIVATE RAGBAG_SUBTITLE_PLUGIN_BUILD)

    if(RAGBAG_FFMPEG_LINK_TARGET)
        target_link_libraries(ragbag_subtitle_ffmpeg PRIVATE ${RAGBAG_FFMPEG_LINK_TARGET})
    else()
        target_include_directories(ragbag_subtitle_ffmpeg PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_link_directories(ragbag_subtitle_ffmpeg PRIVATE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(ragbag_subtitle_ffmpeg PRIVATE ${FFMPEG_LIBRARIES})
    endif()

    if(MSVC)
        target_compile_options(ragbag_subtitle_ffmpeg PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(ragbag_subtitle_ffmpeg PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    set_target_properties(ragbag_subtitle_ffmpeg PROPERTIES
        OUTPUT_NAME "ragbag-subtitle-ffmpeg"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)

    install(TARGETS ragbag_subtitle_ffmpeg
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/runtimes
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/runtimes
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(RAGBAG_BUILD_PROBE)
    add_executable(ragbag_subtitle_probe tools/probe_plugin.cpp)
    target_link_libraries(ragbag_subtitle_probe PRIVATE ragbag_subtitle_plugin_api)
    if(WIN32)
        target_compile_definitions(ragbag_subtitle_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_link_libraries(ragbag_subtitle_probe PRIVATE ${CMAKE_DL_LIBS})
    endif()
    if(MSVC)
        target_compile_options(ragbag_subtitle_probe PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(ragbag_subtitle_probe PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

install(FILES include/ragbag/subtitle_plugin_api.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ragbag)
