cmake_minimum_required(VERSION 3.19)

project(rcp
  VERSION 1.0.1
  DESCRIPTION "Retrieval Context Protocol — header-only C++23 SDK"
  HOMEPAGE_URL "https://rcp-6d6ef6d5.mintlify.site/"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Header-only interface library. Consumers do:
#     find_package(rcp CONFIG REQUIRED)
#     target_link_libraries(app PRIVATE rcp::rcp)
# and get the include path plus the C++23 requirement automatically.
# ---------------------------------------------------------------------------
add_library(rcp INTERFACE)
add_library(rcp::rcp ALIAS rcp)

target_compile_features(rcp INTERFACE cxx_std_23)

target_include_directories(rcp INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

# The transports use TCP sockets: BSD sockets on POSIX (in libc, nothing to
# link) and Winsock2 on Windows (which must be linked explicitly). This is
# INTERFACE because the SDK is header-only — the socket calls are compiled into
# the CONSUMER's translation units, so the consumer is what needs the library.
# mingw/gcc also ignores MSVC's `#pragma comment(lib, "ws2_32")`, so naming it
# here is the only thing that makes a UCRT64 build link.
if(WIN32)
  target_link_libraries(rcp INTERFACE ws2_32)
endif()

# ---------------------------------------------------------------------------
# Optional: build the bundled tests with -DRCP_BUILD_TESTS=ON.
#
# These are built through CMake rather than a hand-written compiler line in CI
# for a specific reason: the tests need exactly what a real consumer needs
# (cxx_std_23 and, on Windows, ws2_32), and linking them against rcp::rcp is
# what guarantees they get it. A bespoke `c++ -std=c++23 ...` in a workflow
# duplicates that knowledge, and the duplicate is what rots — it silently
# omits Winsock on mingw and hard-codes a -std flag Apple Clang spells
# differently.
# ---------------------------------------------------------------------------
option(RCP_BUILD_TESTS "Build the RCP SDK tests" OFF)
if(RCP_BUILD_TESTS)
  enable_testing()
  foreach(t test_types test_transport_platform)
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE rcp::rcp)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Optional: build the bundled examples with -DRCP_BUILD_EXAMPLES=ON.
# ---------------------------------------------------------------------------
option(RCP_BUILD_EXAMPLES "Build the RCP example server/client" OFF)
if(RCP_BUILD_EXAMPLES)
  foreach(ex example_server example_client example_selector example_federation)
    add_executable(${ex} examples/${ex}.cpp)
    target_link_libraries(${ex} PRIVATE rcp::rcp)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Install + export so downstreams (and vcpkg) can find_package(rcp).
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS rcp EXPORT rcpTargets)

install(EXPORT rcpTargets
  FILE rcpTargets.cmake
  NAMESPACE rcp::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rcp)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rcpConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/rcpConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rcp)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/rcpConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/rcpConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/rcpConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rcp)
