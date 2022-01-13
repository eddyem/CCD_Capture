cmake_minimum_required(VERSION 3.20)
set(CCDLIB zwo_module)
set(ZWOLIB ${CCDLIB} PARENT_SCOPE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(${CCDLIB} REQUIRED usefull_macros)

set(CFLAGS -O3 -Wextra -Wall -W -std=gnu99)
set(CMAKE_COLOR_MAKEFILE ON)

if(DEFINED DEBUG AND DEBUG STREQUAL "yes")
    set(CFLAGS ${CFLAGS} -Werror)
    add_definitions(-DEBUG)
    set(CMAKE_VERBOSE_MAKEFILE "ON")
endif()

add_definitions(${CFLAGS})

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} SRC)
add_library(${CCDLIB} ${SRC})
target_link_libraries(${CCDLIB}  ${${CCDLIB}_LIBRARIES} -lASICamera2)
include_directories(${${CCDLIB}_INCLUDE_DIRS} ..)
link_directories(${${CCDLIB}_LIBRARY_DIRS})
