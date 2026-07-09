#pragma once

// Parser for the .gorz.meta sidecar produced by gorpipe's WRITE command.
// Used to surface column types (S/I/D/L) so the DuckDB extension can emit
// INTEGER/BIGINT/DOUBLE instead of all-VARCHAR.
//
// Format reference: model/src/main/java/org/gorpipe/gor/model/GorMeta.java
// in the gormore tree. Lines look like:
//     ## SCHEMA = S,I,S,S,I,I,D,S
//     ## LINE_COUNT = 1234
//     ## COLUMNS = chrom,pos,ref,alt,ac,an,nhomalt,af
//     ## RANGE = chr1\t1\tchr1\t99999
//     ## MD5 = abcdef...
// All values are text; the file is human-readable.

#include <cstdint>
#include "gorz/Io.h"

#include <optional>
#include <string>
#include <vector>

namespace gorz {

enum class GorColumnType {
    String,   // S
    Integer,  // I  (32-bit)
    Long,     // L  (64-bit)
    Double,   // D
};

struct GorMeta {
    // Per-column schema. Outer optional: missing when .meta is absent or has
    // no SCHEMA line at all. Inner optional: present-but-unknown — a `null`
    // entry in `## SCHEMA = …`. WRITE without -inferschema produces
    // all-null SCHEMAs (one null per column); the extension treats these
    // the same as no entry and falls back to the gor convention per column.
    std::optional<std::vector<std::optional<GorColumnType>>> schema;
    std::optional<int64_t> lineCount;
};

// Parse a .meta file. Returns an empty GorMeta if the path doesn't exist or
// the file is unreadable; the caller treats that as "no type info".
// Format errors inside the file (e.g. unknown SCHEMA letter) throw.
// `opener` routes the open (default: local std::ifstream) — pass a
// FileSystem-backed opener to read the sidecar from an object store.
GorMeta parseMetaFile(const std::string& path, const Opener& opener = defaultOpener());

}  // namespace gorz
