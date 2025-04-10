cmake_minimum_required(VERSION 3.20)
set(CCDLIB devhikrobot)

SET(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR})
find_package(PkgConfig REQUIRED)
pkg_check_modules(${CCDLIB} REQUIRED mvs>=2.1 usefull_macros)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} SRC)
add_library(${CCDLIB} SHARED ${SRC})

target_include_directories(${CCDLIB} PRIVATE ${${CCDLIB}_INCLUDE_DIRS} ..)
target_link_directories(${CCDLIB} PRIVATE ${${CCDLIB}_LIBRARY_DIRS})

target_link_libraries(${CCDLIB} ${${CCDLIB}_LIBRARIES} -fPIC)
install(TARGETS ${CCDLIB} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
