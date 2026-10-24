cmake_minimum_required(VERSION 3.10)

# Compiler selection options (must be set before project() call)
option(USE_CLANG "Use clang/clang++ compiler" OFF)
option(USE_GCC "Use gcc/g++ compiler" OFF)

# Set compilers based on options (before project call)
if(USE_CLANG AND USE_GCC)
    message(FATAL_ERROR "Cannot specify both USE_CLANG and USE_GCC. Choose one or neither.")
elseif(USE_CLANG)
    message(STATUS "Forcing clang compiler")
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
elseif(USE_GCC)
    message(STATUS "Forcing GCC compiler")
    set(CMAKE_C_COMPILER gcc)
    set(CMAKE_CXX_COMPILER g++)
else()
    message(STATUS "Using default system compiler")
endif()

project(xcplib VERSION 0.9.2 LANGUAGES C CXX)

# Find required packages
find_package(Threads REQUIRED)


# Set default generator for Windows to Visual Studio 2022
if(WIN32 AND NOT DEFINED CMAKE_GENERATOR)
    set(CMAKE_GENERATOR "Visual Studio 17 2022")
endif()

# Enable compile commands for VS Code IntelliSense
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set C and C++ standards globally
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Set C++ standard based on platform
if(WIN32)
    set(CMAKE_CXX_STANDARD 20)
    message(STATUS "Using C++20 for Windows")
else()
    set(CMAKE_CXX_STANDARD 17)
    message(STATUS "Using C++17 for non-Windows platforms")
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# Platform detection for library selection
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(STATUS "64-bit platform")
    set(PLATFORM_64BIT TRUE)
else()
    message(STATUS "32-bit platform")
    set(PLATFORM_32BIT TRUE)
endif()

# Platform-specific settings
if(WIN32)
   message(STATUS "Building for Windows")
   set(WINDOWS TRUE)
elseif(APPLE)
   message(STATUS "Building for macOS")
   set(MACOS TRUE)
elseif(UNIX)
    message(STATUS "Building for Unix/Linux")
    set(LINUX TRUE)
endif()

# Compiler-specific settings for better compatibility
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Enable additional warnings for GCC and Clang
    set(COMMON_WARNING_FLAGS -pedantic -Wall)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        message(STATUS "Using GCC compiler")
    elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        message(STATUS "Using Clang compiler")
    endif()
elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    message(STATUS "Using MSVC compiler")
    set(COMMON_WARNING_FLAGS /W3)
endif()

# Custom build type configurations for debug symbols in release builds
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # For Release builds: use -O2 optimization and remove debug symbols and assertions
    set(CMAKE_C_FLAGS_RELEASE "-O2  -DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "-O2  -DNDEBUG")
    # For RelWithDebInfo builds: release build with moderate optimization and with debug symbols, preserve stack frames
    # -fno-omit-frame-pointer: Always generate frame pointer for better stack traces
    # -fno-optimize-sibling-calls: Prevent tail call optimization that can confuse debuggers
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O1 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls -DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O1 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls -DNDEBUG")
    # For Debug builds: no optimization, full debug symbols
    set(CMAKE_C_FLAGS_DEBUG "-O0 -g")
    set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")
    message(STATUS "Custom compiler flags set")
elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    # MSVC equivalent settings
    set(CMAKE_C_FLAGS_RELEASE "/O2 /Zi /DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Zi /DNDEBUG")
    # /Oy- disables frame pointer omission (equivalent to -fno-omit-frame-pointer)
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "/O2 /Zi /Oy- /DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/O2 /Zi /Oy- /DNDEBUG")
    message(STATUS "Custom MSVC flags set")
endif()

# xcplib sources
set(xcplib_SOURCES src/xcpAppl.c src/xcpLite.c src/xcpEthServer.c src/xcpEthTl.c src/xcpQueue32.c src/xcpQueue64.c src/a2l.c src/persistence.c src/platform.c src/xcpLog.c)   

set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR})
message(STATUS "PROJECT_ROOT is: ${PROJECT_ROOT}")

# Create xcplib library
set_source_files_properties(${xcplib_SOURCES} PROPERTIES LANGUAGE C)
add_library(xcplib ${xcplib_SOURCES})
target_include_directories(xcplib PUBLIC "${PROJECT_ROOT}/inc" "${PROJECT_ROOT}/src")

# Apply compiler-specific flags
if(COMMON_WARNING_FLAGS)
    target_compile_options(xcplib PRIVATE ${COMMON_WARNING_FLAGS})
endif()

# Platform-specific compile definitions
if(WIN32)
    target_compile_definitions(xcplib PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Link threads library to xcplib (needed for pthread usage in platform.h)
target_link_libraries(xcplib PUBLIC Threads::Threads)

# Check for atomic library support (needed on some ARM platforms with clang)
include(CheckLibraryExists)
check_library_exists(atomic __atomic_store_4 "" HAVE_LIBATOMIC)
# if(HAVE_LIBATOMIC)
#     message(STATUS "Linking with atomic library for C11 atomics support")
#     target_link_libraries(xcplib PUBLIC atomic)
# endif()



