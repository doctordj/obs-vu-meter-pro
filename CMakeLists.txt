cmake_minimum_required(VERSION 3.28...3.30)

project(obs-vu-meter-pro VERSION 3.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" OPTIONAL)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)

add_library(obs-vu-meter-pro MODULE
    src/plugin-main.cpp
    src/vu-meter-source.cpp
)

target_include_directories(obs-vu-meter-pro PRIVATE
    ${LIBOBS_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(obs-vu-meter-pro PRIVATE
    OBS::libobs
)

target_compile_definitions(obs-vu-meter-pro PRIVATE
    PLUGIN_VERSION="3.2.0"
)

set_target_properties(obs-vu-meter-pro PROPERTIES
    PREFIX ""
    OUTPUT_NAME "obs-vu-meter-pro"
)

install(TARGETS obs-vu-meter-pro
    LIBRARY DESTINATION obs-plugins/64bit
    RUNTIME DESTINATION obs-plugins/64bit
)

install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vu-meter-source.hpp
    DESTINATION include/obs-vu-meter-pro
)
