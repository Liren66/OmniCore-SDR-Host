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
foreach (name config_request rx_gaps tx_events)
    add_executable(matlab_${name}_test tests/${name}_test.cpp)
    target_include_directories(matlab_${name}_test PRIVATE mex)
    target_link_libraries(matlab_${name}_test PRIVATE och_bindings_common)
    add_test(NAME matlab_${name} COMMAND matlab_${name}_test)
endforeach ()

# R2018a: the interleaved-complex C API, the oldest release supported.
find_package(Matlab 9.4 REQUIRED COMPONENTS MX_LIBRARY)

matlab_add_mex(NAME omnicore_mex
        SRC mex/omnicore_mex.cpp mex/Registry.cpp mex/DeviceSession.cpp mex/RxSession.cpp mex/TxSession.cpp mex/LibraryLoader.cpp
        R2018a
        # C API only: no MATLAB Engine / Data Array (C++ API) libraries.
        NO_IMPLICIT_LINK_TO_MATLAB_LIBRARIES
        LINK_TO ${Matlab_MEX_LIBRARY} ${Matlab_MX_LIBRARY} OmniCore::omnicore och_bindings_common)
target_compile_features(omnicore_mex PRIVATE cxx_std_17)

# Ctrl+C inside long calls (mex/Interrupt.hpp) uses MATLAB's undocumented utIsInterruptPending.
option(OCH_MATLAB_CTRL_C "Let Ctrl+C stop long MEX calls (undocumented MATLAB libut API)" OFF)
if (OCH_MATLAB_CTRL_C)
    get_filename_component(OCH_MATLAB_LIB_DIR ${Matlab_MX_LIBRARY} DIRECTORY)
    find_library(OCH_MATLAB_UT_LIBRARY NAMES ut libut PATHS ${OCH_MATLAB_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
    target_compile_definitions(omnicore_mex PRIVATE OMNICORE_MEX_CTRL_C)
    target_link_libraries(omnicore_mex ${OCH_MATLAB_UT_LIBRARY})
endif ()

if (MSVC)
    # Load omnicore.dll from next to the MEX file (mex/LibraryLoader.cpp).
    target_link_options(omnicore_mex PRIVATE /DELAYLOAD:omnicore.dll)
    target_link_libraries(omnicore_mex delayimp)
elseif (APPLE)
    # A universal OmniCore Host build (release) still links MATLAB's single-architecture libmx.
    if (Matlab_MEX_EXTENSION STREQUAL "mexmaca64")
        set_target_properties(omnicore_mex PROPERTIES OSX_ARCHITECTURES arm64)
    elseif (Matlab_MEX_EXTENSION STREQUAL "mexmaci64")
        set_target_properties(omnicore_mex PROPERTIES OSX_ARCHITECTURES x86_64)
    endif ()
    # @loader_path first: the staged and installed copies sit next to libomnicore.
    set_target_properties(omnicore_mex PROPERTIES BUILD_RPATH "@loader_path;$<TARGET_FILE_DIR:OmniCore::omnicore>"
            INSTALL_RPATH "@loader_path")
    # FindMatlab exports the C++ MEX API entry points (cppMexFunction.map) from every MEX file and
    # lets them stay undefined (-U); the Xcode 26 linker rejects that. This is a C API MEX file, so
    # export what `mex` exports for one: mexFunction and mexfilerequiredapiversion.
    set_target_properties(omnicore_mex PROPERTIES
            LINK_FLAGS "-Wl,-exported_symbols_list,${Matlab_EXTERN_LIBRARY_DIR}/c_exportsmexfileversion.map")
elseif (UNIX)
    set_target_properties(omnicore_mex PROPERTIES BUILD_RPATH "$ORIGIN;$<TARGET_FILE_DIR:OmniCore::omnicore>"
            INSTALL_RPATH "$ORIGIN")
    # MATLAB loads its own, older libstdc++ first, so a MEX file built with the system GCC fails with
    # "GLIBCXX_3.4.xx not found". Link the C++ runtime statically; FindMatlab's version script keeps
    # its symbols local. libomnicore does the same with OCH_STATIC_LIBSTDCXX.
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(omnicore_mex PRIVATE -static-libgcc -static-libstdc++)
    endif ()
endif ()

# Stage a ready-to-use toolbox folder next to the MEX file in the build tree: the +omnicore package
# and the OmniCore Host runtime, so `addpath(<build>/bindings/matlab/toolbox)` works without install.
set(OCH_MATLAB_STAGE ${CMAKE_CURRENT_BINARY_DIR}/toolbox)
add_custom_command(TARGET omnicore_mex POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OCH_MATLAB_STAGE}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:omnicore_mex> ${OCH_MATLAB_STAGE}
        VERBATIM)
# The .m files change without the MEX file relinking: copy them on every build.
add_custom_target(omnicore_matlab_package ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OCH_MATLAB_STAGE}
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

# Install component "matlab": a self-contained folder <prefix>/matlab with the MEX file, the
# +omnicore package, examples and the OmniCore Host runtime library next to the MEX file.
install(TARGETS omnicore_mex DESTINATION matlab COMPONENT matlab)
install(DIRECTORY +omnicore examples DESTINATION matlab COMPONENT matlab)
install(FILES README.md DESTINATION matlab COMPONENT matlab)
if (WIN32)
    install(FILES $<TARGET_FILE:OmniCore::omnicore> $<TARGET_FILE_DIR:OmniCore::omnicore>/libusbK.dll
            DESTINATION matlab COMPONENT matlab)
else ()
    # Under the SONAME the MEX file records (libomnicore.1.dylib / libomnicore.so.1), as a file:
    # the build tree has it as a symlink.
    install(CODE "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E copy \"$<TARGET_FILE:OmniCore::omnicore>\"
            \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/matlab/$<TARGET_SONAME_FILE_NAME:OmniCore::omnicore>\")"
            COMPONENT matlab)
endif ()

# Release zip: build/omnicore-matlab-<version>-<arch>.zip holding the folder omnicore-matlab/, made
# from the installed component (install rpath only, so it does not lean on the build tree).
if (DEFINED OmniCoreHost_VERSION)
    set(OCH_MATLAB_VERSION ${OmniCoreHost_VERSION})
else ()
    set(OCH_MATLAB_VERSION ${PROJECT_VERSION})
endif ()
string(REGEX REPLACE "^mex" "" OCH_MATLAB_ARCH "${Matlab_MEX_EXTENSION}")
set(OCH_MATLAB_ZIP_STAGE ${CMAKE_CURRENT_BINARY_DIR}/zip)
set(OCH_MATLAB_ZIP ${CMAKE_BINARY_DIR}/omnicore-matlab-${OCH_MATLAB_VERSION}-${OCH_MATLAB_ARCH}.zip)
add_custom_target(omnicore_matlab_zip
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${OCH_MATLAB_ZIP_STAGE} ${OCH_MATLAB_ZIP}
        COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG> --component matlab
        --prefix ${OCH_MATLAB_ZIP_STAGE}
        COMMAND ${CMAKE_COMMAND} -E rename ${OCH_MATLAB_ZIP_STAGE}/matlab ${OCH_MATLAB_ZIP_STAGE}/omnicore-matlab
        COMMAND ${CMAKE_COMMAND} -E chdir ${OCH_MATLAB_ZIP_STAGE}
        ${CMAKE_COMMAND} -E tar cf ${OCH_MATLAB_ZIP} --format=zip omnicore-matlab
        COMMENT "MATLAB package ${OCH_MATLAB_ZIP}"
        VERBATIM)
add_dependencies(omnicore_matlab_zip omnicore_mex)
