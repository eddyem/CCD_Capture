cmake_minimum_required(VERSION 3.20)
set(CCDLIB devdummy)

SET(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR})

find_package(PkgConfig REQUIRED)
pkg_check_modules(${CCDLIB} REQUIRED usefull_macros)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} SRC)
include_directories(${${CCDLIB}_INCLUDE_DIRS} ..)
link_directories(${${CCDLIB}_LIBRARY_DIRS})

add_library(${CCDLIB} SHARED ${SRC})
target_link_libraries(${CCDLIB} ${${CCDLIB}_LIBRARIES} -fPIC)
install(TARGETS ${CCDLIB} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
