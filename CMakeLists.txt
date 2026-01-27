cmake_minimum_required(VERSION 3.10)
project(smartproxy CXX)

# Set C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform detection
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
    set(PLATFORM_SOURCES network.cpp)
else()
    set(PLATFORM_SOURCES network.cpp)
endif()

# Source files
set(SOURCES
    main.cpp
    config.cpp
    dns.cpp
    runway_manager.cpp
    routing.cpp
    tracker.cpp
    validator.cpp
    proxy.cpp
    health.cpp
    utils.cpp
    logger.cpp
    tui.cpp
    tui_input.cpp
    webui.cpp
    ${PLATFORM_SOURCES}
)

# Create executable
add_executable(smartproxy ${SOURCES})

# Platform-specific libraries
if(WIN32)
    target_link_libraries(smartproxy ws2_32 iphlpapi)
else()
    # POSIX: no additional libraries needed (all in standard library)
endif()

# Compiler flags - defensive: enable all warnings
if(MSVC)
    target_compile_options(smartproxy PRIVATE /W4 /WX-)
else()
    target_compile_options(smartproxy PRIVATE -Wall -Wextra -Wpedantic -Wno-pedantic)
    # Allow cross-compilation flags from environment
    if(CMAKE_CROSSCOMPILING)
        if(DEFINED ENV{CFLAGS})
            # Convert space-separated string to CMake list
            string(REPLACE " " ";" CFLAGS_LIST "$ENV{CFLAGS}")
            target_compile_options(smartproxy PRIVATE ${CFLAGS_LIST})
        endif()
        if(DEFINED ENV{CXXFLAGS})
            # Convert space-separated string to CMake list
            string(REPLACE " " ";" CXXFLAGS_LIST "$ENV{CXXFLAGS}")
            target_compile_options(smartproxy PRIVATE ${CXXFLAGS_LIST})
        endif()
        if(DEFINED ENV{LDFLAGS})
            # Convert space-separated string to CMake list
            string(REPLACE " " ";" LDFLAGS_LIST "$ENV{LDFLAGS}")
            target_link_options(smartproxy PRIVATE ${LDFLAGS_LIST})
        endif()
    endif()
endif()

# Debug/Release configurations
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(smartproxy PRIVATE DEBUG)
endif()
