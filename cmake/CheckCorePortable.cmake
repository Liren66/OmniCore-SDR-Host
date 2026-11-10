# Run as: cmake -DSOURCE_DIR=<repo> -P CheckCorePortable.cmake
# src/core and src/capi reach the device only through src/transport/link.hpp. This fails if their
# code names a platform, a platform or USB library type, a backend header or a USB endpoint number.
# Comments and string literals are not checked.
set(forbidden
        "_WIN32|__linux__|__APPLE__|OVERLAPPED|HANDLE|GetLastError|libusb|windows\\.h|transport/(win|posix|fake)/|0x0[123][^0-9a-fA-F]|0x8[123][^0-9a-fA-F]")
file(GLOB sources "${SOURCE_DIR}/src/core/*" "${SOURCE_DIR}/src/capi/*")
set(found "")
foreach (source IN LISTS sources)
    file(STRINGS "${source}" lines)
    foreach (line IN LISTS lines)
        string(REGEX REPLACE "\"[^\"]*\"" "\"\"" code "${line}")
        string(REGEX REPLACE "//.*$" "" code "${code}")
        if ("${code} " MATCHES "${forbidden}")
            file(RELATIVE_PATH name "${SOURCE_DIR}" "${source}")
            string(APPEND found "  ${name}: ${line}\n")
        endif ()
    endforeach ()
endforeach ()
if (found)
    message(FATAL_ERROR "Platform-specific code outside src/transport:\n${found}")
endif ()
