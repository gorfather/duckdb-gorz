#pragma once

// Injectable file-open callback so lib/gorz can read from any storage the
// caller supports without taking a dependency on it. The DuckDB extension
// supplies an opener backed by DuckDB's FileSystem (local FS + object stores
// via httpfs); the CLI and tests use the default std::ifstream opener. This
// keeps lib/gorz dependency-free while letting .meta / .gori / GORD entries
// read from S3/GCS the same way the main .gorz stream already does.

#include <fstream>
#include <functional>
#include <ios>
#include <istream>
#include <memory>
#include <string>

namespace gorz {

// Open `path` for binary reading, returning a seekable istream, or nullptr
// when the path can't be opened (callers treat that as "missing" — an empty
// .meta / .gori is a soft failure that degrades to a sequential scan / the
// gor type convention).
using Opener = std::function<std::unique_ptr<std::istream>(const std::string& path)>;

// Default opener: local filesystem via std::ifstream (binary mode).
inline std::unique_ptr<std::istream> defaultOpen(const std::string& path) {
    auto s = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!*s) return nullptr;
    return s;
}

inline const Opener& defaultOpener() {
    static const Opener op = &defaultOpen;
    return op;
}

}  // namespace gorz
