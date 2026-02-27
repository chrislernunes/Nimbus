cmake_minimum_required(VERSION 3.16)
project(Nimbus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-O2 -Wall -Wextra)

add_executable(nimbus
    main.cpp
    black_scholes.cpp
    sabr.cpp
    svi_surface.cpp
    market_maker.cpp
    binance_options.cpp
    live_surface.cpp
)

if(WIN32)
    target_compile_definitions(nimbus PRIVATE _WIN32_WINNT=0x0601)
endif()

# libcurl + nlohmann/json (required for live modes)
find_package(CURL REQUIRED)
target_link_libraries(nimbus PRIVATE CURL::libcurl)

# nlohmann/json — header only
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(nimbus PRIVATE nlohmann_json::nlohmann_json)
else()
    # fallback: assume installed in system include path
    message(STATUS "nlohmann/json not found via CMake — assuming system include path")
endif()

if(WIN32)
    target_link_libraries(nimbus PRIVATE ws2_32)
endif()

target_include_directories(nimbus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
