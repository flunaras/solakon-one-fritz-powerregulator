# ui/cmake/toolchain-aarch64.cmake
#
# CMake cross-compilation toolchain for aarch64-suse-linux, used to build the
# Qt6 desktop UI for aarch64 (e.g. a Raspberry Pi or other ARM64 desktop)
# without needing an aarch64 host or QEMU.
#
# Used by ui/docker/Dockerfile.tumbleweed-aarch64 together with the
# cross-aarch64-gcc14 package from the standard Tumbleweed OSS repo -- same
# cross-compiler as the parent project's own cmake/toolchain-aarch64.cmake --
# plus a much larger aarch64 sysroot (Qt6 Core/Gui/Widgets/Network/Charts/
# DBus/OpenGL, libsecret, glib2, and their transitive dependencies) assembled
# via `zypper --download-only` + manual RPM extraction (see the Dockerfile
# for details), extracted on top of the same minimal glibc sysroot the
# cross-compiler package ships.
#
# Pass to cmake:
#   cmake -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-aarch64.cmake ...

# ── Target system description ─────────────────────────────────────────────────
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ── Cross-compiler (installed by cross-aarch64-gcc14 package) ─────────────────
set(CMAKE_C_COMPILER   /usr/bin/aarch64-suse-linux-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/aarch64-suse-linux-g++)
set(CMAKE_AR           /usr/bin/aarch64-suse-linux-ar)
set(CMAKE_RANLIB       /usr/bin/aarch64-suse-linux-ranlib)
set(CMAKE_STRIP        /usr/bin/aarch64-suse-linux-strip)

# ── Sysroot ───────────────────────────────────────────────────────────────────
# cross-aarch64-gcc14 ships glibc crt objects + stubs at:
#   /usr/aarch64-suse-linux/sys-root/
# The Dockerfile extracts the aarch64 Qt6/libsecret/glib2 package tree (and
# their transitive dependencies, resolved by `zypper --download-only`) into
# that same sysroot, providing headers, .so files, pkg-config files, and
# CMake package config files under it.
set(CMAKE_SYSROOT /usr/aarch64-suse-linux/sys-root)

# Tell the linker where to look for aarch64 shared libraries.
# --allow-shlib-undefined: suppress "undefined reference in .so" errors for
# transitive deps (X11/Wayland/ICU/fontconfig/...) not present in the
# sysroot; they are available at runtime on the target system, and are never
# referenced directly by our own code -- only indirectly via Qt/libsecret,
# whose own .so files we do provide.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-Wl,-rpath-link,${CMAKE_SYSROOT}/usr/lib64 -Wl,--allow-shlib-undefined")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "-Wl,-rpath-link,${CMAKE_SYSROOT}/usr/lib64 -Wl,--allow-shlib-undefined")

# ── Find-root-path settings ───────────────────────────────────────────────────
# Search the sysroot for headers, libraries, and packages.
# Use the host for executables (cmake, ninja, git, ...) -- see QT_HOST_PATH
# below for the one crucial exception (Qt's own moc/uic/rcc code generators).
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ── Cross-compilation: try-compile workaround ────────────────────────────────
# Prevents CMake from trying to link an aarch64 executable on an x86_64 host
# (no QEMU required). Feature checks compile to a static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ── Qt6 host tools ────────────────────────────────────────────────────────────
# Qt6's CMake build system (moc/uic/rcc, qt_add_executable, ...) needs to run
# code generators as part of the build -- those binaries must be the HOST's
# own (x86_64) Qt6 tools, not the aarch64 target libraries in the sysroot.
# QT_HOST_PATH points Qt6's CMake config at a native Qt6 installation to
# source those tools from, while Qt6::Core/Widgets/... etc. are still linked
# from the aarch64 sysroot. The Dockerfile installs a native Qt6 alongside
# the cross-compiler specifically to provide this.
set(QT_HOST_PATH "/usr" CACHE PATH "Native Qt6 install providing moc/uic/rcc" FORCE)

# ── pkg-config ─────────────────────────────────────────────────────────────────
# qtkeychain's libsecret detection goes through pkg_check_modules(libsecret-1),
# which must resolve against the sysroot's .pc files, not the host's.
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib64/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
