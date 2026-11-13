# SoapySDR module for OmniCoreSDR (driver=omnicore). Built from the top-level project with
# -DOCH_BUILD_SOAPY=ON, or on its own against an installed OmniCore Host:
#   cmake -S bindings/soapy -B build-soapy -DCMAKE_PREFIX_PATH=/opt/omnicore/0.8.0
cmake_minimum_required(VERSION 3.20)
if (NOT TARGET OmniCore::omnicore)
    project(SoapyOmniCore LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    find_package(OmniCoreHost 0.7 CONFIG REQUIRED)
    enable_testing()
endif ()

find_package(SoapySDR 0.8 CONFIG REQUIRED)

# Helpers shared with the other bindings, and their offline tests (bindings/common).
if (NOT TARGET och_bindings_common)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif ()

SOAPY_SDR_MODULE_UTIL(
        TARGET omnicoreSupport
        SOURCES Registration.cpp Device.cpp Streaming.cpp Events.cpp Time.cpp Sensors.cpp
        LIBRARIES OmniCore::omnicore och_bindings_common)

# The module installs to <prefix>/lib/SoapySDR/modules<ABI>/; find libomnicore two levels up.
# If SoapySDR lives under another prefix, point SOAPY_SDR_PLUGIN_PATH at the module directory.
if (APPLE)
    set_target_properties(omnicoreSupport PROPERTIES INSTALL_RPATH "@loader_path/../..")
elseif (UNIX)
    set_target_properties(omnicoreSupport PROPERTIES INSTALL_RPATH "$ORIGIN/../..")
endif ()
