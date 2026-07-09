#pragma once

// Columnar GORZ block decoder. Mirrors org.gorpipe.gor.binsearch.BlockPacker
// in the Java tree (decode + lookupMapFromBytes). See docs/design/gorz-format.md
// in the gormore repo for the on-wire layout.
//
// Phase 3 of the DuckDB-reads-GORZ arc covers a focused subset of TYPEIDs —
// the ones that show up in real WRITE -c output against typical GOR pipelines:
//   5  boff   (byte offset)
//   7  incr   (incremental)
//   12 bdiff  (signed byte diff)
//   13 sdiff  (signed short diff)
//   21 vcseq  (variable char sequence)
//   23 tconst (constant text)
//   25 exttlookupdiff (external table lookup with diff)
// All other TYPEIDs throw UnsupportedTypeIdError. The framework is in place
// — adding a new TYPEID is a single switch arm.
//
// Endianness: big-endian for all multi-byte integers, matching Java's
// DataInput convention.

#include "gorz/Reader.h"  // for UnsupportedError + FormatError

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace gorz {

class UnsupportedTypeIdError : public UnsupportedError {
public:
    using UnsupportedError::UnsupportedError;
};

// Per-column external lookup table: refIndex (int) → string bytes.
// Outer map keyed on column index (within the block). Built once per file
// from the header's compressed lookup section; reused across all blocks.
using ExternalTablesMap =
    std::unordered_map<int, std::unordered_map<int, std::string>>;

// Parse the decompressed external-lookup-table bytes (see
// BlockPacker.lookupMapFromBytes in the Java tree). Mutates `out` in place.
//
// Format:
//   COLCNT (uint16 BE)
//   for each column with a lookup:
//       COLIDXDIFF (uint8) — delta from previous column index
//       MAPCNT     (uint16 BE)
//       MAPCNT * { zero-terminated UTF-8 string } — entries 0..MAPCNT-1
void parseExternalLookupTables(const uint8_t* src, std::size_t srcLen,
                               ExternalTablesMap& out);

// Decode one decompressed columnar block into a TSV byte stream
// (rows joined by '\n', columns by '\t', no trailing '\n').
//
// Caller already has the block decompressed (via inflate_zlib/zstd).
// External tables come from the file header.
std::string decodeBlock(const uint8_t* src, std::size_t srcLen,
                        const ExternalTablesMap& externalTables);

}  // namespace gorz
