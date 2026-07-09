#pragma once

// Parser + lookup for the .gorz.gori block-offset index used by gorpipe for
// range pushdown (`gor -p chr7:1000000-2000000`).
//
// Format (see model/.../GorIndexFile.java in the gormore tree):
//   ## fileformat=GORIv2
//   <chrom>\t<pos>\t<offset>
//   <chrom>\t<pos>\t<offset>
//   ...
//
// Each row is the LAST (chrom, pos) of one block and the byte offset AT
// WHICH THE NEXT BLOCK STARTS in the .gorz file. So:
//   * To seek to a block whose first row contains pos P on chrom C:
//     find the smallest index entry (Ek) where (Ek.chrom, Ek.pos) >= (C, P).
//     Ek's block ends at Ek.offset, and starts at E(k-1).offset (or just
//     after the file header for k == 0).

#include "gorz/Io.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gorz {

struct GoriEntry {
    std::string chrom;
    int64_t pos;
    int64_t offset;  // offset AFTER this block (= start of next block)
};

struct GoriIndex {
    std::vector<GoriEntry> entries;  // sorted by (chrom, pos)
};

// Parse <gorz-path>.gori — i.e. the gori sidecar next to the .gorz file.
// Returns an empty index if the .gori file is missing (seek then falls back
// to a sequential scan). Throws on a present-but-malformed file.
// `opener` routes the open (default: local std::ifstream).
GoriIndex parseGoriIndex(const std::string& gorzPath, const Opener& opener = defaultOpener());

// Return the byte offset of the FIRST block whose last-row >= (chrom, pos)
// — i.e. the block that may contain (chrom, pos). The reader should seek
// there and then walk forward, skipping rows whose key is < (chrom, pos).
//
// Special return values:
//   * std::nullopt → index is empty OR (chrom, pos) is past every block.
//     Caller can decide whether to scan from header or skip to EOF.
//   * Anything else → seek-to byte offset relative to start of file.
//
// `headerEndOffset` is the byte offset right after the '\n' that ends the
// header line (where the first block begins).
std::optional<int64_t> findBlockStart(const GoriIndex& idx,
                                      const std::string& chrom, int64_t pos,
                                      int64_t headerEndOffset);

}  // namespace gorz
