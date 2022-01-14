cmake_minimum_required(VERSION 3.20)
set(CCDLIB fli_module)
set(FLILIB ${CCDLIB} PARENT_SCOPE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(${CCDLIB} REQUIRED fli>=1.71 usefull_macros)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} SRC)
add_library(${CCDLIB} ${SRC})
target_link_libraries(${CCDLIB}  ${${CCDLIB}_LIBRARIES})
include_directories(${${CCDLIB}_INCLUDE_DIRS} ..)
link_directories(${${CCDLIB}_LIBRARY_DIRS})
