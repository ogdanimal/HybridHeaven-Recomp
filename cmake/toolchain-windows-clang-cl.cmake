# Cross-compile the Windows build from WSL, with clang-cl and lld-link.
#
# WHY THIS EXISTS. The Windows build is the port's cheapest route to a real GPU:
# a .exe launched from WSL is an ordinary Windows process, so it gets the
# vendor's own Vulkan driver instead of the software rasteriser this machine is
# otherwise stuck with (there is no NVIDIA Vulkan ICD inside WSL -- the Windows
# driver package does not deliver one -- and Mesa's dzn reaches the GPU through
# D3D12 but is slower than llvmpipe in gameplay).
#
# WHY clang-cl AND NOT cl.exe. recomp.h's MSVC branch defines RECOMP_FUNC without
# weak linkage, which silently breaks the patch override. CMakeLists.txt refuses
# to configure in that case; the reasoning is in the comment there.
#
# WHAT IT NEEDS. Ubuntu's clang package provides clang-cl, lld-link and llvm-rc.
# The headers and import libraries come from the Windows side over /mnt/c, which
# is a case-insensitive mount -- that is what lets <Windows.h> resolve. Both
# paths are cache variables, so a different Visual Studio or SDK version needs a
# -D on the configure line rather than an edit here.
#
#   cmake -S . -B build-win -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-clang-cl.cmake
#
# The MSVC toolset and SDK are found through symlinks by default because the real
# paths contain both spaces and parentheses, which survive CMake but not every
# layer below it.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(HH_WIN_VCTOOLS "$ENV{HOME}/.local/win/vctools" CACHE PATH
    "VC/Tools/MSVC/<version> directory, reachable from Linux")
set(HH_WIN_SDK "$ENV{HOME}/.local/win/sdk" CACHE PATH
    "Windows Kits/10 directory, reachable from Linux")
set(HH_WIN_SDK_VERSION "10.0.26100.0" CACHE STRING
    "Windows SDK version, i.e. the subdirectory name under Include/ and Lib/")

if(NOT EXISTS "${HH_WIN_VCTOOLS}/include")
    message(FATAL_ERROR
        "No MSVC toolset at ${HH_WIN_VCTOOLS}. Point -DHH_WIN_VCTOOLS at a "
        "VC/Tools/MSVC/<version> directory, or create the symlink:\n"
        "  ln -sfn '/mnt/c/Program Files (x86)/Microsoft Visual Studio/<VS>/<edition>/VC/Tools/MSVC/<version>' ~/.local/win/vctools")
endif()
if(NOT EXISTS "${HH_WIN_SDK}/Include/${HH_WIN_SDK_VERSION}")
    message(FATAL_ERROR
        "No Windows SDK ${HH_WIN_SDK_VERSION} at ${HH_WIN_SDK}. Point -DHH_WIN_SDK "
        "at a 'Windows Kits/10' directory and -DHH_WIN_SDK_VERSION at a version "
        "present under its Include/, or create the symlink:\n"
        "  ln -sfn '/mnt/c/Program Files (x86)/Windows Kits/10' ~/.local/win/sdk")
endif()

find_program(HH_CLANG_CL clang-cl REQUIRED)
find_program(HH_LLD_LINK lld-link REQUIRED)
find_program(HH_LLVM_RC llvm-rc REQUIRED)

set(CMAKE_C_COMPILER   "${HH_CLANG_CL}")
set(CMAKE_CXX_COMPILER "${HH_CLANG_CL}")
set(CMAKE_RC_COMPILER  "${HH_LLVM_RC}")
set(CMAKE_LINKER       "${HH_LLD_LINK}")

# clang-cl is a native Linux binary, so CMake would otherwise treat it as hosting
# for Linux. The target triple has to be explicit on every invocation, and the
# driver has to be told where the toolset and SDK headers are.
set(HH_WIN_SYSROOT_FLAGS
    "--target=x86_64-pc-windows-msvc /vctoolsdir ${HH_WIN_VCTOOLS} /winsdkdir ${HH_WIN_SDK} /winsdkversion ${HH_WIN_SDK_VERSION}")

set(CMAKE_C_FLAGS_INIT   "${HH_WIN_SYSROOT_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${HH_WIN_SYSROOT_FLAGS}")

# The LINK step does NOT go through the compiler driver. With an MSVC-style
# frontend CMake invokes the linker itself, so lld-link receives these flags
# verbatim -- give it /libpath: entries rather than the driver's /winsdkdir, or
# it reads "/winsdkversion 10.0.26100.0" as two input files and reports the
# version string as a missing one.
set(HH_WIN_LINK_FLAGS
    "/machine:x64 /libpath:${HH_WIN_VCTOOLS}/lib/x64 /libpath:${HH_WIN_SDK}/Lib/${HH_WIN_SDK_VERSION}/ucrt/x64 /libpath:${HH_WIN_SDK}/Lib/${HH_WIN_SDK_VERSION}/um/x64")

set(CMAKE_EXE_LINKER_FLAGS_INIT    "${HH_WIN_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${HH_WIN_LINK_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${HH_WIN_LINK_FLAGS}")

# Look for headers and libraries in the target sysroot, but let programs resolve
# on the host: the build runs Linux tools (cmake -E, the recompiler) and, through
# WSL's binfmt interop, Windows ones it has just built.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
