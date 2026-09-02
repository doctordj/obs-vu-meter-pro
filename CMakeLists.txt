cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# The source uses C++20 aggregate/designated initialization supported by MSVC 2022.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

# OBS 31.x builds this plugin with a preconfigured C++ standard.
# Explicitly require C++20 on the target because the source_info declaration
# uses C++20 designated initializers.
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /std:c++20)
endif()

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
    find_package(obs-frontend-api REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
    find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
    set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/plugin-main.cpp
    src/vu-meter-source.cpp
    src/vu-meter-source.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})


# Runtime artwork used by the professional hardware-style layouts.
# OBS associates module data with data/obs-plugins/<plugin-name>.
install(DIRECTORY data/
    DESTINATION data/obs-plugins/${_name}
)
