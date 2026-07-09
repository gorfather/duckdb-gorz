cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME gorz)

# The GORZ block codec needs zlib (deflate, read+write) and zstd (read of
# zstd-flagged blocks). Both come from vcpkg (see vcpkg.json) and link
# statically into the extension with hidden visibility, so they don't clash
# with DuckDB's own vendored copies.
find_package(ZLIB REQUIRED)
find_package(zstd CONFIG REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# src/include: the extension header. lib/gorz/include: the DuckDB-independent
# GORZ/GORD codec (Reader/Writer/Gord/Base128/...), vendored from gormore.
include_directories(src/include lib/gorz/include)

set(EXTENSION_SOURCES
    src/gorz_extension.cpp
    lib/gorz/src/Base128.cpp
    lib/gorz/src/Inflate.cpp
    lib/gorz/src/BlockPacker.cpp
    lib/gorz/src/MetaFile.cpp
    lib/gorz/src/GoriIndex.cpp
    lib/gorz/src/Reader.cpp
    lib/gorz/src/Gord.cpp
    lib/gorz/src/GordReader.cpp
    lib/gorz/src/Writer.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link zlib + zstd into both the static library and the loadable extension.
target_link_libraries(${EXTENSION_NAME} ZLIB::ZLIB $<IF:$<TARGET_EXISTS:zstd::libzstd>,zstd::libzstd,zstd::libzstd_static>)
target_link_libraries(${LOADABLE_EXTENSION_NAME} ZLIB::ZLIB $<IF:$<TARGET_EXISTS:zstd::libzstd>,zstd::libzstd,zstd::libzstd_static>)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
