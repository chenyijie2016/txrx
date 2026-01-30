#
# Copyright 2014-2015 Ettus Research LLC
# Copyright 2018 Ettus Research, a National Instruments Company
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

cmake_minimum_required(VERSION 3.12)
project(txrx_samples_from_to_file CXX)

### Configure Compiler ########################################################
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" AND ${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
    set(CMAKE_EXE_LINKER_FLAGS "-lthr ${CMAKE_EXE_LINKER_FLAGS}")
    set(CMAKE_CXX_FLAGS "-stdlib=libc++ ${CMAKE_CXX_FLAGS}")
endif ()

### Set up build environment ##################################################
# Choose a static or shared-library build (shared is default, and static will
# probably need some special care!)
# Set this to ON in order to link a static build of UHD:
option(UHD_USE_STATIC_LIBS OFF)

# To add UHD as a dependency to this project, add a line such as this:
find_package(UHD 4.9.0 REQUIRED)
# The version in ^^^^^  here is a minimum version.
# To specify an exact version:
#find_package(UHD  REQUIRED)

# We copy the same mininum versions that UHD has. This might not be the right
# choice for your application.
include(UHDMinDepVersions)

# This example also requires Boost.
# Set components here, then include UHDBoost to do the actual finding
set(UHD_BOOST_REQUIRED_COMPONENTS
        program_options
)
include(UHDBoost)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)

find_package(Protobuf REQUIRED)
set(PROTO_SRC usrp_protocol.proto)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_SRC})


# Find Boost Interprocess library
find_package(Boost REQUIRED COMPONENTS thread)

### Make the executable #######################################################
add_executable(txrx_sync txrx_sync.cpp utils.cpp usrp_transceiver.cpp)
add_executable(txrx_server usrp_transceiver.cpp server.cpp ${PROTO_SRCS})

target_include_directories(txrx_server
        PRIVATE
        ${Boost_INCLUDE_DIRS}
        ${ZMQ_INCLUDE_DIRS}
        ${UHD_INCLUDE_DIRS}
        ${Protobuf_INCLUDE_DIRS}
        ${CMAKE_CURRENT_BINARY_DIR}
)

set(CMAKE_BUILD_TYPE "Release")
message(STATUS "******************************************************************************")
message(STATUS "* NOTE: When building your own app, you probably need all kinds of different  ")
message(STATUS "* compiler flags. This is just an example, so it's unlikely these settings    ")
message(STATUS "* exactly match what you require. Make sure to double-check compiler and     ")
message(STATUS "* linker flags to make sure your specific requirements are included.          ")
message(STATUS "******************************************************************************")

# Shared library case: All we need to do is link against the library, and
# anything else we need (in this case, some Boost libraries):
if (NOT UHD_USE_STATIC_LIBS)
    message(STATUS "Linking against shared UHD library.")

    target_link_directories(txrx_sync   PRIVATE ${UHD_LIBRARY_DIRS})
    target_link_directories(txrx_server PRIVATE ${UHD_LIBRARY_DIRS} ${ZMQ_LIBRARY_DIRS})

    target_link_libraries(txrx_sync
            PRIVATE
            ${UHD_LIBRARIES}
            Boost::program_options
    )

    target_link_libraries(txrx_server
            PRIVATE
            ${UHD_LIBRARIES}
            Boost::program_options
            ${Protobuf_LIBRARIES}
            ${ZMQ_LIBRARIES}
            ${Boost_LIBRARIES}
    )
    # Shared library case: All we need to do is link against the library, and
    # anything else we need (in this case, some Boost libraries):
else (NOT UHD_USE_STATIC_LIBS)
    message(STATUS "Linking against static UHD library.")
    target_link_libraries(txrx_sync
            # We could use ${UHD_LIBRARIES}, but linking requires some extra flags,
            # so we use this convenience variable provided to us
            ${UHD_STATIC_LIB_LINK_FLAG}
            # Also, when linking statically, we need to pull in all the deps for
            # UHD as well, because the dependencies don't get resolved automatically
            ${UHD_STATIC_LIB_DEPS}
    )
    target_include_directories(txrx_sync PUBLIC ${UHD_INCLUDE_DIRS})
endif (NOT UHD_USE_STATIC_LIBS)

### Once it's built... ########################################################
# Here, you would have commands to install your program.
# We will skip these in this example.