cmake_minimum_required(VERSION 3.20)

include_directories(..)
add_executable(ccd_client ccd_client.c)
target_link_libraries(ccd_client ccdcapture usefull_macros)

add_executable(centroids centroids.c)
target_link_libraries(centroids ccdcapture usefull_macros improc m)
