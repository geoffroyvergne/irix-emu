cmake_minimum_required(VERSION 3.20)

project(IndyEmu
    VERSION 0.1.0
    DESCRIPTION "Clean-room SGI Indy (IP22/Guinness) emulator"
    LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use: cmake -B build")
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(INDY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"              OFF)
option(INDY_ENABLE_LTO         "Enable link-time optimisation for optimised builds" ON)
option(INDY_NATIVE_ARCH        "Tune for the build machine (-march=native)"     OFF)
option(INDY_SANITIZE           "Enable ASan + UBSan (Debug builds)"             OFF)
option(INDY_FETCH_SDL          "Download and build SDL3 if no system copy is found" ON)
option(INDY_BUILD_TESTS        "Build the unit tests (run with ctest)"           ON)
# Profile-guided optimisation (see tools/build-pgo.sh): an instrumented build
# records a profile while IRIX runs; a second build uses it (~13% faster).
option(INDY_PGO_GENERATE       "Instrument for profile-guided optimisation"      OFF)
set(INDY_PGO_PROFILE "" CACHE FILEPATH "Merged PGO profile (.profdata) to optimise with")

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_property(INDY_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT INDY_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(CompilerOptions)   # defines indy::options (warnings, optimisation, sanitizers)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

find_package(SDL3 CONFIG QUIET COMPONENTS SDL3)
if(SDL3_FOUND)
    message(STATUS "Using system SDL3 ${SDL3_VERSION}")
elseif(INDY_FETCH_SDL)
    message(STATUS "System SDL3 not found - fetching from GitHub")
    include(FetchContent)
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-3.2.10
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(SDL3)
else()
    message(FATAL_ERROR "SDL3 not found. Install it (e.g. `brew install sdl3`) or enable INDY_FETCH_SDL.")
endif()

# ---------------------------------------------------------------------------
# indy_core: the machine itself. Deliberately has NO dependency on SDL so it
# can be unit-tested, run headless, or driven by a different frontend.
# ---------------------------------------------------------------------------
add_library(indy_core STATIC
    src/core/Scheduler.cpp
    src/core/System.cpp
    src/core/EmulationThread.cpp
    src/cpu/DecodedCache.cpp
    src/cpu/R5000.cpp
    src/cpu/R5000Execute.cpp
    src/cpu/Fpu.cpp
    src/cpu/Tlb.cpp
    src/memory/MemoryBus.cpp
    src/memory/MemoryController.cpp
    src/memory/PromImage.cpp
    src/scsi/ScsiController.cpp
    src/scsi/CdRomDrive.cpp
    src/scsi/DiskImage.cpp
    src/scsi/ScsiDisk.cpp
    src/scsi/Wd33c93.cpp
    src/graphics/Newport.cpp
    src/graphics/TestPatternDisplay.cpp
    src/hpc/Ds1386.cpp
    src/hpc/Hpc3.cpp
    src/hpc/Int3.cpp
    src/hpc/Pit8254.cpp
    src/hpc/SgiNvram.cpp
    src/hpc/Ioc2.cpp
    src/input/Kbc8042.cpp
    src/input/TextKeys.cpp
    src/serial/Z8530.cpp
)
add_library(indy::core ALIAS indy_core)
# GL pass-through (glpass/, see glpass/README.md): the host GPU through legacy
# OpenGL on macOS; a bridge that reports itself unavailable elsewhere.
target_include_directories(indy_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/glpass/host)
if(APPLE)
    target_sources(indy_core PRIVATE glpass/host/GlBridgeMac.cpp)
    target_link_libraries(indy_core PUBLIC "-framework OpenGL")
else()
    target_sources(indy_core PRIVATE glpass/host/GlBridgeStub.cpp)
endif()
# The guest FPU runs on host IEEE arithmetic under the guest's rounding mode:
# keep the compiler from folding or reordering it across fenv calls.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(src/cpu/Fpu.cpp PROPERTIES COMPILE_OPTIONS "-ffp-model=strict")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(src/cpu/Fpu.cpp PROPERTIES COMPILE_OPTIONS "-frounding-math;-fsignaling-nans")
elseif(MSVC)
    set_source_files_properties(src/cpu/Fpu.cpp PROPERTIES COMPILE_OPTIONS "/fp:strict")
endif()
target_include_directories(indy_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(indy_core
    PUBLIC  Threads::Threads
    PRIVATE indy::options)

# ---------------------------------------------------------------------------
# indy: the SDL3 desktop frontend executable
# ---------------------------------------------------------------------------
add_executable(indy
    src/frontend/main.cpp
    src/frontend/SdlFrontend.cpp
    src/frontend/HostTerminal.cpp
)
target_link_libraries(indy PRIVATE indy::core SDL3::SDL3 indy::options)
target_compile_definitions(indy PRIVATE INDY_VERSION="${PROJECT_VERSION}")

# Windows: copy SDL3.dll next to the executable so it runs from the build tree.
if(WIN32)
    add_custom_command(TARGET indy POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:indy> $<TARGET_FILE_DIR:indy>
        COMMAND_EXPAND_LISTS)
endif()

# ---------------------------------------------------------------------------
# Link-time optimisation
# ---------------------------------------------------------------------------
if(INDY_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT INDY_IPO_OK OUTPUT INDY_IPO_MSG LANGUAGES CXX)
    if(INDY_IPO_OK)
        foreach(tgt IN ITEMS indy_core indy)
            set_target_properties(${tgt} PROPERTIES
                INTERPROCEDURAL_OPTIMIZATION_RELEASE        ON
                INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        endforeach()
    else()
        message(STATUS "LTO not supported: ${INDY_IPO_MSG}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Unit tests (core only, no SDL): ctest --test-dir build
# ---------------------------------------------------------------------------
if(INDY_BUILD_TESTS)
    enable_testing()
    add_executable(indy_tests tests/CoreTests.cpp)
    target_link_libraries(indy_tests PRIVATE indy::core indy::options)
    add_test(NAME indy_core_tests COMMAND indy_tests)
    # CPU interpreter speed, without devices or pacing: indy_bench [seconds]
    add_executable(indy_bench tests/CpuBench.cpp)
    target_link_libraries(indy_bench PRIVATE indy::core indy::options)
endif()
