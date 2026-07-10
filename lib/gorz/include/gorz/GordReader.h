#pragma once

// Streaming reader over a GORD dictionary. Emits rows in (chrom, pos)
// genomic order, drawn from each entry's .gorz on demand, with lazy file
// admission driven by the dictionary metadata: a candidate entry is only
// opened when its declared (chrom_start, pos_start) is at-or-below the
// next row to be emitted. That keeps the open-file count proportional to
// the overlap density at the current emission position, not to the total
// partition count — and means LIMIT-style queries open zero files past
// the cutoff.
//
// Tie-breaking when two entries have the same (chrom, pos): both rows
// emit, in candidate (dictionary) order. The "gor-contract" only orders
// chrom + pos; secondary columns are stream-defined and downstream code
// uses SORT in GORpipe / ORDER BY in DuckDB if it cares.

#include "gorz/Gord.h"
#include "gorz/Reader.h"

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace gorz {

class GordReader {
public:
    // `opener` routes every per-entry .gorz open (default: local
    // std::ifstream) — the DuckDB extension passes a FileSystem-backed
    // opener so dictionary entries can live in object stores.
    explicit GordReader(const Gord& gord, Opener opener = defaultOpener());
    ~GordReader();

    // Column names. Comes from the GORD's `## COLUMNS = ...` header when
    // present; otherwise the first opened entry's .gorz header.
    const std::vector<std::string>& columns() const { return columns_; }

    // Restrict the merge to entries whose declared range overlaps
    // [chrom, posLo..posHi] (inclusive on both bounds). Entries with no
    // overlap are dropped at the candidate-queue stage and never opened.
    // Within admitted entries, rows outside the range are skipped /
    // terminate the iteration early.
    //
    // Must be called BEFORE the first nextRow(). After that the merge
    // state is locked.
    void setRangeFilter(std::string chrom, int64_t posLo, int64_t posHi);

    // GOR -f / -ff partition filtering. Restrict the merge to entries whose
    // declared tags intersect `tags` (drop the rest at the candidate stage —
    // they're never opened), composing with any range filter. For *bucket*
    // entries (rows for several tags in one file, distinguished by a trailing
    // source column) rows whose source ∉ `tags` are additionally dropped as
    // they're read — GOR's POSSIBLE_TAG row filter. No-op when `tags` is empty.
    // Must be called BEFORE the first nextRow().
    void setTagFilter(std::vector<std::string> tags);

    // Pull the next genomic-ordered row. The returned view is valid until
    // the next nextRow() call (it points into the heap-min iterator's
    // owned row buffer). Returns false at EOF.
    bool nextRow(std::string_view& outLine);

    // The entries that survive bucket selection + range + tag filters, sorted
    // by declared (chromStart, posStart) — i.e. the set this reader would ever
    // open (for a bucketized dictionary this includes the synthetic bucket
    // entries the heuristic chose). A caller can build a "planning" reader, set
    // the filters, and read this to partition the surviving files across threads
    // (Part 3), then hand each group to its own reader. Triggers finalisation;
    // call after the filters are set and before the first nextRow().
    const std::vector<GordEntry>& candidates();

    // The *effective* requested tag set after finalisation: the explicit -f/-ff
    // set, or the dictionary's validTags for a bucketized full scan (empty for
    // an un-bucketized full scan). A caller that partitions candidates() across
    // worker readers must pass this to each so their bucket row filter matches
    // what the planner selected. Triggers finalisation.
    std::vector<std::string> requestedTags();

private:
    // One per opened entry. The Reader is constructed against the
    // ifstream the OpenIterator owns, so both have the same lifetime.
    // `nextRow_` holds the row peeked at the head of this iterator —
    // copied out of the Reader's transient buffer so heap operations
    // don't invalidate it.
    struct OpenIterator {
        std::unique_ptr<std::istream> in;
        std::unique_ptr<Reader> reader;
        std::string nextRow_;
        std::string nextChrom;
        int64_t nextPos = 0;
        std::size_t entryIdx = 0;  // stable tie-break across same (chrom, pos)
        bool exhausted = false;
        bool rowFilter = false;    // bucket entry → filter rows on source col
        // Deleted tags for a selected bucket entry: rows whose source ∈ this set
        // are dropped even if requested (GOR's |D| stale-row handling).
        std::unordered_set<std::string> deletedTags;
        // Source-column normalisation: a primary (single-tag) file has no source
        // column, so append its tag as the trailing field; a bucket already
        // carries it. Keeps every emitted row at the logical N+1 width.
        bool appendSource = false;
        std::string sourceValue;   // the tag to append (primary files only)
    };

    // Lazily apply bucket selection (for bucketized dictionaries), then the
    // tag + range prunes, and sort the candidate list. Idempotent; runs once,
    // triggered by candidates() or the first nextRow().
    void finalize();

    // Open the entry, read its first in-range row into nextRow_, push
    // onto the heap. Drops the iterator (close + discard) if the entry
    // has no in-range row at all.
    void admit(const GordEntry& entry, std::size_t entryIdx);

    // After emitting the heap-top's row, advance its reader. Push back
    // if more in-range rows remain; close + discard otherwise.
    void advanceTop();

    // Compare two keys (chrom, pos). Tie-break on entryIdx so the order
    // is deterministic across runs.
    static int cmpKey(const std::string& aChr, int64_t aPos, std::size_t aIdx,
                      const std::string& bChr, int64_t bPos, std::size_t bIdx);

    // True if the entry's declared range can possibly intersect the
    // active range filter. Conservative: false-positive (open + skip
    // inside) is the price of avoiding under-pruning. An entry with no
    // declared range (2-col dict line) always "overlaps" — it can't be
    // range-pruned.
    bool entryOverlapsRange(const GordEntry& e) const;

    // True if the entry's tags intersect the active tag filter (or no tag
    // filter is set). Used for candidate pruning.
    bool entryTagsMatch(const GordEntry& e) const;

    // True when a per-row source-column filter is needed for this entry: a
    // bucket whose tags aren't all requested (GOR's POSSIBLE_TAG). Rows whose
    // last column ∉ the tag filter are then dropped as they're read.
    bool entryNeedsRowFilter(const GordEntry& e) const;

    // True if (chrom, pos) is strictly past the range filter's upper
    // bound. Caller uses this both to skip rows when the iterator
    // walks past the filter, and to fast-EOF the whole merge.
    bool isPastRange(const std::string& chrom, int64_t pos) const;

    // True iff the entry's declared start (chromStart, posStart) is strictly
    // before the seek target (rangeChrom_, rangePosLo_) — i.e. the declared
    // range straddles the seek position and there are leading rows below the
    // window that an intra-entry seek would skip. When false the entry
    // already begins in-range, so admit() reads it from the top and skips
    // the seek (which would otherwise be pure overhead). Only meaningful
    // when rangeActive_.
    bool declaredStartBeforeSeek(const GordEntry& e) const;

    // Split "chrom\tpos\t..." into (chrom, pos); used to peek the heap
    // key without reparsing the whole row.
    static void parseKey(std::string_view row, std::string& outChrom, int64_t& outPos);

    Gord gord_;
    Opener opener_;  // routes per-entry .gorz opens
    std::vector<std::string> columns_;
    std::vector<GordEntry> candidates_;  // sorted by (chrom_start, pos_start, entryIdx)
    std::size_t candidateCursor_ = 0;

    // Min-heap. Smallest indexed entries go to the front for vector
    // emplace/pop convenience; we treat it as a heap via std::push_heap /
    // std::pop_heap with a greater-than comparator.
    std::vector<std::unique_ptr<OpenIterator>> heap_;

    // Range filter
    bool rangeActive_ = false;
    std::string rangeChrom_;
    int64_t rangePosLo_ = 0;
    int64_t rangePosHi_ = 0;

    // Tag filter (-f / -ff). `pendingTags_` records the explicit request until
    // finalize(); `tagFilter_` is the *effective* requested set used for both
    // candidate pruning and the bucket row filter (the explicit tags, or the
    // dictionary's validTags for a full scan of a bucketized dictionary).
    bool tagFilterRequested_ = false;      // an explicit -f/-ff was given
    std::vector<std::string> pendingTags_;
    bool tagFilterActive_ = false;         // tagFilter_ is populated (post-finalize)
    std::unordered_set<std::string> tagFilter_;

    bool finalized_ = false;
    bool firstCallDone_ = false;

    // The row returned by the most recent nextRow() call. We copy out of
    // the heap-top's nextRow_ into this buffer BEFORE advanceTop() runs,
    // so the caller's string_view stays valid across heap mutations
    // (which can overwrite or drop the heap-top's own buffer).
    std::string outBuf_;
};

}  // namespace gorz
