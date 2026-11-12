cmake_minimum_required(VERSION 3.20)
# The version is written once, in och.h, so headers used without CMake still carry it.
set(OCH_PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/omnicore/och.h")
file(STRINGS "${OCH_PUBLIC_HEADER}" OCH_VERSION_LINE REGEX "^#define OCH_VERSION \"[0-9]+\\.[0-9]+\\.[0-9]+\"$")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" OCH_VERSION "${OCH_VERSION_LINE}")
if (NOT OCH_VERSION)
    message(FATAL_ERROR "Cannot read OCH_VERSION from ${OCH_PUBLIC_HEADER}")
endif ()

project(OmniCoreHost VERSION ${OCH_VERSION} LANGUAGES C CXX)
# Re-run configure when the version in och.h changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${OCH_PUBLIC_HEADER}")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif ()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(OCH_TOP_LEVEL ON)
else ()
    set(OCH_TOP_LEVEL OFF)
endif ()
option(OCH_BUILD_EXAMPLES "Build the programs in examples/" ${OCH_TOP_LEVEL})
option(OCH_BUILD_SOAPY "Build the SoapySDR module in bindings/soapy (needs SoapySDR 0.8+)" OFF)

add_library(och_dsp STATIC src/dsp/codec.cpp src/protocol/frame.cpp)
target_include_directories(och_dsp PUBLIC src)
set_target_properties(och_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if (WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "OmniCore Host currently supports Windows x64 only")
    endif ()

    # Implementation (core + transport) as a static library: the shared library adds the C entry
    # points, and offline tests link it directly because the shared library exports only the C API.
    add_library(och_core STATIC src/core/device.cpp src/core/control.cpp src/core/configuration.cpp src/core/reconfigure.cpp src/core/stream.cpp src/core/stream_io.cpp src/core/rf.cpp src/core/power.cpp src/core/board.cpp src/core/firmware.cpp src/core/timebase.cpp src/core/sync.cpp)
    # One transport backend per platform; core sees only src/transport/link.hpp.
    if (WIN32)
        target_sources(och_core PRIVATE src/transport/win/libusbk_link.cpp)
    else ()
        target_sources(och_core PRIVATE src/transport/posix/libusb_link.cpp)
    endif ()
    # OCH_BUILD: och.h declares the C API as defined in this build, not imported from a DLL.
    target_compile_definitions(och_core PUBLIC OCH_BUILD)
    target_include_directories(och_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(och_core PUBLIC och_dsp Threads::Threads)
    set_target_properties(och_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

    add_library(omnicore SHARED src/capi/device.cpp src/capi/stream.cpp src/capi/rf.cpp src/capi/board.cpp src/capi/firmware.cpp src/capi/timebase.cpp src/capi/sync.cpp)
    target_include_directories(omnicore PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
    target_link_libraries(omnicore PRIVATE och_core)
    set_target_properties(omnicore PROPERTIES VERSION ${PROJECT_VERSION})
    # Same name as the installed package target, so examples build either way.
    add_library(OmniCore::omnicore ALIAS omnicore)

    if (WIN32)
        target_compile_definitions(och_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(och_core PUBLIC setupapi ole32 advapi32)
        set_target_properties(omnicore PROPERTIES PREFIX "")
        set(OCH_LIBUSBK_DLL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/driver/libusbK-3.1.0.0/libusbK-3.1.0.0-bin/bin/dll/amd64/libusbK.dll")
        if (EXISTS "${OCH_LIBUSBK_DLL}")
            add_custom_command(TARGET omnicore POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${OCH_LIBUSBK_DLL}" "$<TARGET_FILE_DIR:omnicore>/libusbK.dll")
            install(FILES "${OCH_LIBUSBK_DLL}" DESTINATION bin)
        else ()
            message(FATAL_ERROR "Missing bundled libusbK runtime: ${OCH_LIBUSBK_DLL}")
        endif ()
    else ()
        if (APPLE)
            # IDEs often start without Homebrew on PATH; look for pkg-config and libusb there.
            list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /usr/local)
        endif ()
        if (APPLE)
            # Idle-sleep assertion while streaming (src/transport/posix/libusb_link.cpp).
            target_link_libraries(och_core PUBLIC "-framework IOKit" "-framework CoreFoundation")
            # libc++ gives some of its templates default visibility; export the C API only.
            target_link_options(omnicore PRIVATE "LINKER:-exported_symbol,_och_*")
        endif ()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0>=1.0.21)
        target_link_libraries(och_core PUBLIC PkgConfig::LIBUSB)
        set_target_properties(omnicore och_core och_dsp PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
        set_target_properties(omnicore PROPERTIES SOVERSION 1)
    endif ()

    if (MINGW)
        target_link_options(omnicore PRIVATE -static -static-libgcc -static-libstdc++)
    endif ()
    add_executable(och apps/och/main.cpp)
    target_link_libraries(och PRIVATE omnicore Threads::Threads)

    if (WIN32)
        target_link_libraries(och PRIVATE shell32)
    elseif (APPLE)
        set_target_properties(och PROPERTIES INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
    else ()
        set_target_properties(och PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
        install(FILES packaging/linux/60-omnicore.rules DESTINATION ${CMAKE_INSTALL_DATADIR}/omnicore)
    endif ()

    if (MINGW)
        target_link_options(och PRIVATE -static)
    endif ()

    if (OCH_BUILD_EXAMPLES)
        add_subdirectory(examples)
    endif ()

    if (OCH_TOP_LEVEL)
        enable_testing()
        add_test(NAME core_is_portable COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckCorePortable.cmake)
        # Runs whether or not the bindings are built: it only reads bindings/.
        add_test(NAME bindings_use_public_api COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckBindingsPublicApi.cmake)
        # Helpers shared by the bindings; their offline tests need neither SoapySDR nor MATLAB.
        add_subdirectory(bindings/common)

        # Offline tests: no device needed.
        add_executable(frame_parser_test tests/frame_parser_test.cpp)
        target_link_libraries(frame_parser_test PRIVATE och_dsp)
        add_test(NAME frame_parser COMMAND frame_parser_test)

        add_library(och_fake STATIC src/transport/fake/fake_link.cpp)
        target_link_libraries(och_fake PUBLIC och_core)
        add_executable(fake_link_test tests/fake_link_test.cpp)
        target_link_libraries(fake_link_test PRIVATE och_fake)
        add_test(NAME fake_link COMMAND fake_link_test)
        set_tests_properties(fake_link PROPERTIES TIMEOUT 120)

        # multi_sync on a model of the sync wiring; slower, because multi_sync sleeps between polls.
        add_executable(sync_test tests/sync_test.cpp)
        target_link_libraries(sync_test PRIVATE och_fake)
        add_test(NAME sync COMMAND sync_test)
        set_tests_properties(sync PROPERTIES TIMEOUT 300)
    endif ()

    if (OCH_BUILD_SOAPY)
        add_subdirectory(bindings/soapy)
    endif ()

    # Install the library and headers for use by other projects.
    install(TARGETS omnicore och EXPORT OmniCoreHostTargets RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT OmniCoreHostTargets NAMESPACE OmniCore:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OmniCoreHost)
    configure_package_config_file(cmake/OmniCoreHostConfig.cmake.in
            ${CMAKE_CURRENT_BINARY_DIR}/OmniCoreHostConfig.cmake INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OmniCoreHost)
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/OmniCoreHostConfigVersion.cmake
            VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/OmniCoreHostConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/OmniCoreHostConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OmniCoreHost)
    install(DIRECTORY include/ DESTINATION include)
    install(DIRECTORY docs/ DESTINATION docs PATTERN "src" EXCLUDE)
    install(DIRECTORY examples/ DESTINATION examples)
    install(FILES README.md LICENSE DESTINATION .)
endif ()
