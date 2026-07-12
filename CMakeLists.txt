cmake_minimum_required(VERSION 3.25)

project(MediaCurator
    VERSION 1.3.2
    DESCRIPTION "Smart media library curator — scan, filter, and clean your video collection"
    HOMEPAGE_URL "https://github.com/bleze/MediaCurator"
    LANGUAGES CXX
)

# ── C++ Standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Output directories ─────────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ── Platform detection ─────────────────────────────────────────────────────────
if(WIN32)
    add_compile_definitions(MC_PLATFORM_WINDOWS)
elseif(APPLE)
    add_compile_definitions(MC_PLATFORM_MACOS)
else()
    add_compile_definitions(MC_PLATFORM_LINUX)
endif()

# ── MSVC-specific flags ────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /WX- /utf-8 /MP)
    add_compile_definitions(_UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    # Treat angle-bracket includes (Qt, STL, third-party) as external so MSVC
    # Code Analysis does not flag issues inside third-party headers.
    add_compile_options(/external:anglebrackets /external:W0)
endif()

# ── Find Qt6 ──────────────────────────────────────────────────────────────────
# Set Qt6_DIR in your CMake config or via:
#   cmake -DQt6_DIR="D:/Development/Environment/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6" ...
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Sql
    Concurrent
    Network
    Svg
)

qt_standard_project_setup()

# ── Generated version header ──────────────────────────────────────────────────
# Single source of truth for the app version is the project(VERSION ...) call
# above. Everything else (CPack, macOS bundle, in-app "About"/splash, this
# header) derives from PROJECT_VERSION — bump it in one place per release.
configure_file(
    "${CMAKE_SOURCE_DIR}/include/core/Version.h.in"
    "${CMAKE_BINARY_DIR}/generated/core/Version.h"
    @ONLY
)

# ── nlohmann/json (vendored single-header) ────────────────────────────────────
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${CMAKE_SOURCE_DIR}/third_party")
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)

# ── Sub-directories ────────────────────────────────────────────────────────────
add_subdirectory(src)

# ── CPack installer config ────────────────────────────────────────────────────
include(cmake/Packaging.cmake)
