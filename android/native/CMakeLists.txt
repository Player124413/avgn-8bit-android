# Android (NDK) build of the Butterscotch runtime that powers the AVGN 8-bit port.
#
# The runtime sources live in ../../runtime (Butterscotch, AGPL-3.0, see runtime/LICENSE) with the
# game-specific fixes the PS Vita port needed. Only the pieces this platform uses are compiled:
# no SDL/desktop backends, no Vita/PS2/Switch code, no FFmpeg (video_play is a no-op on Android, see
# src/video/null_video.c), miniaudio for sound and FreeType for font_add().
cmake_minimum_required(VERSION 3.22.1)

project(avgn8bit C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(BS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../runtime" ABSOLUTE)

# --------------------------------------------------------------------------------------------------------------------
# FreeType (font_add): AVGN loads Silver.ttf / LanaPixel.ttf / Probly8-CJK.ttf at runtime.
# No optional dependencies: zlib/bzip2/png/harfbuzz/brotli are all switched off, so it builds with the NDK alone.
# --------------------------------------------------------------------------------------------------------------------
set(FT_DISABLE_ZLIB TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI TRUE CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${BS_ROOT}/vendor/freetype" freetype_build EXCLUDE_FROM_ALL)

# --------------------------------------------------------------------------------------------------------------------
# Runtime sources
# --------------------------------------------------------------------------------------------------------------------
file(GLOB BS_CORE_SOURCES "${BS_ROOT}/src/*.c")
list(REMOVE_ITEM BS_CORE_SOURCES "${BS_ROOT}/src/loop.c")   # desktop main loop; the Android host drives the frames
file(GLOB BS_GL_SOURCES "${BS_ROOT}/src/gl/*.c" "${BS_ROOT}/src/gl_common/*.c" "${BS_ROOT}/src/image/*.c" "${BS_ROOT}/src/debug_font/*.c")
file(GLOB BS_PHYSICS_SOURCES "${BS_ROOT}/src/physics/*.c")
file(GLOB BS_AUDIO_SOURCES "${BS_ROOT}/src/audio/miniaudio/*.c")

file(GLOB BOX2D_SOURCES "${BS_ROOT}/vendor/box2d/src/*.c")
list(REMOVE_ITEM BOX2D_SOURCES "${BS_ROOT}/vendor/box2d/src/timer.c")

add_library(avgn8bit_box2d STATIC ${BOX2D_SOURCES})
target_include_directories(avgn8bit_box2d BEFORE PUBLIC "${BS_ROOT}/vendor/box2d/include")
target_include_directories(avgn8bit_box2d PRIVATE "${BS_ROOT}/src")
target_compile_definitions(avgn8bit_box2d PRIVATE BOX2D_DISABLE_SIMD B2_SINGLE_THREADED)
target_compile_options(avgn8bit_box2d PRIVATE -w -fno-fast-math -ffp-contract=off)

add_library(avgn8bit_bzip2 STATIC
    "${BS_ROOT}/vendor/bzip2/bz_internal_error.c"
    "${BS_ROOT}/vendor/bzip2/bzlib.c"
    "${BS_ROOT}/vendor/bzip2/crctable.c"
    "${BS_ROOT}/vendor/bzip2/decompress.c"
    "${BS_ROOT}/vendor/bzip2/huffman.c"
    "${BS_ROOT}/vendor/bzip2/randtable.c"
)
target_include_directories(avgn8bit_bzip2 PUBLIC "${BS_ROOT}/vendor/bzip2")
target_compile_options(avgn8bit_bzip2 PRIVATE -w)

add_library(avgn8bit_sha1 STATIC "${BS_ROOT}/vendor/sha1/sha1.c")
target_include_directories(avgn8bit_sha1 SYSTEM PUBLIC "${BS_ROOT}/vendor/sha1")
target_compile_options(avgn8bit_sha1 PRIVATE -w)

add_library(avgn8bit SHARED
    ${BS_CORE_SOURCES}
    ${BS_GL_SOURCES}
    ${BS_PHYSICS_SOURCES}
    ${BS_AUDIO_SOURCES}
    "${BS_ROOT}/src/android/main.c"
    "${BS_ROOT}/src/android/stb_impl.c"
    "${BS_ROOT}/src/video/null_video.c"
    "${BS_ROOT}/vendor/md5/md5.c"
    "${BS_ROOT}/vendor/base64/base64.c"
    "${BS_ROOT}/vendor/miniz/miniz.c"
    "${BS_ROOT}/vendor/miniz/miniz_tdef.c"
    "${BS_ROOT}/vendor/miniz/miniz_tinfl.c"
)

target_include_directories(avgn8bit PRIVATE
    "${BS_ROOT}/src"
    "${BS_ROOT}/src/android"
    "${BS_ROOT}/src/gl"
    "${BS_ROOT}/src/gl_common"
    "${BS_ROOT}/src/image"
    "${BS_ROOT}/src/debug_font"
    "${BS_ROOT}/src/video"
    "${BS_ROOT}/src/audio"
    "${BS_ROOT}/src/audio/miniaudio"
    "${BS_ROOT}/vendor/box2d/include"
    "${BS_ROOT}/vendor/miniz"
    "${BS_ROOT}/vendor/bzip2"
    "${BS_ROOT}/vendor/md5"
    "${BS_ROOT}/vendor/base64"
    "${BS_ROOT}/vendor/sha1"
)
target_include_directories(avgn8bit SYSTEM PRIVATE
    "${BS_ROOT}/vendor/stb/ds"
    "${BS_ROOT}/vendor/stb/image"
    "${BS_ROOT}/vendor/stb/vorbis"
    "${BS_ROOT}/vendor/miniaudio"
)

target_compile_definitions(avgn8bit PRIVATE
    ENABLE_GLES
    PLATFORM_ANDROID
    ENABLE_MODERN_GL
    ENABLE_PHYSICS
    ENABLE_WAD14
    ENABLE_WAD16
    ENABLE_WAD17
    ENABLE_VM_STUB_LOGS
    USE_MINIAUDIO
    MA_NO_RUNTIME_LINKING
    MINIZ_NO_ARCHIVE_APIS
    MINIZ_NO_STDIO
    BS_HAVE_FREETYPE
    # The runtime keeps its "Vita" helper files (marker files, texture cache) next to data.win. Relative to the
    # process working directory, which JNI Native.setBaseDir() switches to the app's data folder.
    BS_VITA_DATA_DIR="."
)

target_compile_options(avgn8bit PRIVATE
    -O2
    -ffast-math
    -fno-finite-math-only
    -fomit-frame-pointer
    -fno-strict-aliasing
    -Wall
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-missing-field-initializers
)

target_link_libraries(avgn8bit
    avgn8bit_box2d
    avgn8bit_bzip2
    avgn8bit_sha1
    freetype
    GLESv3
    OpenSLES
    log
    android
    m
    dl
)
