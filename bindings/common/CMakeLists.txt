# Header-only helpers shared by the bindings (SoapySDR module, MATLAB MEX gateway, Python module):
# channel (de)interleaving, value mappings, RF configuration requests, RX block verdicts and loss
# reports, TX burst bookkeeping and the TX event queue. They need only the
# public header <omnicore/och.h>, never the library itself. Sources include them as "common/X.hpp".
#
# Added by the top-level project, and by each binding when it is built on its own against an
# installed OmniCore Host; the first one to add it defines the target and the tests.
if (TARGET och_bindings_common)
    return()
endif ()

add_library(och_bindings_common INTERFACE)
target_include_directories(och_bindings_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..
        $<TARGET_PROPERTY:OmniCore::omnicore,INTERFACE_INCLUDE_DIRECTORIES>)

# Offline tests of the pure logic: no device, SoapySDR or MATLAB needed.
find_package(Threads REQUIRED)
foreach (name mapping interleave config_request rx_gaps tx_events)
    add_executable(bindings_${name}_test tests/${name}_test.cpp)
    target_link_libraries(bindings_${name}_test PRIVATE och_bindings_common Threads::Threads)
    add_test(NAME bindings_${name} COMMAND bindings_${name}_test)
endforeach ()
