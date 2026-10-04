cmake_minimum_required(VERSION 3.20)
project(HybridHeavenRecompiled CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FetchContent's URL downloads warn about extraction timestamps otherwise. Set
# before any FetchContent_Declare, which on Windows means before everything.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

# --- Windows ------------------------------------------------------------------
# The port is a Windows target as much as a Linux one; this section is what makes
# the same tree configure under clang-cl. Everything here follows
# Goemon64Recomp's Windows build, which is the one this UI layer came from.
if(WIN32)
    # lib/FindFreetype.cmake resolves the vendored freetype-windows-binaries
    # submodule. There is no system FreeType to find, so find_package(Freetype)
    # below only works once this is on the module path.
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/lib/")

    # Opt out of the constexpr mutex constructor, so the binary does not demand a
    # newer vcredist than the one a user is likely to have.
    add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

    # windows.h defines min and max as macros, which breaks any header declaring
    # functions by those names -- hlslpp, which rt64's render context pulls in,
    # has a whole family of them. rt64's own CMakeLists sets NOMINMAX, but
    # add_compile_definitions is directory-scoped, so it never reaches this
    # project's targets. Set here, before any add_subdirectory, so it covers both.
    add_compile_definitions(NOMINMAX)

    # clang-cl defaults to a far noisier warning set than either GCC on Linux or
    # cl.exe: -Wc++98-compat and -Wunsafe-buffer-usage alone produce hundreds of
    # lines per vendored translation unit, which buries real diagnostics. Reset to
    # /W4, the same thing Goemon64Recomp does for this compiler.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(-Wno-everything /W4)

        # Every SDL2 in this build -- rt64's vendored 2.26.3 and the 2.30.3 the
        # Windows branch fetches -- ships an SDL_endian.h that defines
        # `_m_prefetch` itself, working around a Clang 11 bug its own comment
        # calls temporary. It is long fixed: modern clang has `_m_prefetch` as a
        # builtin, and defining a builtin is a hard error, so anything including
        # SDL_endian.h fails to compile.
        #
        # SDL skips its version when `__PRFCHWINTRIN_H` is already defined, so
        # force-including <intrin.h> lets clang's own prfchwintrin.h set that
        # guard first. Global rather than per-target because both SDLs are hit,
        # in rt64 and in this project's own sources. Defining __PRFCHWINTRIN_H
        # directly would also silence SDL, but it would gag clang's real header
        # too and leave `_m_prefetchw` undefined for anything that wants it.
        add_compile_options(/FIintrin.h)
    endif()

    # THE WINDOWS BUILD NEEDS clang-cl, NOT MSVC's OWN cl.exe, and this is a hard
    # error rather than a warning because of how it fails otherwise.
    #
    # recomp.h picks RECOMP_FUNC by compiler. Its clang branch marks every
    # recompiled function `extern inline __attribute__((weak,noinline))`; its MSVC
    # branch is a plain `__declspec(noinline)` with no weak linkage at all. The
    # patch override depends on that weakness: a patched function is emitted
    # twice, once by the main recompilation and once by the patch recompilation,
    # and the override works only because the linker is free to pick PatchesLib's
    # definition over RecompiledFuncs'. With two strong definitions the result is
    # decided by which archive the linker reaches first -- and that is not a
    # hypothetical, it is exactly the bug that kept the overlay loader hook from
    # ever firing here: a build that succeeds, runs, and has no patches applied.
    #
    # So refuse to configure rather than emit a binary whose patches are silently
    # absent. See the link-order comment further down, which is the same hazard.
    if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR
            "The Windows build requires clang-cl (CMAKE_CXX_COMPILER_ID would be "
            "\"Clang\"), but this is MSVC ${CMAKE_CXX_COMPILER_ID}. recomp.h's MSVC "
            "branch does not mark recompiled functions weak, which silently breaks "
            "the patch override -- see the comment at this line. Install the "
            "\"C++ Clang tools for Windows\" component in the Visual Studio "
            "installer and configure with -T ClangCL, or point CMAKE_C_COMPILER and "
            "CMAKE_CXX_COMPILER at clang-cl.exe directly.")
    endif()
endif()

# ---------------------------------------------------------------------------
# What this builds
#
# The recompiled game, the recompiled patches, the glue that registers both with
# librecomp, and the application: main.cpp, the RT64 render context, and the
# RmlUi launcher and config menus.
#
# Structure follows Goemon64Recomp/CMakeLists.txt, which the UI layer was taken
# from -- the two share lib/rt64 at the same commit and lib/N64ModernRuntime
# branches off the same one.
# ---------------------------------------------------------------------------

set(HH_RUNTIME ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)

# --- Android ------------------------------------------------------------------
# Android is a cross-build with no executable at the end of it: SDLActivity loads
# a shared library and calls SDL_main inside it. Two consequences run through the
# rest of this file -- every build-time TOOL has to be a host binary, and the main
# target is a .so rather than an .exe.
#
# This block has to come BEFORE add_subdirectory(lib/rt64) below, because rt64
# reads RT64_FILE_TO_C during its own configure to embed its shaders. Setting it
# afterwards would leave rt64 trying to run an arm64 file_to_c on the build host.
#
# Nothing here patches the submodules: lib/rt64 is already on the goemon-android
# branch and lib/N64ModernRuntime's hybridheaven-port branch already contains all
# of goemon-android, so the renderer and the runtime arrive Android-ready. That
# includes the RSP -- rsp_vu.hpp takes its <sse2neon.h> branch on __aarch64__ and
# ultramodern already puts thirdparty/sse2neon on the include path -- so the
# SSSE3 intrinsics in rsp/aspMain.cpp need no work of their own. The -march
# further down is already gated to x86_64 and simply does not apply here.
if(ANDROID)
    include(FetchContent)

    # 16 KB page-size compliance, which Play requires from 2027-02-01. Set
    # globally rather than as a target link option so it also covers the SDL2
    # that rt64 fetches: that is a separate .so in the APK and a per-target
    # option would miss it. Only the shared-linker flags matter, since this path
    # builds no executables.
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,-z,max-page-size=16384")

    # file_to_c and DXC run ON THE BUILD MACHINE -- they emit C blobs and SPIR-V
    # that are then compiled for the target -- so cross-building them with the
    # NDK produces arm64 binaries the build cannot execute. gradle passes a
    # prebuilt host one in. The HH_UI block further down picks the same binary up
    # through its CMAKE_CROSSCOMPILING branch, which the Windows cross-build
    # already established.
    if(NOT RT64_FILE_TO_C)
        message(FATAL_ERROR
            "The Android build needs -DRT64_FILE_TO_C pointing at a HOST-built "
            "file_to_c; gradle passes build-host-tools/file_to_c_host. Build one "
            "with:\n"
            "  c++ -std=c++17 -O2 -o build-host-tools/file_to_c_host "
            "lib/rt64/src/tools/file_to_c/file_to_c.cpp")
    endif()

    # There is no system FreeType to find_package on Android, and RmlUi's font
    # engine needs the Freetype::Freetype target that find_package would have
    # supplied. Build it as part of the NDK build instead, with every optional
    # codec off -- the UI only rasterises glyphs.
    FetchContent_Declare(
        freetype
        GIT_REPOSITORY https://github.com/freetype/freetype.git
        GIT_TAG VER-2-13-3
        GIT_SHALLOW TRUE
    )
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(freetype)
    if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()
endif()

# The version the launcher UI shows. Passed in by the build -- gradle derives it
# from the git tag via -PvName -- so it cannot drift from the release it ships
# in. main.cpp falls back to a dev string when it is not defined, which is the
# case for every local desktop build.
if(DEFINED HH_VERSION_STRING)
    add_compile_definitions(HH_VERSION_STRING="${HH_VERSION_STRING}")
endif()

# --- The renderer -------------------------------------------------------------
# ON  -> src/main/rt64_render_context.cpp, and a picture.
# OFF -> src/main/null_render_context.cpp, which implements RendererContext as
#        no-ops. That is still worth having: it runs everything below the
#        display list without standing up RT64, so a failure that survives it is
#        not a renderer bug. Configure with -DHH_RT64=OFF to bisect.
option(HH_RT64 "Build with the RT64 renderer" ON)

if(HH_RT64)
    set(RT64_STATIC TRUE)
    set(RT64_SDL_WINDOW_VULKAN TRUE)
    # plume predates CMake's normal-variable propagation into subdirectories
    # (it asks for 3.12), so this has to be a CACHE entry to reach it.
    if(CMAKE_SYSTEM_NAME MATCHES "Linux" AND RT64_SDL_WINDOW_VULKAN)
        set(SDL_VULKAN_ENABLED ON CACHE BOOL "Forcing Plume to use Vulkan on Linux" FORCE)
    endif()
    add_compile_definitions(HLSL_CPU)
    add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)
    # rt64 generates shader headers into its own binary dir and includes them
    # unqualified.
    target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/rt64/src)

endif()

# --- The UI -------------------------------------------------------------------
# The RmlUi launcher and config menus (src/ui/), from Goemon64Recomp.
#
# It cannot be built without the renderer: ui_renderer.cpp draws through RT64's
# own render hooks and its interface shaders are compiled by rt64's DXC rules. So
# -DHH_RT64=OFF forces this off too, which keeps the null-renderer bisect build
# working -- with no UI, main.cpp starts the game directly, exactly as it did
# before the launcher existed.
option(HH_UI "Build the RmlUi launcher and config menus" ON)

if(HH_UI AND NOT HH_RT64)
    message(STATUS "HH_RT64=OFF, so HH_UI is forced OFF -- the UI draws through RT64.")
    set(HH_UI OFF)
endif()

if(HH_UI)
    # lunasvg for the <svg> elements in the menus, then RmlUi with its SVG plugin
    # pointed at it. Both are static: BUILD_SHARED_LIBS is forced off around them
    # so a shared-libs configure cannot produce a .so nothing installs.
    set(BUILD_SHARED_LIBS OFF)
    set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/lib/lunasvg)
    set(RMLUI_SVG_PLUGIN ON CACHE BOOL "" FORCE)
    set(RMLUI_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/lib/RmlUi)
    target_compile_definitions(rmlui_core PRIVATE LUNASVG_BUILD_STATIC)
endif()

# ultramodern + librecomp. Self-contained: it pulls in miniz, N64Recomp and
# LiveRecomp itself and does not need rt64, which is what makes a build without
# the renderer possible at all.
add_subdirectory(${HH_RUNTIME} ${CMAKE_BINARY_DIR}/N64ModernRuntime)

# --- Optional user-supplied Vulkan driver (libadrenotools) ---------------------
# Android only, and OFF by default, which leaves the Vulkan init path untouched
# -- the hook goes in plume's volkInitialize call, which every platform runs, so
# nothing here can reach a build that does not ask for it. gradle turns it on
# via -PcustomDriver=true, which
# ALSO flips jniLibs.useLegacyPackaging: the hook libraries below have to exist
# as real files in nativeLibraryDir, and AGP 8's default (uncompressed, read
# straight from the APK) leaves that directory without them. One gradle flag
# drives both because either half alone is broken -- see android/app/build.gradle.
if(ANDROID AND HH_CUSTOM_VULKAN_DRIVER)
    if(NOT HH_RT64)
        message(FATAL_ERROR
            "HH_CUSTOM_VULKAN_DRIVER needs HH_RT64=ON: the loader is handed to "
            "plume, and the null render context never creates a Vulkan device.")
    endif()
    if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR
            "HH_CUSTOM_VULKAN_DRIVER requires arm64-v8a (libadrenotools supports "
            "no other ABI).")
    endif()
    # Builds libadrenotools (static, linked into libHybridHeaven.so -- it inherits
    # the BUILD_SHARED_LIBS OFF set above) plus four SHARED hook libraries --
    # main_hook, file_redirect_hook, gsl_alloc_hook, hook_impl -- which are loaded
    # by path at runtime rather than linked. They land in CMake's library output
    # directory, which is how AGP finds and packages them.
    add_subdirectory(${CMAKE_SOURCE_DIR}/lib/libadrenotools ${CMAKE_BINARY_DIR}/libadrenotools)
    # plume's hook is behind its own define, so a build without this option
    # compiles the original volkInitialize() statement unchanged.
    target_compile_definitions(plume PRIVATE PLUME_CUSTOM_VULKAN_LOADER)
endif()

set(HH_RECOMP_INCLUDES
    ${CMAKE_SOURCE_DIR}/include
    ${HH_RUNTIME}/ultramodern/include
    ${HH_RUNTIME}/librecomp/include
    ${HH_RUNTIME}/N64Recomp/include
    # ultramodern.hpp includes <lightweightsemaphore.h> unqualified, so the
    # thirdparty directories have to be on the path the same way
    # N64ModernRuntime's own CMakeLists puts them there.
    ${HH_RUNTIME}/thirdparty
    ${HH_RUNTIME}/thirdparty/concurrentqueue
    # rsp_vu.hpp reaches for <sse2neon.h> on __aarch64__, which is how the RSP's
    # SSSE3 intrinsics survive on Arm. ultramodern already exports this directory
    # to anything that LINKS it, but the targets built from this list only get
    # include paths named here -- HybridHeavenRuntime does not link ultramodern --
    # so it has to be repeated. Harmless on x86_64, where rsp_vu.hpp takes its
    # <nmmintrin.h> branch and never looks for the header at all.
    ${HH_RUNTIME}/thirdparty/sse2neon
)

# The recompiler emits calls to `<name>_recomp` for every libultra function it
# skips, without declaring them -- the definitions live in librecomp. GCC 14
# promotes an implicit declaration from a warning to an error, so
# -Wno-implicit-function-declaration (what Goemon64Recomp uses) is no longer
# enough on its own and the -Wno-error form is required.
#
# This deliberately does NOT hide a real problem: a `_recomp` symbol that
# nothing defines still fails at link, and tools/recomp_symbol_gap.py reports
# the whole class up front rather than one compile error at a time.
set(HH_RECOMP_FLAGS
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-implicit-function-declaration
    -Wno-error=implicit-function-declaration
)

# clang-cl understands all four of those, but COFF caps an object file at 65,279
# sections and the recompiled translation units are the largest in the project --
# one section per function, thousands of functions per file. /bigobj lifts the
# cap. Added on the two recompiled targets only, which are the only ones near it.
if(WIN32)
    list(APPEND HH_RECOMP_FLAGS /bigobj)
endif()

# --- RecompiledFuncs: the whole game -----------------------------------------
file(GLOB FUNC_C_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c)
file(GLOB FUNC_CXX_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.cpp)

if(NOT FUNC_C_SOURCES)
    message(FATAL_ERROR
        "No recompiler output in RecompiledFuncs/. Build the decomp and run the "
        "recompiler first -- see \"Generate the C sources\" in BUILDING.md.")
endif()

add_library(RecompiledFuncs STATIC ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES})
target_compile_options(RecompiledFuncs PRIVATE ${HH_RECOMP_FLAGS})
target_include_directories(RecompiledFuncs PRIVATE ${HH_RECOMP_INCLUDES})

# --- Patch codegen ------------------------------------------------------------
# Build the MIPS patch ELF, recompile it to C, and embed the resulting blob.
# These are host build steps; on Android the generated files are consumed as
# static sources instead, exactly as Goemon64Recomp does it.
#
# Windows and Android are the same case and for the same reason: the codegen
# shells out to `make` with clang and ld.lld to link a MIPS ELF, then runs the
# N64Recomp binary over it. A stock Windows toolchain has none of that, and
# inside the NDK cross-build every one of those tools would be built for arm64
# and could not execute on the build host. So both consume RecompiledPatches/ as
# static sources, which means it has to be generated on the Linux side first --
# and since RecompiledFuncs/ and RecompiledPatches/ are both gitignored, a fresh
# checkout has neither.
#
# Checked here rather than discovered as a missing-file error partway through the
# build. That distinction is worth the lines: patches.c is named explicitly as a
# source below, so its absence at least fails at generate time, but
# RecompiledFuncs/ is a GLOB that simply comes back empty -- which on Android
# would link a .so with no game in it and no diagnostic anywhere. Silently
# dropping the patches is this port's most repeated failure; do not add a sixth
# route to it.
if(WIN32 OR ANDROID)
    if(ANDROID)
        set(HH_NO_CODEGEN_HOST "Android")
    else()
        set(HH_NO_CODEGEN_HOST "Windows")
    endif()
    foreach(generated
            ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches.c
            ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.c
            ${CMAKE_SOURCE_DIR}/RecompiledPatches/recomp_overlays.inl)
        if(NOT EXISTS ${generated})
            message(FATAL_ERROR
                "Missing ${generated}. The ${HH_NO_CODEGEN_HOST} build does not "
                "run the patch codegen -- it needs make, clang and ld.lld to link "
                "a MIPS ELF, as host tools. Generate it on Linux first (see "
                "\"Generate the C sources\" in BUILDING.md) and build ${HH_NO_CODEGEN_HOST} "
                "against the same tree.")
        endif()
    endforeach()
endif()

if(NOT ANDROID AND NOT WIN32)
    if(NOT DEFINED HH_N64RECOMP)
        set(HH_N64RECOMP ${CMAKE_SOURCE_DIR}/N64Recomp)
    endif()
    if(NOT DEFINED PATCHES_C_COMPILER)
        set(PATCHES_C_COMPILER clang)
    endif()
    if(NOT DEFINED PATCHES_LD)
        set(PATCHES_LD ld.lld)
    endif()

    add_custom_command(OUTPUT ${CMAKE_SOURCE_DIR}/patches/build/patches.elf
        COMMAND ${CMAKE_COMMAND} -E env CC=${PATCHES_C_COMPILER} LD=${PATCHES_LD} make
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/patches
        # Every patch .c has to be listed. patches/Makefile globs them, so a new
        # one builds either way -- but an EDIT to a file missing from this list
        # leaves cmake believing patches.elf is up to date, and the port ships
        # the previous version of the patch with nothing said. That failure has
        # already happened four times in this project by four different
        # mechanisms; do not let this be the fifth.
        DEPENDS ${CMAKE_SOURCE_DIR}/patches/required.c
                ${CMAKE_SOURCE_DIR}/patches/camera.c
                ${CMAKE_SOURCE_DIR}/patches/pak.c
                ${CMAKE_SOURCE_DIR}/patches/autosave.c
                ${CMAKE_SOURCE_DIR}/patches/autosave.h
                ${CMAKE_SOURCE_DIR}/patches/patches.h
                ${CMAKE_SOURCE_DIR}/patches/misc_funcs.h
                ${CMAKE_SOURCE_DIR}/patches/game_funcs.h
                ${CMAKE_SOURCE_DIR}/patches/patches.ld
                ${CMAKE_SOURCE_DIR}/patches/syms.ld
        COMMENT "Building patches.elf")

    add_custom_command(OUTPUT
            ${CMAKE_SOURCE_DIR}/patches/patches.bin
            ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches.c
            ${CMAKE_SOURCE_DIR}/RecompiledPatches/recomp_overlays.inl
            ${CMAKE_SOURCE_DIR}/RecompiledPatches/funcs.h
        COMMAND ${HH_N64RECOMP} patches.toml
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS ${CMAKE_SOURCE_DIR}/patches/build/patches.elf
        COMMENT "Recompiling patches")

    add_custom_command(OUTPUT
            ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.c
            ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.h
        COMMAND ${CMAKE_COMMAND}
            -DINPUT=${CMAKE_SOURCE_DIR}/patches/patches.bin
            -DSYMBOL=hh_patches_bin
            -DOUT_C=${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.c
            -DOUT_H=${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.h
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_binary.cmake
        DEPENDS ${CMAKE_SOURCE_DIR}/patches/patches.bin
                ${CMAKE_SOURCE_DIR}/cmake/embed_binary.cmake
        COMMENT "Embedding patches.bin")
endif()

# --- PatchesLib: the recompiled patches ---------------------------------------
add_library(PatchesLib STATIC
    ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches.c
    ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.c
)
target_compile_options(PatchesLib PRIVATE ${HH_RECOMP_FLAGS})
target_include_directories(PatchesLib PRIVATE ${HH_RECOMP_INCLUDES})

# --- HybridHeavenRuntime: the glue --------------------------------------------
# Overlay/patch registration and the host exports the patches call. This is the
# part that has to be written rather than generated.
add_library(HybridHeavenRuntime STATIC
    ${CMAKE_SOURCE_DIR}/src/main/register_overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/main/register_patches.cpp
    ${CMAKE_SOURCE_DIR}/src/game/recomp_api.cpp
    ${CMAKE_SOURCE_DIR}/src/game/pak_shims.cpp
    ${CMAKE_SOURCE_DIR}/src/game/ultra_missing.cpp
    # The `.manual.bak` rollback point that autosave depends on. Here rather than
    # in HH_UI_SOURCES because it only observes librecomp's save hooks: it is
    # maintained even in a build with no menu to switch autosave on from.
    ${CMAKE_SOURCE_DIR}/src/game/save_rollback.cpp
    # Retail cart dump -> the decompressed image the recompiler was built from.
    # Registered as the GameEntry's decompression_routine, which is what lets the
    # port accept the ROM a player actually owns.
    ${CMAKE_SOURCE_DIR}/src/game/rom_decompression.cpp
)
target_include_directories(HybridHeavenRuntime PRIVATE ${HH_RECOMP_INCLUDES})
# This library holds the host exports the patches call, and some of them answer
# from the UI layer (the config getters, the Saved toast). It therefore has to
# know whether the UI exists: with HH_UI=OFF those implementations are not
# compiled, and src/game/recomp_api.cpp supplies "the feature is off" stubs
# instead. Without this definition the stubs would be compiled into every build
# and collide with the real ones.
target_compile_definitions(HybridHeavenRuntime PRIVATE $<$<BOOL:${HH_UI}>:HH_UI>)
add_dependencies(HybridHeavenRuntime PatchesLib)

# --- The application ----------------------------------------------------------
# With HH_RT64 on this is the game plus a picture plus the launcher. With it off
# the render context draws nothing (src/main/null_render_context.cpp) and there is
# no UI, which is still enough to run everything below the display list.
# Windows has no system SDL2 to find, so fetch the prebuilt VC development
# package. The version is read from the environment first so a CI pin does not
# need a source edit, which is how Goemon64Recomp does it.
if(WIN32)
    include(FetchContent)
    if(DEFINED ENV{SDL2_VERSION})
        set(SDL2_VERSION $ENV{SDL2_VERSION})
    else()
        set(SDL2_VERSION "2.30.3")
    endif()
    FetchContent_Declare(
        sdl2
        URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip
    )
    FetchContent_MakeAvailable(sdl2)
elseif(ANDROID)
    # Nothing to do: lib/rt64 fetches and builds SDL2 itself for the NDK build,
    # because Android has no system SDL2 either. CMake targets are global, so the
    # SDL2 target it created is already reachable from here -- but the
    # SDL2_INCLUDE_DIRS/SDL2_LIBRARIES variables it set alongside are NOT, since
    # add_subdirectory gives rt64 its own scope. Link the SDL2 target rather than
    # those variables and the interface include directories come with it.
    #
    # find_package(SDL2) here would fail outright: there is no config package in
    # the NDK sysroot, and a REQUIRED find is a hard error.
else()
    find_package(SDL2 REQUIRED)
endif()
find_package(Threads REQUIRED)

# widescreen.cpp goes with the RT64 context and only with it: it exists to give
# the widened projection somewhere to draw, and the null context draws nothing.
if(HH_RT64)
    set(HH_RENDER_CONTEXT
        ${CMAKE_SOURCE_DIR}/src/main/rt64_render_context.cpp
        ${CMAKE_SOURCE_DIR}/src/main/widescreen.cpp)
else()
    set(HH_RENDER_CONTEXT ${CMAKE_SOURCE_DIR}/src/main/null_render_context.cpp)
endif()

# The UI layer. Listed here rather than globbed so that dropping a file is a
# visible edit; the order is Goemon64Recomp's, which groups the menus, then the
# scripting API, then the element wrappers.
set(HH_UI_SOURCES
    ${CMAKE_SOURCE_DIR}/src/ui/ui_renderer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_state.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_launcher.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_config.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_prompt.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_saved_indicator.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_config_sub_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_gpu_driver.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_saves.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_color_hack.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_rml_hacks.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_elements.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_mod_details_panel.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_mod_installer.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_mod_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_api.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_api_events.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_api_images.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ui_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/util/hsv.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/core/ui_context.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/elements/ui_button.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/elements/ui_clickable.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/elements/ui_container.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/elements/ui_element.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/elements/ui_image.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/elements/ui_label.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/elements/ui_radio.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/elements/ui_scroll_container.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/elements/ui_slider.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/elements/ui_span.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/elements/ui_style.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/elements/ui_text_input.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/elements/ui_toggle.cpp

    # Config, input and the platform helpers the UI calls. These are only needed
    # with the UI: without it main.cpp reads no config and polls the keyboard
    # itself.
    ${CMAKE_SOURCE_DIR}/src/game/config.cpp
    ${CMAKE_SOURCE_DIR}/src/game/controls.cpp
    ${CMAKE_SOURCE_DIR}/src/game/input.cpp
    ${CMAKE_SOURCE_DIR}/src/main/support.cpp

    # RmlUi's own SDL platform backend (key/mouse translation).
    ${CMAKE_SOURCE_DIR}/lib/RmlUi/Backends/RmlUi_Platform_SDL.cpp
)

# On desktop this is the executable. On Android there is no executable at all:
# SDLActivity loads a shared library by name and calls SDL_main inside it, so the
# same sources are linked into libHybridHeaven.so instead. The name matters --
# SDLActivity's getMainSharedObject() looks for lib<name>.so, and the Java side
# names "HybridHeaven".
if(ANDROID)
    add_library(HybridHeavenRecompiled SHARED)
    set_target_properties(HybridHeavenRecompiled PROPERTIES OUTPUT_NAME "HybridHeaven")
else()
    add_executable(HybridHeavenRecompiled)
endif()

target_sources(HybridHeavenRecompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/src/main/main.cpp
    ${HH_RENDER_CONTEXT}
    ${CMAKE_SOURCE_DIR}/rsp/aspMain.cpp
)

# The Android-only translation units: the SDL_main/JNI bootstrap that SDLActivity
# enters through, the crash classifier that stands in for the backtrace
# handler in main.cpp (which has no useful output on a device), and the native
# half of the on-screen touch controls.
if(ANDROID)
    target_sources(HybridHeavenRecompiled PRIVATE
        ${CMAKE_SOURCE_DIR}/src/main/android_glue.cpp
        ${CMAKE_SOURCE_DIR}/src/main/android_diag.cpp
        ${CMAKE_SOURCE_DIR}/src/main/android_touch.cpp
    )
endif()

target_include_directories(HybridHeavenRecompiled PRIVATE ${HH_RECOMP_INCLUDES})

if(HH_RT64)
    target_include_directories(HybridHeavenRecompiled PRIVATE
        ${CMAKE_SOURCE_DIR}/lib/rt64/src
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/inc
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/rhi
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/render
    )
    target_compile_definitions(HybridHeavenRecompiled PRIVATE RT64_SDL_WINDOW_VULKAN HH_RT64)
    target_link_libraries(HybridHeavenRecompiled PRIVATE rt64)
endif()

# The icon Explorer, the taskbar and Alt-Tab show for the .exe FILE. A compiled-in
# Windows resource is the only thing that can supply that one; SDL_SetWindowIcon
# in main.cpp covers the running window and cannot touch it. Both come from
# icons/512.png, via icons/app.ico.
#
# RC is not in the project() language list, so it has to be enabled here. The
# cross-build's toolchain file already resolves CMAKE_RC_COMPILER to llvm-rc,
# which ships with Ubuntu's clang alongside clang-cl and lld-link.
if(WIN32)
    enable_language(RC)
    target_sources(HybridHeavenRecompiled PRIVATE ${CMAKE_SOURCE_DIR}/icons/app.rc)
endif()

if(HH_UI)
    target_sources(HybridHeavenRecompiled PRIVATE ${HH_UI_SOURCES})
    target_compile_definitions(HybridHeavenRecompiled PRIVATE HH_UI)

    target_include_directories(HybridHeavenRecompiled PRIVATE
        ${CMAKE_SOURCE_DIR}/src/ui
        ${CMAKE_SOURCE_DIR}/src/ui/core
        ${CMAKE_SOURCE_DIR}/src/ui/elements
        ${CMAKE_SOURCE_DIR}/src/ui/util
        ${CMAKE_SOURCE_DIR}/lib/RmlUi/Include
        ${CMAKE_SOURCE_DIR}/lib/RmlUi/Backends
        ${CMAKE_SOURCE_DIR}/lib/SlotMap
        ${CMAKE_SOURCE_DIR}/lib/GamepadMotionHelpers
        # The native file dialog the launcher's "Select ROM" opens. It is vendored
        # inside rt64's contrib rather than being a dependency of its own.
        ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/nativefiledialog-extended/src/include
        # The interface shaders are compiled into headers under the build tree and
        # included unqualified, the same way rt64 includes its own.
        ${CMAKE_BINARY_DIR}/shaders
        ${CMAKE_CURRENT_BINARY_DIR}
    )

    # Android has no system FreeType, so the block at the top of this file built
    # it from source and left the Freetype::Freetype target behind. Everywhere
    # else the system one is found. Both spellings end up satisfying RmlUi's font
    # engine; they just arrive by different routes.
    if(NOT ANDROID)
        find_package(Freetype REQUIRED)
        target_include_directories(HybridHeavenRecompiled PRIVATE ${FREETYPE_INCLUDE_DIRS})
    endif()

    target_link_libraries(HybridHeavenRecompiled PRIVATE
        RmlUi::Core
        RmlUi::Debugger
        lunasvg
    )

    if(ANDROID)
        target_link_libraries(HybridHeavenRecompiled PRIVATE Freetype::Freetype)
    else()
        target_link_libraries(HybridHeavenRecompiled PRIVATE ${FREETYPE_LIBRARIES})
    endif()

    # nfd (nativefiledialog-extended) is desktop-only -- it is a GTK/Win32/Cocoa
    # file dialog and does not build for Android at all. There the ROM is chosen
    # by the Java launcher through the Storage Access Framework and handed to the
    # native side, so support.cpp stubs the dialog entry points out.
    if(NOT ANDROID)
        target_link_libraries(HybridHeavenRecompiled PRIVATE nfd)
    endif()

    # The two shaders the UI renderer draws with. rt64 defines these functions and
    # sets FILE_TO_C in its OWN directory scope, so both the DXC command line and
    # FILE_TO_C have to be repeated here -- otherwise the embed step is emitted
    # with an empty ${FILE_TO_C} and tries to execute the .spv itself.
    # Cross-compiling, rt64 does not add its file_to_c subdirectory at all -- the
    # tool would be built for the target and could not run on the build host -- so
    # the CMake target named here does not exist and ninja fails at generate time
    # with "missing and no known rule to make it". Take the host binary rt64 was
    # given instead, which is the same thing it does for its own shaders.
    if(CMAKE_CROSSCOMPILING)
        if(NOT RT64_FILE_TO_C)
            message(FATAL_ERROR
                "Cross-compiling needs -DRT64_FILE_TO_C pointing at a host-built "
                "file_to_c. Build one with:\n"
                "  c++ -std=c++17 -O2 -o <path> lib/rt64/src/tools/file_to_c/file_to_c.cpp")
        endif()
        set(FILE_TO_C "${RT64_FILE_TO_C}")
        set(FILE_TO_C_DEP "${RT64_FILE_TO_C}")
    else()
        set(FILE_TO_C file_to_c)
        set(FILE_TO_C_DEP file_to_c)
    endif()
    set(DXC_COMMON_OPTS "-I${PROJECT_SOURCE_DIR}/src")
    set(DXC_DXIL_OPTS "-Wno-ignored-attributes")
    set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
    set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_0" "-D DYNAMIC_RENDER_PARAMS")
    set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_0" "-D DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
    # Selected by the HOST, not the target: DXC runs during the build and nothing
    # of it ships. A Windows build cross-compiled from WSL still wants dxc-linux.
    # rt64's own CMakeLists makes the same choice the same way -- keep the two in
    # step, since a mismatch here compiles the interface shaders with a different
    # DXC than every other shader in the binary.
    if(CMAKE_HOST_WIN32)
        # dxc.exe carries its own dxcompiler.dll/dxil.dll beside it, so unlike the
        # Linux binaries it needs no library-path prefix.
        set(DXC "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc.exe")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
        set(DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/x64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc-linux")
    else()
        set(DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/arm64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/arm64/dxc-linux")
    endif()

    build_vertex_shader(HybridHeavenRecompiled "shaders/InterfaceVS.hlsl" "shaders/InterfaceVS.hlsl")
    build_pixel_shader(HybridHeavenRecompiled "shaders/InterfacePS.hlsl" "shaders/InterfacePS.hlsl")
endif()

# The RSP vector unit is emulated with SSSE3 intrinsics (rsp_vu_impl.hpp), so
# rsp/aspMain.cpp will not compile at the default baseline -- `_mm_shuffle_epi8`
# fails to inline with "target specific option mismatch". Goemon64Recomp builds
# its whole executable at -march=nehalem for the same reason.
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    target_compile_options(HybridHeavenRecompiled PRIVATE -march=nehalem)
endif()
target_compile_options(HybridHeavenRecompiled PRIVATE -fno-strict-aliasing -fms-extensions)

# -g so a crash backtrace has line information to feed addr2line, and -rdynamic
# so backtrace_symbols_fd can name functions at all (it reads the dynamic symbol
# table). Both are cheap and this port has no debugger available.
#
# Not on Windows: -rdynamic is a GNU-ld option that lld-link rejects outright,
# and the only thing it serves is the crash handler in main.cpp, which is inside
# `#if defined(__linux__)` and does not exist in a Windows build. Debug info
# there comes from the build type (RelWithDebInfo gives a .pdb), not from -g.
if(NOT WIN32)
    target_compile_options(HybridHeavenRecompiled PRIVATE -g)
    # -rdynamic serves exactly one thing: backtrace_symbols_fd() in the crash
    # handler, which needs the dynamic symbol table to name a frame. Bionic has no
    # backtrace() at all, so on Android the flag buys nothing and costs a lot --
    # it would export every symbol in a very large .so. android_diag.cpp reports
    # the fault address instead and it is symbolized off-device with addr2line
    # against the unstripped library.
    if(NOT ANDROID)
        target_link_options(HybridHeavenRecompiled PRIVATE -rdynamic)
    endif()
endif()
# These three reference each other in both directions -- the generated section
# table names functions in RecompiledFuncs, while RecompiledFuncs calls the
# libultra shims in HybridHeavenRuntime -- so a single pass over the archives in
# any order leaves symbols unresolved. RESCAN wraps them in --start-group.
#
# PatchesLib MUST come before RecompiledFuncs. A patched function is emitted
# TWICE -- once by the main recompilation and once by the patch recompilation --
# and the override works by letting the patch's definition win. Both definitions
# are weak (see tools/n64recomp-gcc-weak-recomp-func.patch), so the linker takes
# whichever archive it reaches first. With the order reversed the build still
# succeeds and the patch is silently dropped: that is what kept the overlay
# loader hook from ever firing. Goemon64Recomp lists them in this same order.
if(WIN32)
    # No LINK_GROUP on Windows: RESCAN is defined for GNU-style linkers and CMake
    # rejects it for link.exe/lld-link. Nor is it needed -- the MSVC-style linker
    # already resolves against every library on the command line rather than in a
    # single left-to-right pass, which is the whole of what --start-group buys on
    # ELF.
    #
    # But ORDER IS NOT ENOUGH HERE, and the paragraph above was measured wrong on
    # 2026-07-28: with PatchesLib listed first exactly as below, the definition of
    # `func_8000469C_529C` that survived into the .exe came from
    # RecompiledFuncs/funcs_293.c -- the unpatched one. The port then ran with no
    # patches applied, so `recomp_load_overlays` never fired, no overlay was ever
    # registered, and the first indirect call into one died with
    # `Failed to find function at 0x80107830`. That is the same silent failure the
    # GCC weak patch exists to prevent, reached by a different route.
    #
    # The route is COFF's, and library order does not control it. clang-cl turns
    # RECOMP_FUNC's `weak` into a COMDAT with select-any, and the linker keeps
    # whichever copy it SEES first, not whichever library it reaches first. An
    # archive member is only seen once something pulls it in, and RecompiledFuncs
    # packs ~50 functions per object -- so funcs_293.c.obj gets pulled for any one
    # of its other 49 functions, and by the time PatchesLib is consulted for
    # `func_8000469C_529C` the symbol is already defined and the patch's COMDAT is
    # discarded as the duplicate. Reordering cannot fix that: the pull is demand
    # driven, and the demand comes from somewhere else entirely.
    #
    # /WHOLEARCHIVE is the fix, and it is the MSVC spelling of --whole-archive:
    # every member of PatchesLib joins the link as if it had been named on the
    # command line, which is strictly before any archive is searched. So the
    # patch's definitions are the ones already present when the duplicates show
    # up. Order below is kept anyway, since it costs nothing and is what the ELF
    # link still depends on.
    target_link_libraries(HybridHeavenRecompiled PRIVATE
        HybridHeavenRuntime
        PatchesLib
        RecompiledFuncs
        librecomp
        ultramodern
        SDL2
        Winmm.lib
        Threads::Threads
    )
    target_link_options(HybridHeavenRecompiled PRIVATE
        "/WHOLEARCHIVE:$<TARGET_FILE:PatchesLib>")
elseif(ANDROID)
    # Same archive grouping and the same PatchesLib-before-RecompiledFuncs order
    # as the Linux link below, and for the same reasons -- the NDK links with lld
    # against ELF, so --start-group and weak-symbol override both behave exactly
    # as they do on the desktop Linux build.
    #
    # The grouping is spelled out by hand here, because LINK_GROUP is a CMake 3.24
    # feature and the NDK ships CMake 3.22.1. This is not a weaker substitute: the
    # RESCAN feature expands to precisely --start-group/--end-group on ELF, and the
    # library order inside the group is preserved either way, so the two forms
    # produce the same link line. Doing it this way keeps the build working with
    # the stock SDK toolchain rather than requiring a newer CMake to be installed
    # alongside it.
    #
    # SDL2 differs though: rt64 fetched SDL2 from source rather than finding a
    # package, so the target is plain `SDL2` and there is no `SDL2::SDL2` alias to
    # link. SDL2main is a separate library on Android and is NOT optional -- it
    # carries the JNI entry point that SDLActivity calls, which is what reaches
    # SDL_main in android_glue.cpp. Without it the .so loads and nothing ever
    # runs.
    target_link_libraries(HybridHeavenRecompiled PRIVATE
        -Wl,--start-group
        HybridHeavenRuntime
        PatchesLib
        RecompiledFuncs
        -Wl,--end-group
        librecomp
        ultramodern
        SDL2
        SDL2main
        # The platform libraries RT64, Plume and SDL need: the Vulkan loader, the
        # ANativeWindow/asset APIs, and __android_log_write.
        android
        log
        vulkan
        ${CMAKE_DL_LIBS}
        Threads::Threads
    )

    # The optional user-supplied Vulkan driver, see the HH_CUSTOM_VULKAN_DRIVER
    # block near the top of this file. The define is what makes android_glue.cpp
    # compile its loader at all; without it the driver entry points are the
    # no-op half of the same file and the settings tab hides itself.
    if(HH_CUSTOM_VULKAN_DRIVER)
        target_link_libraries(HybridHeavenRecompiled PRIVATE adrenotools)
        target_compile_definitions(HybridHeavenRecompiled PRIVATE HH_CUSTOM_VULKAN_DRIVER)
    endif()
else()
    target_link_libraries(HybridHeavenRecompiled PRIVATE
        "$<LINK_GROUP:RESCAN,HybridHeavenRuntime,PatchesLib,RecompiledFuncs>"
        librecomp
        ultramodern
        SDL2::SDL2
        ${CMAKE_DL_LIBS}
        Threads::Threads
    )
endif()

# --- Windows: SDL2 headers, the runtime DLLs and the subsystem ----------------
# Kept in one block at the end rather than spread through the file, so what the
# Windows build adds is readable as a unit.
if(WIN32)
    target_include_directories(HybridHeavenRecompiled PRIVATE ${sdl2_SOURCE_DIR}/include)
    target_link_directories(HybridHeavenRecompiled PRIVATE ${sdl2_SOURCE_DIR}/lib/x64)

    # SDL2.dll and the two DXC DLLs have to sit beside the .exe: rt64 loads the
    # shader compiler at runtime, so a build that links cleanly still fails to
    # start without them. recompcontrollerdb.txt travels with them for the same
    # reason assets/ does -- get_program_path() resolves against the .exe on
    # Windows, and a pad with no matching entry falls back to SDL's guess
    # silently.
    add_custom_command(TARGET HybridHeavenRecompiled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            "${CMAKE_SOURCE_DIR}/recompcontrollerdb.txt"
            $<TARGET_FILE_DIR:HybridHeavenRecompiled>)

    # assets/ likewise. It was hand-copied until now, and the failure mode when it
    # is absent is unhelpful in a specific way: RmlUi reports one missing
    # stylesheet and then the gfx thread faults inside init_styling, which reads
    # as a renderer bug. The .exe is also the only build on this machine a real
    # gamepad can reach, so an .exe that cannot start is a controller test that
    # cannot run.
    #
    # An ALL custom target, NOT a POST_BUILD command on HybridHeavenRecompiled.
    # POST_BUILD only runs when the target is RELINKED, so editing only an asset
    # -- a .rml, a stylesheet, a font -- gives "ninja: no work to do" and leaves
    # the deployed copy STALE. The .exe then runs the old markup while the source
    # shows the fix, which is indistinguishable from the fix not working. That
    # cost a debugging round on the V-Sync control's gamepad navigation. An ALL
    # target runs every build; copy_directory is cheap enough to do every time.
    add_custom_target(HybridHeavenAssets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:HybridHeavenRecompiled>/assets"
        COMMENT "Copying assets/ beside the executable")
    add_dependencies(HybridHeavenAssets HybridHeavenRecompiled)

    # A console in Debug, because every diagnostic this port has is a line on
    # stderr and a windowed build throws them away. Release detaches the console
    # but still enters at mainCRTStartup, since main() is the entry point.
    set_target_properties(HybridHeavenRecompiled PROPERTIES
        LINK_FLAGS_DEBUG          "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_RELEASE        "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
        LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
        LINK_FLAGS_MINSIZEREL     "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )

    # Identical COMDAT folding merges functions with identical bodies, which
    # breaks patching: two recompiled functions that happen to compile to the same
    # code become one symbol and a patch aimed at one hits both.
    target_link_options(HybridHeavenRecompiled PRIVATE /OPT:NOICF)
endif()

# The standing checks, as a target instead of a habit.
#
# Both were written after the bug they detect and both were then run only when
# someone remembered to. Neither needs the build, so `cmake --build build
# --target check` is cheap and safe at any point.
#
#   verify_overlay_hook.py   the loader hook's (rom_addr, ram_addr) still match
#                            the recompiler's section table for all 92 overlays.
#                            Run after anything that changes the decomp's
#                            segmentation or regenerates RecompiledFuncs/.
#   recomp_symbol_gap.py     every `<name>_recomp` the recompiled code calls is
#                            actually defined somewhere. The recompiler emits
#                            these undeclared, so the alternative is finding out
#                            one link error at a time.
find_program(HH_UV uv)
if(HH_UV)
    add_custom_target(check
        COMMAND ${HH_UV} run python tools/verify_overlay_hook.py
        COMMAND ${HH_UV} run python tools/recomp_symbol_gap.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running the standing checks"
        VERBATIM
    )
else()
    message(STATUS "uv not found -- the 'check' target will not be available")
endif()
