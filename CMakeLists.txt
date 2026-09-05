cmake_minimum_required(VERSION 3.20)
project(Luma C CXX)

if(NOT DEFINED ENV{OO_PS4_TOOLCHAIN})
    message(FATAL_ERROR "OO_PS4_TOOLCHAIN environment variable is not set")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/OrbisTargets.cmake)

# Vendored Lua 5.4 (C) and the scripting layer (C++). CONFIGURE_DEPENDS so a new
# source file is picked up without re-running cmake by hand.
file(GLOB LUA_SOURCES      CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/lua/*.c)
file(GLOB SCRIPT_SOURCES   CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/script/*.cpp)
file(GLOB INVOKER_SOURCES  CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/rage/invoker/*.cpp)
file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.cpp)
file(GLOB NET_SOURCES      CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/net/*.cpp)

add_orbis_target(Luma
    TYPE GHPLUGIN
    LIBS SceLibcInternal kernel SceSysmodule SceNet GoldHEN_Hook
    SOURCES
        ${LUA_SOURCES}
        ${SCRIPT_SOURCES}
        ${INVOKER_SOURCES}
        ${PLATFORM_SOURCES}
        ${NET_SOURCES}
        src/main.cpp
        src/game/game_thread.cpp
        src/game/profile.cpp
        src/game/gta5/natives_gta5.cpp
        src/game/rdr2/natives_rdr2.cpp
        src/stl/cxxrt.cpp
)

# natives.h resolves "invoker.h" relative to itself; "rage/types/base_types.h"
# and "platform/log.h" need src/ on the search path.
target_include_directories(Luma PRIVATE src src/lua)

target_compile_definitions(Luma PRIVATE STL_FREESTANDING)

set_target_properties(Luma PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
