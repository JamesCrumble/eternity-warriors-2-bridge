# ew3host.exe - runs the ARM32 Linux guest (port/build/ew3) on Windows x64:
# a Linux user-mode emulator on dynarmic plus the host side of the GL/audio/input
# bridge (ANGLE + SDL2). Build: host/build.cmd from Windows, or host/build.sh from WSL.
cmake_minimum_required(VERSION 3.20)
project(ew3host CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(EW3_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# dynarmic: A32 frontend, x64 backend, bundled externals; Boost headers from third_party/boost.
set(Boost_INCLUDE_DIR ${EW3_ROOT}/third_party/boost CACHE PATH "" FORCE)
set(BOOST_ROOT ${EW3_ROOT}/third_party/boost CACHE PATH "" FORCE)
set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(${EW3_ROOT}/third_party/dynarmic ${CMAKE_BINARY_DIR}/dynarmic EXCLUDE_FROM_ALL)

# C17 everywhere; C++ only for the dynarmic binding (src/cpu.cpp), whose API is C++.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
file(GLOB EW3HOST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_executable(ew3host ${EW3HOST_SOURCES})
target_link_libraries(ew3host PRIVATE dynarmic)
target_compile_definitions(ew3host PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if (MSVC)
    target_compile_options(ew3host PRIVATE /W3 /utf-8 $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
    # C11 atomics and threads in MSVC need the experimental switch.
    target_compile_options(ew3host PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
endif()

# ew3gl.exe - the GL bridge host (glbridge/ew3gl.c, C): replays the guest's GLES stream on ANGLE
# (Direct3D 11) in an SDL2 window. Needs third_party/sdl2-win (SDL2 VC package) and third_party/angle
# (tools/angle_setup.sh: the DLLs plus libGLESv2.def, made into an import library here).
set(ANGLE_DIR ${EW3_ROOT}/third_party/angle)
set(SDL2_WIN_DIR ${EW3_ROOT}/third_party/sdl2-win)
set(ANGLE_IMPLIB ${CMAKE_BINARY_DIR}/libGLESv2.lib)
add_custom_command(
    OUTPUT ${ANGLE_IMPLIB}
    COMMAND ${CMAKE_AR} /nologo /def:${ANGLE_DIR}/libGLESv2.def /out:${ANGLE_IMPLIB} /machine:x64
    DEPENDS ${ANGLE_DIR}/libGLESv2.def
    COMMENT "ANGLE import library")
add_custom_target(angle_implib DEPENDS ${ANGLE_IMPLIB})

add_executable(ew3gl ${CMAKE_CURRENT_SOURCE_DIR}/glbridge/ew3gl.c)
add_dependencies(ew3gl angle_implib)
target_include_directories(ew3gl PRIVATE ${SDL2_WIN_DIR}/include ${EW3_ROOT}/third_party/khronos/include)
target_compile_definitions(ew3gl PRIVATE SDL_MAIN_HANDLED NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
                                         _WINSOCK_DEPRECATED_NO_WARNINGS)
target_link_libraries(ew3gl PRIVATE ${SDL2_WIN_DIR}/lib/SDL2.lib ${ANGLE_IMPLIB} ws2_32)
if (MSVC)
    target_compile_options(ew3gl PRIVATE /W3 /utf-8)
endif()
# The DLLs go next to the exe: SDL loads libEGL.dll itself, libGLESv2.dll is linked.
add_custom_command(TARGET ew3gl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDL2_WIN_DIR}/lib/SDL2.dll ${ANGLE_DIR}/bin/libEGL.dll
            ${ANGLE_DIR}/bin/libGLESv2.dll $<TARGET_FILE_DIR:ew3gl>)
