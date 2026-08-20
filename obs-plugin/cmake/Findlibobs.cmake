include_guard(GLOBAL)

if(NOT OBS_SOURCE_DIR)
    message(FATAL_ERROR "OBS_SOURCE_DIR must point to the matching OBS Studio source tree.")
endif()

if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs.h")
    message(FATAL_ERROR "OBS_SOURCE_DIR does not contain libobs/obs.h: ${OBS_SOURCE_DIR}")
endif()

if(NOT OBS_IMPORT_LIB OR NOT EXISTS "${OBS_IMPORT_LIB}")
    message(FATAL_ERROR "OBS_IMPORT_LIB must point to an import library generated from the local obs.dll.")
endif()

add_library(OBS::libobs SHARED IMPORTED GLOBAL)
set_target_properties(OBS::libobs PROPERTIES
    IMPORTED_IMPLIB "${OBS_IMPORT_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/libobs;${OBS_SOURCE_DIR}"
)

set(libobs_FOUND TRUE)
