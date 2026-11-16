cmake_minimum_required(VERSION 3.20)
project(remote_stream VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Generates compile_commands.json for clangd/IDEs

include(FetchContent)

option(STATIC_BUILD "Statically link OpenSSL, WebP, libjpeg-turbo, and C++ runtime" OFF)

if(STATIC_BUILD)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()

set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v12.1.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ixwebsocket)

# Embed ./web assets into the binary at build time
file(GLOB_RECURSE WEB_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/web/*")
set(EMBEDDED_WEB_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_web.cpp")

add_custom_command(
    OUTPUT "${EMBEDDED_WEB_CPP}"
    COMMAND ${CMAKE_COMMAND}
        -DWEB_DIR=${CMAKE_SOURCE_DIR}/web
        -DOUTPUT_FILE=${EMBEDDED_WEB_CPP}
        -P ${CMAKE_SOURCE_DIR}/cmake/embed_web.cmake
    DEPENDS ${WEB_FILES} "${CMAKE_SOURCE_DIR}/cmake/embed_web.cmake"
    COMMENT "Embedding web assets from ./web"
    VERBATIM
)

# Core cross-platform sources
set(SOURCES
    src/main.cpp
    src/crypto/gen_certs.cpp
    "${EMBEDDED_WEB_CPP}"
)

add_executable(${PROJECT_NAME} ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src)

find_package(OpenSSL REQUIRED)

if(STATIC_BUILD)
    set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        libwebp
        GIT_REPOSITORY https://github.com/webmproject/libwebp.git
        GIT_TAG v1.5.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(libwebp)

    set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
    set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
    set(WITH_TURBOJPEG ON CACHE BOOL "" FORCE)

    include(ExternalProject)
    set(LIBJPEG_TURBO_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/_deps/libjpeg-turbo")
    ExternalProject_Add(
        libjpeg_turbo_ep
        GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git
        GIT_TAG 3.1.0
        GIT_SHALLOW TRUE
        PREFIX "${LIBJPEG_TURBO_PREFIX}"
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${LIBJPEG_TURBO_PREFIX}/install
            -DCMAKE_INSTALL_LIBDIR=lib
            -DENABLE_SHARED=OFF
            -DENABLE_STATIC=ON
            -DWITH_TURBOJPEG=ON
        BUILD_BYPRODUCTS "${LIBJPEG_TURBO_PREFIX}/install/lib/libturbojpeg.a"
    )
    file(MAKE_DIRECTORY "${LIBJPEG_TURBO_PREFIX}/install/include")
    add_library(turbojpeg_static STATIC IMPORTED)
    set_target_properties(turbojpeg_static PROPERTIES
        IMPORTED_LOCATION "${LIBJPEG_TURBO_PREFIX}/install/lib/libturbojpeg.a"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBJPEG_TURBO_PREFIX}/install/include"
    )
    add_dependencies(turbojpeg_static libjpeg_turbo_ep)

    target_include_directories(${PROJECT_NAME} PRIVATE
        "${libwebp_SOURCE_DIR}/src"
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ixwebsocket
        OpenSSL::Crypto
        OpenSSL::SSL
        webp
        turbojpeg_static
    )
else()
    find_package(WebP REQUIRED)
    find_package(libjpeg-turbo REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ixwebsocket
        OpenSSL::Crypto
        WebP::webp
        libjpeg-turbo::turbojpeg
    )
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_LINUX)
    
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET 
        xcb 
        xcb-damage 
        xcb-shm 
        xcb-xfixes
        xcb-xtest
        xcb-keysyms
    )
    
    target_sources(${PROJECT_NAME} PRIVATE 
        src/capture/linux/x11_tile_source.cpp
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::XCB)

    if(STATIC_BUILD)
        target_link_options(${PROJECT_NAME} PRIVATE -static-libstdc++ -static-libgcc)
    endif()

elseif(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_WINDOWS)
    
    target_sources(${PROJECT_NAME} PRIVATE 
        src/capture/windows/dxgi_tile_source.cpp
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE d3d11 dxgi)
endif()