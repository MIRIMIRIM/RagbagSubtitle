cmake_minimum_required(VERSION 3.25)

project(RagbagSubtitle
    VERSION 0.2.0
    DESCRIPTION "Packet-backed bitmap subtitle decoder for Aegisub"
    LANGUAGES C CXX)

option(RAGBAG_BUILD_FFMPEG_DECODER "Build the FFmpeg/libavcodec subtitle decoder plugin" ON)
option(RAGBAG_BUILD_PROBE "Build a tiny plugin loader probe executable" ON)
set(RAGBAG_FFMPEG_ROOT "" CACHE PATH "Optional preinstalled FFmpeg package root")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(GNUInstallDirs)
include(CTest)

add_library(ragbag_subtitle_plugin_api INTERFACE)
target_include_directories(ragbag_subtitle_plugin_api INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

if(RAGBAG_BUILD_FFMPEG_DECODER)
    set(RAGBAG_FFMPEG_LINK_TARGET "")

    if(RAGBAG_FFMPEG_ROOT)
        set(RAGBAG_VCPKG_TARGET_DIR "${RAGBAG_FFMPEG_ROOT}")
    elseif(WIN32 AND VCPKG_TARGET_TRIPLET MATCHES "static" AND DEFINED VCPKG_INSTALLED_DIR)
        set(RAGBAG_VCPKG_TARGET_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    endif()
    if(RAGBAG_VCPKG_TARGET_DIR)
        if(NOT EXISTS "${RAGBAG_VCPKG_TARGET_DIR}/include/libavcodec/avcodec.h")
            message(FATAL_ERROR "FFmpeg headers were not found below ${RAGBAG_VCPKG_TARGET_DIR}.")
        endif()

        add_library(ragbag_ffmpeg_explicit INTERFACE)
        target_include_directories(ragbag_ffmpeg_explicit INTERFACE
            "${RAGBAG_VCPKG_TARGET_DIR}/include")
        foreach(RAGBAG_FFMPEG_LIB avcodec avutil swresample)
            string(TOUPPER "${RAGBAG_FFMPEG_LIB}" RAGBAG_FFMPEG_LIB_UPPER)
            unset(RAGBAG_${RAGBAG_FFMPEG_LIB_UPPER}_LIBRARY_RELEASE CACHE)
            unset(RAGBAG_${RAGBAG_FFMPEG_LIB_UPPER}_LIBRARY_DEBUG CACHE)
            find_library(RAGBAG_${RAGBAG_FFMPEG_LIB_UPPER}_LIBRARY_RELEASE
                NAMES "${RAGBAG_FFMPEG_LIB}"
                PATHS
                    "${RAGBAG_VCPKG_TARGET_DIR}/lib"
                    "${RAGBAG_VCPKG_TARGET_DIR}/lib64"
                NO_DEFAULT_PATH)
            find_library(RAGBAG_${RAGBAG_FFMPEG_LIB_UPPER}_LIBRARY_DEBUG
                NAMES "${RAGBAG_FFMPEG_LIB}"
                PATHS
                    "${RAGBAG_VCPKG_TARGET_DIR}/debug/lib"
                    "${RAGBAG_VCPKG_TARGET_DIR}/debug/lib64"
                NO_DEFAULT_PATH)
            if(NOT RAGBAG_${RAGBAG_FFMPEG_LIB_UPPER}_LIBRARY_RELEASE)
                message(FATAL_ERROR "FFmpeg library ${RAGBAG_FFMPEG_LIB} was not found below ${RAGBAG_VCPKG_TARGET_DIR}.")
            endif()
            target_link_libraries(ragbag_ffmpeg_explicit INTERFACE
                optimized "${RAGBAG_${RAGBAG_FFMPEG_LIB_UPPER}_LIBRARY_RELEASE}")
            if(RAGBAG_${RAGBAG_FFMPEG_LIB_UPPER}_LIBRARY_DEBUG)
                target_link_libraries(ragbag_ffmpeg_explicit INTERFACE
                    debug "${RAGBAG_${RAGBAG_FFMPEG_LIB_UPPER}_LIBRARY_DEBUG}")
            else()
                target_link_libraries(ragbag_ffmpeg_explicit INTERFACE
                    debug "${RAGBAG_${RAGBAG_FFMPEG_LIB_UPPER}_LIBRARY_RELEASE}")
            endif()
        endforeach()
        if(WIN32)
            target_link_libraries(ragbag_ffmpeg_explicit INTERFACE
                secur32
                ncrypt
                crypt32
                ws2_32
                mfuuid
                strmiids
                ole32
                user32
                bcrypt)
        endif()
        set(RAGBAG_FFMPEG_LINK_TARGET ragbag_ffmpeg_explicit)
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
            libavcodec
            libavutil)
        set(PKG_CONFIG_ARGN "${RAGBAG_OLD_PKG_CONFIG_ARGN}")
        if(RAGBAG_FFMPEG_FOUND)
            set(RAGBAG_FFMPEG_LINK_TARGET PkgConfig::RAGBAG_FFMPEG)
        endif()
    endif()

    if(NOT RAGBAG_FFMPEG_LINK_TARGET)
        message(FATAL_ERROR "FFmpeg libavcodec/libavutil development libraries were not found. Use pkg-config, set RAGBAG_FFMPEG_ROOT to a preinstalled FFmpeg package root, or configure with the vcpkg toolchain so the ffmpeg[avcodec] manifest dependency is installed. Broad FindFFMPEG fallbacks are intentionally unsupported because they may add libavformat.")
    endif()

    add_library(ragbag_subtitle_ffmpeg SHARED
        src/ffmpeg_subtitle_provider.cpp
        src/plugin_entry.cpp)
    add_library(RagbagSubtitle::ffmpeg ALIAS ragbag_subtitle_ffmpeg)

    target_link_libraries(ragbag_subtitle_ffmpeg PRIVATE ragbag_subtitle_plugin_api)
    target_compile_definitions(ragbag_subtitle_ffmpeg PRIVATE RAGBAG_SUBTITLE_PLUGIN_BUILD)

    target_link_libraries(ragbag_subtitle_ffmpeg PRIVATE ${RAGBAG_FFMPEG_LINK_TARGET})

    if(MSVC)
        target_compile_options(ragbag_subtitle_ffmpeg PRIVATE
            /W4
            /permissive-
            /utf-8
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/Gy>
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/Gw>
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/Zc:inline>)
        target_link_options(ragbag_subtitle_ffmpeg PRIVATE
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/INCREMENTAL:NO>
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/OPT:REF>
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/OPT:ICF>)
    else()
        target_compile_options(ragbag_subtitle_ffmpeg PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-ffunction-sections>
            $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-fdata-sections>)
        if(APPLE)
            target_link_options(ragbag_subtitle_ffmpeg PRIVATE
                $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-Wl,-dead_strip>)
        else()
            target_link_options(ragbag_subtitle_ffmpeg PRIVATE
                $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-Wl,--gc-sections>)
        endif()
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
    if(BUILD_TESTING AND RAGBAG_BUILD_FFMPEG_DECODER)
        add_test(
            NAME ragbag_subtitle_decoder_smoke
            COMMAND ragbag_subtitle_probe $<TARGET_FILE:ragbag_subtitle_ffmpeg>)
    endif()
endif()

install(FILES include/ragbag/subtitle_plugin_api.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ragbag)
