cmake_minimum_required(VERSION 3.25)

project(MediaCurator
    VERSION 1.8.3
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
    # /W4 is scoped to the MediaCurator target only (src/CMakeLists.txt), not
    # applied directory-wide here — the vendored cld2 target below sets its own
    # /w to silence upstream warnings, and a directory-wide /W4 would still
    # reach cld2's compile command ahead of its /w, which MSVC flags with
    # D9025 ("overriding '/W4' with '/w'") since the two conflict on the
    # command line.
    add_compile_options(/utf-8 /MP)
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

# ── CLD2 (vendored "lite" build — n-gram language detection for subtitle text) ─
# Pinned at CLD2Owners/cld2@b56fa78, Apache-2.0 (see third_party/cld2/LICENSE).
# This is the same table set ("quadchrome_2") Chrome shipped for years — accurate
# enough for a few paragraphs of dialogue without the ~70MB "full" table set.
set(CLD2_DIR "${CMAKE_SOURCE_DIR}/third_party/cld2")
add_library(cld2 STATIC
    "${CLD2_DIR}/internal/cldutil.cc"
    "${CLD2_DIR}/internal/cldutil_shared.cc"
    "${CLD2_DIR}/internal/compact_lang_det.cc"
    "${CLD2_DIR}/internal/compact_lang_det_hint_code.cc"
    "${CLD2_DIR}/internal/compact_lang_det_impl.cc"
    "${CLD2_DIR}/internal/debug.cc"
    "${CLD2_DIR}/internal/fixunicodevalue.cc"
    "${CLD2_DIR}/internal/generated_entities.cc"
    "${CLD2_DIR}/internal/generated_language.cc"
    "${CLD2_DIR}/internal/generated_ulscript.cc"
    "${CLD2_DIR}/internal/getonescriptspan.cc"
    "${CLD2_DIR}/internal/lang_script.cc"
    "${CLD2_DIR}/internal/offsetmap.cc"
    "${CLD2_DIR}/internal/scoreonescriptspan.cc"
    "${CLD2_DIR}/internal/tote.cc"
    "${CLD2_DIR}/internal/utf8statetable.cc"
    "${CLD2_DIR}/internal/cld_generated_cjk_uni_prop_80.cc"
    "${CLD2_DIR}/internal/cld2_generated_cjk_compatible.cc"
    "${CLD2_DIR}/internal/cld_generated_cjk_delta_bi_4.cc"
    "${CLD2_DIR}/internal/generated_distinct_bi_0.cc"
    "${CLD2_DIR}/internal/cld2_generated_quadchrome_2.cc"
    "${CLD2_DIR}/internal/cld2_generated_deltaoctachrome.cc"
    "${CLD2_DIR}/internal/cld2_generated_distinctoctachrome.cc"
    "${CLD2_DIR}/internal/cld_generated_score_quad_octa_2.cc"
)
target_include_directories(cld2
    PUBLIC  "${CLD2_DIR}/public"
    PRIVATE "${CLD2_DIR}/internal"
)
# Vendored source we don't maintain — silence rather than patch upstream warnings.
if(MSVC)
    # /W0 (not /w): CMake's Visual Studio generator recognizes /W<n> and writes
    # it straight into the vcxproj's <WarningLevel> property. Plain /w gets
    # dropped into AdditionalOptions as a raw string instead, which then sits
    # alongside whatever WarningLevel CMake still defaults the project to —
    # MSVC sees both on the actual command line and emits D9025 ("overriding
    # '/W1' with '/w'") even though functionally /w still wins either way.
    target_compile_options(cld2 PRIVATE /W0)
else()
    target_compile_options(cld2 PRIVATE -w)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Clang treats C++11 narrowing in brace-init as a hard error that plain
        # -w cannot silence (GCC/MSVC don't enforce it the same way) — cld2's
        # generated lookup tables rely on it throughout (e.g. negative int
        # literals narrowed to uint8 in cld_generated_cjk_uni_prop_80.cc).
        target_compile_options(cld2 PRIVATE -Wno-c++11-narrowing)
    endif()
endif()
add_library(cld2::cld2 ALIAS cld2)

# ── Sub-directories ────────────────────────────────────────────────────────────
add_subdirectory(src)

# ── CPack installer config ────────────────────────────────────────────────────
include(cmake/Packaging.cmake)
