# - Try to find libpylonc.so
# Once done this will define
#
#  BASLER_FOUND - system has lib
#  BASLER_INCLUDE_DIR - include directory
#  BASLER_LIBRARIES - Link these to use lib

# Copyright (c) 2021, Edward V. Emelianov <edward.emelianoff@gmail.com>
#
# Redistribution and use is allowed according to the terms of the GPLv2/GPLv3.

include(GNUInstallDirs)

find_path(BASLER_INCLUDE_DIR  pylonc/PylonC.h
    PATHS /usr/pylon/include /opt/pylon/include /usr/local/pylon/include /usr/include /usr/local/include /opt/include /opt/local/include
)
find_path(BASLER_LIBRARY_DIR  libpylonc.so
    PATHS /usr/pylon/lib /opt/pylon/lib /usr/local/pylon/lib /lib /lib64 /usr/lib /usr/lib64 /opt/lib /opt/lib64 /usr/local/lib /usr/local/lib64
)
find_library(BASLER_LIBRARY NAMES pylonc
    PATHS  /opt/pylon/lib /usr/pylon/lib /usr/local/pylon/lib /lib /lib64 /usr/lib /usr/lib64 /opt/lib /opt/lib64 /usr/local/lib /usr/local/lib64
)

find_package_handle_standard_args(BASLER DEFAULT_MSG BASLER_INCLUDE_DIR BASLER_LIBRARY BASLER_LIBRARY_DIR)

if(BASLER_FOUND)
    set(BASLER_INCLUDE_DIRS ${BASLER_INCLUDE_DIR})
    set(BASLER_LIBRARIES ${BASLER_LIBRARY})
    set(BASLER_LIBRARY_DIRS ${BASLER_LIBRARY_DIR})
#    message("BASLER include dir = ${BASLER_INCLUDE_DIRS}")
#    message("BASLER lib = ${BASLER_LIBRARIES}")
#    message("BASLER libdir = ${BASLER_LIBRARY_DIRS}")
    mark_as_advanced(BASLER_INCLUDE_DIRS BASLER_LIBRARIES BASLER_LIBRARY_DIRS)
else()
    message("BASLER not found")
endif(BASLER_FOUND)
