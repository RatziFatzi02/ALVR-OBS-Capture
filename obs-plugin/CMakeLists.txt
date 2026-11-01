cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/bootstrap.cmake" NO_POLICY_SCOPE)

project(alvr-capture VERSION ${PLUGIN_VERSION} LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(libobs REQUIRED)

add_library(${CMAKE_PROJECT_NAME} MODULE)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/plugin-main.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "alvr-capture"
    PREFIX ""
    SUFFIX ".dll"
)

install(TARGETS ${CMAKE_PROJECT_NAME}
    RUNTIME DESTINATION obs-plugins/64bit
    LIBRARY DESTINATION obs-plugins/64bit
)

install(DIRECTORY data/
    DESTINATION data/obs-plugins/alvr-capture
)

# OpenVR is supplied by the existing pinned submodule, alongside the effect.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../openvr/bin/win64/openvr_api.dll"
    DESTINATION data/obs-plugins/alvr-capture)
