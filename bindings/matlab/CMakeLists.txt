# MATLAB MEX gateway for OmniCoreSDR (MEX_PLAN.md). Built from the top-level project with
# -DOCH_BUILD_MATLAB=ON, or on its own against an installed OmniCore Host:
#   cmake -S bindings/matlab -B build-matlab -DCMAKE_PREFIX_PATH=/opt/omnicore/0.7.2
# Matlab_ROOT_DIR selects the MATLAB installation when several are present. Without CMake, see
# build_omnicore_mex.m.
cmake_minimum_required(VERSION 3.20)
if (NOT TARGET OmniCore::omnicore)
    project(OmniCoreMatlab LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    find_package(OmniCoreHost 0.7 CONFIG REQUIRED)
    enable_testing()
endif ()

# Helpers shared with the other bindings, and their offline tests (bindings/common).
if (NOT TARGET och_bindings_common)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)
endif ()

# Offline tests of the configuration mapping and the RX gap report: need neither MATLAB nor a device.
foreach (name config_request rx_gaps)
    add_executable(matlab_${name}_test tests/${name}_test.cpp)
    target_include_directories(matlab_${name}_test PRIVATE mex)
    target_link_libraries(matlab_${name}_test PRIVATE och_bindings_common)
    add_test(NAME matlab_${name} COMMAND matlab_${name}_test)
endforeach ()

# R2018a: the interleaved-complex C API, the oldest release supported.
find_package(Matlab 9.4 REQUIRED COMPONENTS MX_LIBRARY)

matlab_add_mex(NAME omnicore_mex
        SRC mex/omnicore_mex.cpp mex/Registry.cpp mex/DeviceSession.cpp mex/RxSession.cpp mex/LibraryLoader.cpp
        R2018a
        # C API only: no MATLAB Engine / Data Array (C++ API) libraries.
        NO_IMPLICIT_LINK_TO_MATLAB_LIBRARIES
        LINK_TO ${Matlab_MEX_LIBRARY} ${Matlab_MX_LIBRARY} OmniCore::omnicore och_bindings_common)
target_compile_features(omnicore_mex PRIVATE cxx_std_17)

if (MSVC)
    # Load omnicore.dll from next to the MEX file (mex/LibraryLoader.cpp).
    target_link_options(omnicore_mex PRIVATE /DELAYLOAD:omnicore.dll)
    target_link_libraries(omnicore_mex delayimp)
elseif (APPLE)
    # @loader_path first: the staged and installed copies sit next to libomnicore.
    set_target_properties(omnicore_mex PROPERTIES BUILD_RPATH "@loader_path;$<TARGET_FILE_DIR:OmniCore::omnicore>"
            INSTALL_RPATH "@loader_path")
elseif (UNIX)
    set_target_properties(omnicore_mex PROPERTIES BUILD_RPATH "$ORIGIN;$<TARGET_FILE_DIR:OmniCore::omnicore>"
            INSTALL_RPATH "$ORIGIN")
endif ()

# Stage a ready-to-use toolbox folder next to the MEX file in the build tree: the +omnicore package
# and the OmniCore Host runtime, so `addpath(<build>/bindings/matlab/toolbox)` works without install.
set(OCH_MATLAB_STAGE ${CMAKE_CURRENT_BINARY_DIR}/toolbox)
add_custom_command(TARGET omnicore_mex POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OCH_MATLAB_STAGE}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:omnicore_mex> ${OCH_MATLAB_STAGE}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/+omnicore ${OCH_MATLAB_STAGE}/+omnicore
        VERBATIM)
if (WIN32)
    add_custom_command(TARGET omnicore_mex POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:OmniCore::omnicore>
            $<TARGET_FILE_DIR:OmniCore::omnicore>/libusbK.dll ${OCH_MATLAB_STAGE}
            VERBATIM)
else ()
    # The MEX file records the SONAME (libomnicore.1.dylib / libomnicore.so.1); stage that name.
    add_custom_command(TARGET omnicore_mex POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_SONAME_FILE:OmniCore::omnicore> ${OCH_MATLAB_STAGE}
            VERBATIM)
endif ()

install(TARGETS omnicore_mex DESTINATION matlab)
install(DIRECTORY +omnicore DESTINATION matlab)
