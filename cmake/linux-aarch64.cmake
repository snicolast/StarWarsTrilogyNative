# Cross-compiling for Linux arm64 from another Linux with clang (build_linux.sh
# --arch aarch64). The arm64 libraries come from the distribution's multiarch
# packages (Debian, Ubuntu: dpkg --add-architecture arm64, then the :arm64 -dev
# packages, see docs/LINUX.md). The tools the build runs on the way (glslang,
# m68kmake) and the tests go through qemu-aarch64.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(triple aarch64-linux-gnu)
set(CMAKE_C_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET ${triple})
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_COMPILER_TARGET ${triple})
set(CMAKE_EXE_LINKER_FLAGS_INIT -fuse-ld=lld)
set(CMAKE_SHARED_LINKER_FLAGS_INIT -fuse-ld=lld)
set(CMAKE_LIBRARY_ARCHITECTURE ${triple})
# pkg-config (SDL3's X11, Wayland, sound...): the arm64 .pc files only
set(ENV{PKG_CONFIG_LIBDIR} /usr/lib/${triple}/pkgconfig:/usr/share/pkgconfig)
find_program(QEMU_AARCH64 NAMES qemu-aarch64-static qemu-aarch64)
if(QEMU_AARCH64)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${QEMU_AARCH64})
else()
    message(FATAL_ERROR "qemu-aarch64 not found: the build runs glslang and m68kmake for arm64 (install qemu-user)")
endif()
