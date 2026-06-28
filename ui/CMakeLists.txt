# solakon-one-fritz-powerregulator-ui — Qt6 desktop client for the parent
# project's REST API (src/restapi.h/.cpp). It does not talk Modbus/FRITZ!Box
# directly.
#
# This file works in two modes, mirroring solakon-one-ui's own standalone
# CMakeLists.txt:
#
#   - Standalone (`cmake -S ui -B build`, or docker/build.sh in this
#     directory): CMAKE_SOURCE_DIR == CMAKE_CURRENT_SOURCE_DIR, so this file
#     declares its own project()/version and CPack packaging, producing its
#     own RPM/DEB independent of the CLI/service package.
#   - As a subdirectory of the parent project (`-DBUILD_UI=ON` at the repo
#     root): convenient for local development builds of both binaries at
#     once; no separate CPack config is generated in that mode (use the
#     standalone mode, via ui/docker/build.sh, to produce packages).

set(SOLAKON_UI_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SOLAKON_UI_STANDALONE ON)
    cmake_minimum_required(VERSION 3.20)
    project(solakon-one-fritz-powerregulator-ui VERSION 1.0.0 LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Charts)

qt_standard_project_setup()

include(FetchContent)

# QtKeychain — cross-platform bindings to the OS secret store (Secret
# Service/libsecret + KWallet on Linux, Credential Manager on Windows,
# Keychain on macOS). Used exclusively to store/retrieve the REST API key;
# never written to QSettings or any plaintext file.
#
# BUILD_WITH_QT6 is required -- without it, qtkeychain's own CMakeLists.txt
# unconditionally does find_package(Qt5), which fails outright on a
# Qt6-only system.
set(BUILD_WITH_QT6 ON CACHE BOOL "Build qtkeychain against Qt6" FORCE)
set(BUILD_TRANSLATIONS OFF CACHE BOOL "Disable qtkeychain translations" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Link qtkeychain statically" FORCE)
FetchContent_Declare(qt6keychain
    GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain
    GIT_TAG        0.14.3
    # EXCLUDE_FROM_ALL keeps qtkeychain's own install() rules (headers, CMake
    # package config, .pri file, the static lib itself) out of *our*
    # `cmake --install`/CPack output -- we only need it linked in, not
    # installed/packaged as a separate dev artifact.
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(qt6keychain)

qt_add_executable(solakon-one-fritz-powerregulator-ui
    src/main.cpp
    src/mainwindow.cpp
    src/connectiondialog.cpp
    src/statuswidget.cpp
    src/chartwidget.cpp
    src/overridewidget.cpp
    src/apiclient.cpp
    src/secretstore.cpp
)

target_include_directories(solakon-one-fritz-powerregulator-ui PRIVATE
    src
    # qtkeychain's CMakeLists.txt only exposes keychain.h (and the
    # generated qkeychain_export.h) via its $<INSTALL_INTERFACE:...>
    # (i.e. after `make install`); since it's consumed here directly from
    # the FetchContent source/build trees, add both explicitly.
    "${qt6keychain_SOURCE_DIR}"
    "${qt6keychain_BINARY_DIR}"
)

target_link_libraries(solakon-one-fritz-powerregulator-ui PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Charts
    qt6keychain
)

set_target_properties(solakon-one-fritz-powerregulator-ui PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

include(GNUInstallDirs)

install(TARGETS solakon-one-fritz-powerregulator-ui
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES data/solakon-one-fritz-powerregulator-ui.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

# ── Packaging (CPack) — standalone builds only ────────────────────────────────

if(SOLAKON_UI_STANDALONE)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_PACKAGE_NAME             "solakon-one-fritz-powerregulator-ui")
    set(CPACK_PACKAGE_VERSION          "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Qt6 desktop UI for solakon-one-fritz-powerregulator's REST API")
    set(CPACK_PACKAGE_VENDOR           "flunaras")

    # RPM
    set(CPACK_RPM_PACKAGE_RELEASE  "1")
    set(CPACK_RPM_PACKAGE_LICENSE  "GPL-3.0-or-later")
    set(CPACK_RPM_PACKAGE_GROUP    "Productivity/Networking/Other")
    set(CPACK_RPM_PACKAGE_REQUIRES "qt6-base qt6-charts")
    # CPack's RPM generator otherwise defaults the package's %{ARCH} tag to
    # the *build host's* `uname -m` (x86_64) even when cross-compiling --
    # always derive it from the actual target instead, so e.g. an aarch64
    # cross-build produces a genuinely aarch64-tagged (installable) RPM.
    set(CPACK_RPM_PACKAGE_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")

    # DEB
    set(CPACK_DEBIAN_PACKAGE_RELEASE    "1")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "flunaras")
    set(CPACK_DEBIAN_PACKAGE_SECTION    "net")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS    "libqt6widgets6, libqt6network6, libqt6charts6")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7l|armhf)$")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "armhf")
    else()
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    endif()

    include(CPack)
endif()
