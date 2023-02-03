# - Try to find libflycapture
# Once done this will define
#
#  FLYCAP_FOUND - system has libflycapture
#  FLYCAP_INCLUDE_DIR - include directory
#  FLYCAP_LIBRARIES - Link these to use libflycapture

# Copyright (c) 2021, Edward V. Emelianov <edward.emelianoff@gmail.com>
#
# Redistribution and use is allowed according to the terms of the GPLv2/GPLv3.

include(GNUInstallDirs)

find_path(FLYCAP_INCLUDE_DIR  FlyCapture2.h
    PATH_SUFFIXES libflycapture flycapture
    PATHS /usr/include /usr/local/include /opt/include /opt/local/include
)
find_path(FLYCAP_LIBRARY_DIR  libflycapture.so
    PATHS /lib /lib64 /usr/lib /usr/lib64 /opt/lib /opt/lib64 /usr/local/lib /usr/local/lib64
)
find_library(FLYCAP_LIBRARY NAMES flycapture
    PATHS /lib /lib64 /usr/lib /usr/lib64 /opt/lib /opt/lib64 /usr/local/lib /usr/local/lib64
)
find_library(FLYCAP_LIBRARYC NAMES flycapture-c
    PATHS /lib /lib64 /usr/lib /usr/lib64 /opt/lib /opt/lib64 /usr/local/lib /usr/local/lib64
)

find_package_handle_standard_args(FLYCAP DEFAULT_MSG FLYCAP_INCLUDE_DIR FLYCAP_LIBRARY FLYCAP_LIBRARYC FLYCAP_LIBRARY_DIR)

if(FLYCAP_FOUND)
    set(FLYCAP_INCLUDE_DIRS ${FLYCAP_INCLUDE_DIR})
    set(FLYCAP_LIBRARIES ${FLYCAP_LIBRARY} ${FLYCAP_LIBRARYC})
    set(FLYCAP_LIBRARY_DIRS ${FLYCAP_LIBRARY_DIR})
#    message("FLYCAP include dir = ${FLYCAP_INCLUDE_DIRS}")
#    message("FLYCAP lib = ${FLYCAP_LIBRARIES}")
#    message("FLYCAP libdir = ${FLYCAP_LIBRARY_DIRS}")
    mark_as_advanced(FLYCAP_INCLUDE_DIRS FLYCAP_LIBRARIES FLYCAP_LIBRARY_DIRS)
else()
    message("FLYCAP not found")
endif(FLYCAP_FOUND)
