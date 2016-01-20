cmake_minimum_required(VERSION 2.8)
INCLUDE(${CMAKE_SOURCE_DIR}/../build-scripts/CMakeDependencies.txt)

set(EXT_LIBS ${CSI_HTTP_CLIENT_LIBS})

add_subdirectory(csi_http_client)
add_subdirectory(samples)
