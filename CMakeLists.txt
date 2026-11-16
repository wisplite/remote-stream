cmake_minimum_required(VERSION 3.20)
project(remote_stream VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Generates compile_commands.json for clangd/IDEs

include(FetchContent)

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
    "${EMBEDDED_WEB_CPP}"
)

add_executable(${PROJECT_NAME} ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src)

target_link_libraries(${PROJECT_NAME} PRIVATE ixwebsocket)

if(UNIX AND NOT APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_LINUX)
    
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET 
        xcb 
        xcb-damage 
        xcb-shm 
        xcb-xfixes
    )
    
    target_sources(${PROJECT_NAME} PRIVATE 
        src/capture/linux/x11_tile_source.cpp
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::XCB)

elseif(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_WINDOWS)
    
    target_sources(${PROJECT_NAME} PRIVATE 
        src/capture/windows/dxgi_tile_source.cpp
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE d3d11 dxgi)
endif()