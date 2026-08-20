include_guard(GLOBAL)

if("${CMAKE_CURRENT_BINARY_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
    message(FATAL_ERROR "In-source builds are not supported.")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" _buildspec)
string(JSON PLUGIN_VERSION GET "${_buildspec}" version)
string(JSON PLUGIN_AUTHOR GET "${_buildspec}" author)
string(JSON PLUGIN_WEBSITE GET "${_buildspec}" website)
string(JSON PLUGIN_EMAIL GET "${_buildspec}" email)

set(CMAKE_EXPORT_PACKAGE_REGISTRY FALSE)
set(CMAKE_INCLUDE_CURRENT_DIR TRUE)
