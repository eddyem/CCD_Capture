cmake_minimum_required(VERSION 3.20)
set(CCDLIB devflycap)

SET(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR})
find_package(PkgConfig REQUIRED)
find_package(FLYCAP    REQUIRED)
pkg_check_modules(${CCDLIB} REQUIRED usefull_macros)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} SRC)
add_library(${CCDLIB} SHARED ${SRC})

target_include_directories(${CCDLIB} PRIVATE ${${CCDLIB}_INCLUDE_DIRS} ${FLYCAP_INCLUDE_DIRS} ..)
target_link_directories(${CCDLIB} PRIVATE ${${CCDLIB}_LIBRARY_DIRS} ${FLYCAP_LIBRARY_DIRS})

target_link_libraries(${CCDLIB} ${${CCDLIB}_LIBRARIES} ${FLYCAP_LIBRARY} ${FLYCAP_LIBRARYC} -fPIC)
install(TARGETS ${CCDLIB} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
