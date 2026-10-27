# ew2host.exe - the native Windows host: runs the ARM32 Linux guest (port/build/ew2) on Windows x64 without WSL.
# A Linux user-mode emulator on dynarmic (JIT) plus the host side of the GL/audio/input bridge.
# STATUS: work in progress. What builds today is the dynarmic binding + guest memory + a self-test
# (ew2host.exe --selftest). See docs/NATIVE_WINDOWS.md for what is missing and the plan.
#
# Build: host\build.cmd on Windows, or host/build.sh from WSL (both use Visual Studio's MSVC + CMake + Ninja).
# Dependencies (sources, not installed anywhere):
#   -DDYNARMIC_DIR=<dynarmic checkout>   default third_party/dynarmic, then ../eternity-warriors-3-probe/third_party/dynarmic
#   -DBOOST_DIR=<folder that contains boost/>  default third_party/boost, then the same folder of the EW3 project
cmake_minimum_required(VERSION 3.20)
project(ew2host CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(EW2_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(EW3_SIBLING ${EW2_ROOT}/../eternity-warriors-3-probe/third_party)

set(DYNARMIC_DIR "" CACHE PATH "dynarmic source tree")
set(BOOST_DIR "" CACHE PATH "folder that contains boost/ (headers only)")
foreach(dep DYNARMIC BOOST)
    string(TOLOWER ${dep} sub)
    if (NOT ${dep}_DIR)
        if (EXISTS ${EW2_ROOT}/third_party/${sub})
            set(${dep}_DIR ${EW2_ROOT}/third_party/${sub})
        elseif (EXISTS ${EW3_SIBLING}/${sub})
            set(${dep}_DIR ${EW3_SIBLING}/${sub})
        endif()
    endif()
endforeach()
if (NOT DYNARMIC_DIR OR NOT EXISTS ${DYNARMIC_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "dynarmic sources not found: pass -DDYNARMIC_DIR=... (https://github.com/merryhime/dynarmic, "
                        "built with commit a46601580d5512d324104f985b5f0209dc980ddc)")
endif()
if (NOT BOOST_DIR OR NOT EXISTS ${BOOST_DIR}/boost)
    message(FATAL_ERROR "Boost headers not found: pass -DBOOST_DIR=<folder with boost/> (https://www.boost.org/, headers only)")
endif()
message(STATUS "dynarmic: ${DYNARMIC_DIR}")
message(STATUS "boost:    ${BOOST_DIR}")

# dynarmic: A32 frontend, x64 backend, bundled externals; Boost headers only.
set(Boost_INCLUDE_DIR ${BOOST_DIR} CACHE PATH "" FORCE)
set(BOOST_ROOT ${BOOST_DIR} CACHE PATH "" FORCE)
set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(${DYNARMIC_DIR} ${CMAKE_BINARY_DIR}/dynarmic EXCLUDE_FROM_ALL)

# C17 everywhere; C++ only for the dynarmic binding (src/cpu.cpp), whose API is C++.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
file(GLOB EW2HOST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_executable(ew2host ${EW2HOST_SOURCES})
target_link_libraries(ew2host PRIVATE dynarmic)
target_compile_definitions(ew2host PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if (MSVC)
    target_compile_options(ew2host PRIVATE /W3 /utf-8 $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
    # C11 atomics and threads in MSVC need the experimental switch.
    target_compile_options(ew2host PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
endif()
