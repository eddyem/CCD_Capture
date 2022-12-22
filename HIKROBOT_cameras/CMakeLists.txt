cmake_minimum_required(VERSION 3.20)
set(CCDLIB devhikrobot)

find_package(PkgConfig REQUIRED)
pkg_check_modules(${CCDLIB} REQUIRED mvs>=2.1 usefull_macros)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} SRC)
include_directories(${${CCDLIB}_INCLUDE_DIRS} ..)
link_directories(${${CCDLIB}_LIBRARY_DIRS})

add_library(${CCDLIB} SHARED ${SRC})
target_link_libraries(${CCDLIB} ${${CCDLIB}_LIBRARIES} -fPIC)
install(TARGETS ${CCDLIB} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
