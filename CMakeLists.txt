cmake_minimum_required(VERSION 3.20)
project(remote_stream VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Generates compile_commands.json for clangd/IDEs

# Core cross-platform sources
set(SOURCES
    src/main.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src)

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