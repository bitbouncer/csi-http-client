cmake_minimum_required(VERSION 2.8.1)
cmake_policy(SET CMP0015 NEW)

include_directories(${CSI_INCLUDE_PATH} ${CMAKE_SOURCE_DIR})
link_directories(${CSI_LIBRARY_PATH})

add_subdirectory(csi_http_client)
add_subdirectory(samples)
