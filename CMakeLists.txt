cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/plugin-main.cpp
    src/vu-meter-source.cpp
    src/vu-meter-source.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(MSVC)
    target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /W4 /permissive- /std:c++20)
endif()

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    PLUGIN_VERSION="${PROJECT_VERSION}"
)

# OBS plugin locale/data files.
install(DIRECTORY data/
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${_name}"
)
