cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME gorz)

# zlib (deflate, read+write) comes from vcpkg (see vcpkg.json). zstd (read of
# zstd-flagged blocks) is NOT a separate dependency: inside the extension build
# <zstd.h> resolves to DuckDB's own vendored, namespaced zstd (duckdb_zstd),
# which sits first on the include path and can't be reliably overridden. We
# reuse it via the GORZ_DUCKDB_ZSTD define below (as DuckDB's parquet extension
# does), sidestepping the header-shadowing / include-order fight entirely.
find_package(ZLIB REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
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

# lib/gorz uses C++17 (std::optional, std::make_unique). DuckDB's extension
# build injects -std=c++11 into the global flags (its own parquet extension
# builds c++11 too), which overrides CMAKE_CXX_STANDARD. Append -std=c++17 to
# our targets specifically — it lands after the c++11 in the compile line, and
# clang honours the last -std. Only our TUs are affected; the DuckDB ABI
# boundary uses DuckDB's own types, so the mixed standard is safe.
target_compile_features(${EXTENSION_NAME} PUBLIC cxx_std_17)
target_compile_features(${LOADABLE_EXTENSION_NAME} PUBLIC cxx_std_17)
target_compile_options(${EXTENSION_NAME} PRIVATE -std=c++17)
target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE -std=c++17)

# Reuse DuckDB's bundled, namespaced zstd (duckdb_zstd) for reading zstd-flagged
# blocks — <zstd.h> in the extension build already resolves to it, so
# GORZ_DUCKDB_ZSTD makes Inflate.cpp pull duckdb_zstd::* into scope instead of
# depending on / shadow-fighting a separate zstd.
target_compile_definitions(${EXTENSION_NAME} PRIVATE GORZ_DUCKDB_ZSTD)
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE GORZ_DUCKDB_ZSTD)

# Link zlib into both the static library and the loadable extension. (zstd is
# provided by DuckDB itself — see GORZ_DUCKDB_ZSTD above.)
target_link_libraries(${EXTENSION_NAME} ZLIB::ZLIB)
target_link_libraries(${LOADABLE_EXTENSION_NAME} ZLIB::ZLIB)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
