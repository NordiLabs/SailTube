cmake_minimum_required(VERSION 3.16)
project(SailTuneQuickJs C)

if(NOT IS_DIRECTORY "${QUICKJS_SOURCE_DIR}")
    message(FATAL_ERROR "QUICKJS_SOURCE_DIR must contain the verified upstream sources")
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_LIBC ON CACHE BOOL "" FORCE)
set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${QUICKJS_SOURCE_DIR}" upstream EXCLUDE_FROM_ALL)

# Preserve the upstream CLI, renaming only its entry point. The application
# invokes it in a separate process before creating any GUI or audio objects.
add_library(sailtune_quickjs SHARED
    "${QUICKJS_SOURCE_DIR}/qjs.c"
    "${QUICKJS_SOURCE_DIR}/gen/repl.c"
    "${QUICKJS_SOURCE_DIR}/gen/standalone.c")
target_compile_definitions(sailtune_quickjs PRIVATE
    _GNU_SOURCE QUICKJS_NG_BUILD QJS_BUILD_LIBC main=sailtune_qjs_main)
target_link_libraries(sailtune_quickjs PRIVATE qjs)
set_target_properties(sailtune_quickjs PROPERTIES
    OUTPUT_NAME sailtune-quickjs VERSION 0.18.0 SOVERSION 1
    BUILD_WITH_INSTALL_RPATH OFF INSTALL_RPATH "")
install(TARGETS sailtune_quickjs LIBRARY DESTINATION lib)
