cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME gorz)

# The GORZ block codec needs zlib (deflate, read+write) and zstd (read of
# zstd-flagged blocks). Both come from vcpkg (see vcpkg.json) and link
# statically into the extension with hidden visibility, so they don't clash with
# DuckDB's own vendored copies.
find_package(ZLIB REQUIRED)
find_package(zstd CONFIG REQUIRED)

# vcpkg's static triplet (used by the community CI) exports
# zstd::libzstd_static; a shared build (e.g. homebrew) exports zstd::libzstd.
# Resolve whichever exists into one variable used for both the include-order fix
# and linking below.
if(TARGET zstd::libzstd)
  set(GORZ_ZSTD_TARGET zstd::libzstd)
elseif(TARGET zstd::libzstd_static)
  set(GORZ_ZSTD_TARGET zstd::libzstd_static)
else()
  message(
    FATAL_ERROR
      "zstd found but neither zstd::libzstd nor zstd::libzstd_static target exists"
  )
endif()

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

# DuckDB puts its vendored, namespaced third_party/zstd (duckdb_zstd) early on
# the include path, so a bare <zstd.h> binds to DuckDB's copy and lib/gorz's
# global ZSTD_* calls don't resolve. A plain BEFORE prepend of the dep's include
# dir doesn't win when that dir is a system prefix (e.g. /opt/homebrew/include):
# CMake demotes it below the third_party dirs, and a raw -I compile flag lands
# in CXX_FLAGS, which this generator places *after* CXX_INCLUDES. So resolve
# zstd.h / zlib.h to their REAL (non-system) directory — e.g. the homebrew
# Cellar path behind the /opt/homebrew/include symlink — and prepend that with
# BEFORE, which then actually sorts ahead of third_party. In a vcpkg build the
# include dir is already project-local, so this just re-points at the same
# place.
foreach(_h zstd zlib)
  find_path(GORZ_${_h}_INC NAMES ${_h}.h)
  if(GORZ_${_h}_INC)
    get_filename_component(_real "${GORZ_${_h}_INC}/${_h}.h" REALPATH)
    get_filename_component(_realdir "${_real}" DIRECTORY)
    target_include_directories(${EXTENSION_NAME} BEFORE PRIVATE "${_realdir}")
    target_include_directories(${LOADABLE_EXTENSION_NAME} BEFORE
                               PRIVATE "${_realdir}")
  endif()
endforeach()

# Link zlib + zstd into both the static library and the loadable extension.
target_link_libraries(${EXTENSION_NAME} ZLIB::ZLIB ${GORZ_ZSTD_TARGET})
target_link_libraries(${LOADABLE_EXTENSION_NAME} ZLIB::ZLIB ${GORZ_ZSTD_TARGET})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
