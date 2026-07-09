#pragma once

#include "gorz/GoriIndex.h"
#include "gorz/Io.h"

#include <cstdint>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gorz {

// Header parse result. Holds column names + the format detected at file open
// time. Columnar files in phase 1 carry an empty external-lookup-table
// payload — phase 3 fills this in.
struct GorzHeader {
    std::vector<std::string> columns;
    bool columnarHeader = false;
    std::vector<uint8_t> externalLookupRaw;  // compressed+encoded, phase 3 parses
};

// Streaming row-oriented reader. Phase 1 only handles flag bytes 0x00 (zlib,
// row) and 0x02 (zstd, row). Bodies with flag 0x01 / 0x03 (columnar) throw
// UnsupportedError; phase 3 lifts that restriction.
class UnsupportedError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Reader {
public:
    explicit Reader(std::istream& in);

    // Path-aware variant. `sourcePath` lets the reader find the .gori
    // sidecar (sourcePath + ".gori") for high-level seek(). Pass an empty
    // string to opt out (equivalent to the single-arg constructor).
    // `opener` routes the .gori open (default: local std::ifstream) — the
    // DuckDB extension passes a FileSystem-backed opener so the sidecar is
    // read from the same storage as the main .gorz (incl. object stores).
    Reader(std::istream& in, std::string sourcePath, Opener opener = defaultOpener());

    const GorzHeader& header() const { return header_; }

    // Pull the next row's worth of TSV text. Each call returns at most one
    // line; sequential calls give all rows in the file in their stored order.
    //
    // The returned string_view points into an internal buffer that survives
    // until the next nextRow() call. Caller copies if it needs to keep the
    // line.
    //
    // Returns false at EOF.
    bool nextRow(std::string_view& outLine);

    // Jump to a specific block in the file and drop any cached decoded
    // rows. Used by callers that have a .gori index — see GoriIndex.h's
    // findBlockStart. After seekToBlockStart, the next nextRow() call
    // returns the first row of that block.
    //
    // The underlying istream must be seekable (std::ifstream is fine);
    // calling this on a non-seekable stream raises an exception.
    void seekToBlockStart(int64_t offset);

    // Byte offset right after the header's '\n'. Equivalent to "where
    // block 0 starts". Used as the fallback offset for GoriIndex lookups
    // that match the very first block.
    int64_t headerEndOffset() const { return headerEndOffset_; }

    // High-level seek to the first row in `chrom` whose pos is >= `pos`.
    //
    // **Single-chromosome bounded.** seekTo installs a chrom filter on
    // the reader: subsequent nextRow() calls return rows on `chrom`
    // (skipping any straddling rows on an earlier chrom that happen to
    // share the bracketing block) and return false the moment iteration
    // would cross into a later chrom. Cross-chromosome iteration is
    // meaningless for gor's range model (`gor -p chrX:N-` is the same
    // shape), so the reader handles it rather than asking every caller
    // to remember the contract.
    //
    // Uses the .gori sidecar (when sourcePath was provided and a
    // sidecar exists) to bracket the first block; otherwise falls back
    // to a byte-range binary search over block boundaries. Either way
    // the cost is O(log N) probe reads plus at most one decompressed
    // block's worth of row-skip.
    //
    // Returns true on success (a seek decision was made — either jumped
    // to a block, or positioned at EOF because no chrX row qualifies).
    // No-throw on any sidecar parse failure.
    //
    // Call {@link clearChromFilter} to lift the per-chrom restriction
    // (e.g. to walk the whole file after a seek).
    bool seekTo(const std::string& chrom, int64_t pos);

    // Lift the chrom filter installed by the most recent seekTo. nextRow
    // will then return rows from any chrom, including later ones, until
    // EOF. No-op if no filter is active.
    void clearChromFilter();

    // True iff a .gori sidecar is present and was parsed successfully.
    // Forces the lazy parse on first call. Useful for tests / metrics.
    bool hasIndex();

    // Restrict this reader to the blocks whose start byte falls in
    // [startOffset, endOffset) — the unit for parallel intra-file reads (a
    // .gorz block is a `\n`-delimited record, so this is the CSV byte-range
    // split). On the first read the reader realigns to the next block boundary
    // at/after startOffset (unless startOffset <= headerEndOffset, i.e. the
    // first partition, which starts at the first block); it then decodes whole
    // blocks and reports EOF once the next block would begin at/after
    // endOffset. A block straddling endOffset is still read in full (its start
    // is < endOffset), so the neighbouring partition must NOT re-read it — the
    // standard "own the block whose start is in your range" rule. Must be set
    // before the first nextRow(); mutually exclusive with seekTo().
    void setByteRange(int64_t startOffset, int64_t endOffset);

    // Byte offset where the first block begins (after the header line). The
    // first partition passes this as its startOffset.
    int64_t bodyStartOffset() const { return headerEndOffset_; }

private:
    void readHeader();
    bool readNextBlock();  // returns false at EOF

    // Lazy-loaded .gori sidecar. Empty optional = not yet attempted;
    // present with empty entries = sidecar missing or malformed.
    void ensureIndexLoaded();

    // Lazy file-size discovery used by binary-search seek. -1 = not yet
    // probed; non-negative = byte count.
    int64_t fileSize();

    std::istream& in_;
    std::string sourcePath_;
    Opener opener_ = defaultOpener();  // routes the .gori sidecar open
    std::optional<GoriIndex> index_;
    int64_t fileSize_ = -1;

    // Single-chrom filter applied to nextRow() — see seekTo().
    bool chromFilterActive_ = false;
    std::string chromFilter_;

    // Byte-range restriction for parallel intra-file reads — see setByteRange.
    bool byteRangeActive_ = false;
    bool byteRangeAligned_ = false;  // realigned to a block boundary yet?
    int64_t byteRangeStart_ = 0;
    int64_t byteRangeEnd_ = 0;

    GorzHeader header_;
    int64_t headerEndOffset_ = 0;

    // Current decompressed block (TSV rows joined by '\n'). Row-oriented
    // blocks land here directly; columnar blocks are run through
    // BlockPacker::decodeBlock first.
    std::vector<uint8_t> blockBytes_;
    std::size_t blockCursor_ = 0;
    std::size_t blockEnd_ = 0;

    // External lookup tables, parsed lazily on the first columnar block.
    // Keyed on column index → (refId → string). Reused across blocks.
    bool externalTablesParsed_ = false;
    std::unordered_map<int, std::unordered_map<int, std::string>>
        externalTables_;
};

}  // namespace gorz
