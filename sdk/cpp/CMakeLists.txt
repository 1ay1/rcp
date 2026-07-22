cmake_minimum_required(VERSION 3.19)

project(rcp
  VERSION 1.0.0
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
