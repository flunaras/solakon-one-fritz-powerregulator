cmake_minimum_required(VERSION 3.20)
project(solakon-one-fritz-powerregulator VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Options ──────────────────────────────────────────────────────────────────

option(SOLAKON_FRITZ_POWERREGULATOR_STATIC
    "Link OpenSSL and C++ runtime statically; libc stays shared" OFF)

# BUILD_UI: builds the optional Qt6 desktop client (ui/) alongside the
# headless CLI/service binary. Off by default so the CLI build never
# requires Qt to be installed. See ui/CMakeLists.txt for its own
# dependencies (Qt6 Widgets/Network/Charts, QtKeychain).
option(BUILD_UI "Build the Qt6 desktop UI (ui/solakon-one-fritz-powerregulator-ui)" OFF)

# cpp-httplib git tag — override to v0.14.3 for Raspbian bullseye (OpenSSL 1.1.1).
# v0.15.0+ refuses to compile against OpenSSL < 3.0.0 when HTTPLIB_REQUIRE_OPENSSL=ON.
set(SOLAKON_HTTPLIB_TAG "v0.18.0" CACHE STRING "cpp-httplib git tag to fetch")

# ── Dependencies ─────────────────────────────────────────────────────────────

# OpenSSL (system package; required by cpp-httplib TLS support).
if(SOLAKON_FRITZ_POWERREGULATOR_STATIC)
    set(OPENSSL_USE_STATIC_LIBS ON)
endif()
find_package(OpenSSL REQUIRED)

# Modbus TCP is implemented directly over POSIX sockets (no libmodbus dependency).
# Avahi (optional, for mDNS discovery).

# Threads (needed for Logger mutex and sleep).
find_package(Threads REQUIRED)

# Avahi — multicast DNS service discovery (optional but strongly recommended).
# Provides mDNS discovery of Solakon ONE inverters on the local network.
# If not available, mDNS discovery commands will fail gracefully with a helpful message.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(AVAHI_CLIENT QUIET avahi-client)
    pkg_check_modules(AVAHI_COMMON QUIET avahi-common)
endif()

if(AVAHI_CLIENT_FOUND AND AVAHI_COMMON_FOUND)
    set(HAVE_AVAHI ON)
    message(STATUS "Avahi found: mDNS discovery enabled")
else()
    set(HAVE_AVAHI OFF)
    message(STATUS "Avahi not found: mDNS discovery will be unavailable. "
                   "To enable, install: libavahi-client-dev libavahi-common-dev")
endif()

include(FetchContent)

# Do not install files from fetched third-party dependencies into our packages.
set(HTTPLIB_INSTALL OFF CACHE BOOL "Disable httplib install targets" FORCE)
set(CLI11_INSTALL OFF CACHE BOOL "Disable CLI11 install targets" FORCE)
set(NLOHMANN_JSON_INSTALL OFF CACHE BOOL "Disable nlohmann_json install targets" FORCE)
set(JSON_Install OFF CACHE BOOL "Disable nlohmann_json install targets (legacy option name)" FORCE)

# cpp-httplib — header-only HTTP client with OpenSSL TLS.
set(HTTPLIB_REQUIRE_OPENSSL         ON  CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE   OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
    GIT_TAG        ${SOLAKON_HTTPLIB_TAG})

# nlohmann/json — header-only JSON library.
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG        v3.11.3)

# CLI11 — header-only argument parser with INI config file support.
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11
    GIT_TAG        v2.4.2)

FetchContent_MakeAvailable(httplib nlohmann_json CLI11)

# ── Target ───────────────────────────────────────────────────────────────────

add_executable(solakon-one-fritz-powerregulator
    src/main.cpp
    src/logger.cpp
    src/solakonapi.cpp
    src/fritzapi.cpp
    src/mdnsapi.cpp
    src/restapi.cpp
    src/statestore.cpp
)

target_include_directories(solakon-one-fritz-powerregulator PRIVATE
    src
    $<$<BOOL:${HAVE_AVAHI}>:${AVAHI_CLIENT_INCLUDE_DIRS}>
    $<$<BOOL:${HAVE_AVAHI}>:${AVAHI_COMMON_INCLUDE_DIRS}>
)

target_compile_definitions(solakon-one-fritz-powerregulator PRIVATE
    # CPPHTTPLIB_OPENSSL_SUPPORT is already set by httplib's own CMake target
    # when HTTPLIB_REQUIRE_OPENSSL=ON; defining it twice triggers a warning.
    # It is left here as a comment for documentation purposes only.
    $<$<BOOL:${HAVE_AVAHI}>:HAVE_AVAHI>
)

target_link_libraries(solakon-one-fritz-powerregulator PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    $<$<BOOL:${HAVE_AVAHI}>:${AVAHI_CLIENT_LIBRARIES}>
    $<$<BOOL:${HAVE_AVAHI}>:${AVAHI_COMMON_LIBRARIES}>
)

target_link_directories(solakon-one-fritz-powerregulator PRIVATE)

if(CMAKE_CROSSCOMPILING)
    target_link_libraries(solakon-one-fritz-powerregulator PRIVATE pthread)
endif()

if(SOLAKON_FRITZ_POWERREGULATOR_STATIC)
    target_link_options(solakon-one-fritz-powerregulator PRIVATE
        -static-libstdc++
        -static-libgcc
    )
    target_link_libraries(solakon-one-fritz-powerregulator PRIVATE ${CMAKE_DL_LIBS})
endif()

# ── Install ───────────────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(TARGETS solakon-one-fritz-powerregulator
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES data/solakon-one-fritz-powerregulator.service
    DESTINATION lib/systemd/system)

install(FILES data/solakon-one-fritz-powerregulator.conf
    DESTINATION /etc/solakon-one-fritz-powerregulator)

# ── Packaging (CPack) ─────────────────────────────────────────────────────────

set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_PACKAGE_NAME             "solakon-one-fritz-powerregulator")
set(CPACK_PACKAGE_VERSION          "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Adjusts Solakon ONE export power based on FRITZ!Box Smart Home device load")
set(CPACK_PACKAGE_VENDOR           "flunaras")

# RPM
set(CPACK_RPM_PACKAGE_RELEASE  "1")
set(CPACK_RPM_PACKAGE_LICENSE  "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP    "System/Daemons")
set(CPACK_RPM_USER_FILELIST
    "%dir /etc/solakon-one-fritz-powerregulator"
    "%config(noreplace) /etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf")
if(SOLAKON_FRITZ_POWERREGULATOR_STATIC)
    set(CPACK_RPM_PACKAGE_REQUIRES "")
else()
    if(OPENSSL_VERSION VERSION_GREATER_EQUAL "3.0")
        set(CPACK_RPM_PACKAGE_REQUIRES "libopenssl3")
    else()
        set(CPACK_RPM_PACKAGE_REQUIRES "libopenssl1_1")
    endif()
endif()

# DEB
set(CPACK_DEBIAN_PACKAGE_RELEASE    "1")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "flunaras")
set(CPACK_DEBIAN_PACKAGE_SECTION    "net")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7l|armhf)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "armhf")
else()
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
endif()
if(SOLAKON_FRITZ_POWERREGULATOR_STATIC)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "")
else()
    if(OPENSSL_VERSION VERSION_GREATER_EQUAL "3.0")
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libssl3")
    else()
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libssl1.1")
    endif()
endif()

# Treat /etc content as conffiles so dpkg preserves local modifications.
set(_cpack_debian_conffiles "${CMAKE_CURRENT_BINARY_DIR}/conffiles")
file(WRITE "${_cpack_debian_conffiles}"
    "/etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf\n")
list(APPEND CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${_cpack_debian_conffiles}")

include(CPack)

# ── Optional Qt6 desktop UI ──────────────────────────────────────────────────

if(BUILD_UI)
    add_subdirectory(ui)
endif()
