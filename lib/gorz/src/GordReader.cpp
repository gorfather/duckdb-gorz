#include "gorz/GordReader.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace gorz {

namespace {

// Sort key for the candidate queue. Order entries by their declared
// (chrom_start, pos_start) so that admission walks them in genomic
// order. Stable secondary key on input index (preserved by stable_sort).
struct CandidateLess {
    bool operator()(const GordEntry& a, const GordEntry& b) const {
        int c = a.chromStart.compare(b.chromStart);
        if (c != 0) return c < 0;
        return a.posStart < b.posStart;
    }
};

// The source/tag column of a bucket row is its last tab-separated field
// (GOR's tagColIdx = header columns - 1). Returns the whole row when there's
// no tab (defensive; such a row can't be a real bucket row).
std::string_view lastField(std::string_view row) {
    auto p = row.rfind('\t');
    return p == std::string_view::npos ? row : row.substr(p + 1);
}

}  // namespace

GordReader::GordReader(const Gord& gord, Opener opener)
    : gord_(gord), opener_(std::move(opener)) {
    // Candidate list is built lazily in finalize() (after the filters are set),
    // because bucket selection depends on the requested tag set.
}

GordReader::~GordReader() = default;

void GordReader::setRangeFilter(std::string chrom, int64_t posLo, int64_t posHi) {
    if (firstCallDone_) {
        throw std::runtime_error("GordReader::setRangeFilter called after nextRow()");
    }
    rangeActive_ = true;
    rangeChrom_ = std::move(chrom);
    rangePosLo_ = posLo;
    rangePosHi_ = posHi;
    // Actual candidate pruning is deferred to finalize().
}

void GordReader::finalize() {
    if (finalized_) return;
    finalized_ = true;

    // Effective requested tag set: the explicit -f/-ff request, or (for a full
    // scan of a bucketized dictionary) all valid tags so the heuristic still
    // opens buckets and deleted rows are dropped.
    std::unordered_set<std::string> requested;
    if (tagFilterRequested_) {
        for (auto& t : pendingTags_) requested.insert(std::move(t));
    } else if (!gord_.buckets.empty()) {
        requested = gord_.validTags;
    }

    if (!gord_.buckets.empty()) {
        // Bucketized: GOR's bucket-vs-primary selection replaces the entry list
        // with the optimal mix of source files + synthetic bucket entries.
        candidates_ = optimizedFileList(gord_, requested);
        tagFilter_ = std::move(requested);   // drives the bucket row filter
        tagFilterActive_ = true;
    } else {
        // Un-bucketized: keep the raw entries, tag-pruning on an explicit filter.
        candidates_ = gord_.entries;
        if (tagFilterRequested_) {
            tagFilter_ = std::move(requested);
            tagFilterActive_ = true;
            candidates_.erase(
                std::remove_if(candidates_.begin(), candidates_.end(),
                               [&](const GordEntry& e) { return !entryTagsMatch(e); }),
                candidates_.end());
        }
    }

    // Range prune, then order by declared (chromStart, posStart) for admission.
    if (rangeActive_) {
        candidates_.erase(
            std::remove_if(candidates_.begin(), candidates_.end(),
                           [&](const GordEntry& e) { return !entryOverlapsRange(e); }),
            candidates_.end());
    }
    std::stable_sort(candidates_.begin(), candidates_.end(), CandidateLess{});
}

const std::vector<GordEntry>& GordReader::candidates() {
    finalize();
    return candidates_;
}

std::vector<std::string> GordReader::requestedTags() {
    finalize();
    return std::vector<std::string>(tagFilter_.begin(), tagFilter_.end());
}

bool GordReader::entryOverlapsRange(const GordEntry& e) const {
    if (!rangeActive_) return true;
    if (!e.hasRange) return true;  // no declared range → can't range-prune
    // Single-chrom entry case (the common PGOR shape: chrom_start == chrom_end).
    // Range overlap reduces to chrom equality + pos interval overlap.
    if (e.chromStart == e.chromEnd) {
        if (e.chromStart != rangeChrom_) return false;
        return e.posEnd >= rangePosLo_ && e.posStart <= rangePosHi_;
    }
    // Multi-chrom entry: query's chrom must fall in [chromStart, chromEnd].
    // The pos bound only matters at the boundary chroms; entries that
    // span past the query chrom always overlap.
    if (rangeChrom_ < e.chromStart || rangeChrom_ > e.chromEnd) return false;
    if (rangeChrom_ == e.chromStart && rangePosHi_ < e.posStart) return false;
    if (rangeChrom_ == e.chromEnd && rangePosLo_ > e.posEnd) return false;
    return true;
}

bool GordReader::isPastRange(const std::string& chrom, int64_t pos) const {
    if (!rangeActive_) return false;
    int c = chrom.compare(rangeChrom_);
    if (c > 0) return true;            // past the target chrom
    if (c < 0) return false;           // before the target chrom (shouldn't happen post-filter)
    return pos > rangePosHi_;          // same chrom, past upper bound
}

bool GordReader::declaredStartBeforeSeek(const GordEntry& e) const {
    // No declared range → we don't know where the file begins, so seek to be
    // safe (the file may start before the window).
    if (!e.hasRange) return true;
    // Compare the entry's declared start to the seek target. Strictly-before
    // means there are rows on an earlier chrom, or on the target chrom below
    // rangePosLo_, that a seek would skip. Equal/after → the entry already
    // begins in-window, so seeking would probe for nothing.
    int c = e.chromStart.compare(rangeChrom_);
    if (c < 0) return true;
    if (c > 0) return false;
    return e.posStart < rangePosLo_;
}

void GordReader::setTagFilter(std::vector<std::string> tags) {
    if (firstCallDone_) {
        throw std::runtime_error("GordReader::setTagFilter called after nextRow()");
    }
    if (tags.empty()) return;  // no explicit filter — full scan
    tagFilterRequested_ = true;
    pendingTags_ = std::move(tags);
    // Bucket selection + candidate pruning are deferred to finalize(), which
    // needs both the range and the tag request in hand.
}

bool GordReader::entryTagsMatch(const GordEntry& e) const {
    if (!tagFilterActive_) return true;
    for (const auto& t : e.tags) {
        if (tagFilter_.count(t)) return true;
    }
    return false;
}

bool GordReader::entryNeedsRowFilter(const GordEntry& e) const {
    // Only a bucket (several tags in one file, rows tagged by a trailing source
    // column) can hold rows for non-requested tags. If every one of the
    // bucket's tags is requested, all rows qualify (GOR's full match) → no row
    // filter; otherwise POSSIBLE_TAG → filter rows on the source column.
    if (!tagFilterActive_ || !e.sourceInserted) return false;
    for (const auto& t : e.tags) {
        if (!tagFilter_.count(t)) return true;  // a bucket tag not requested
    }
    return false;
}

void GordReader::parseKey(std::string_view row, std::string& outChrom, int64_t& outPos) {
    auto t1 = row.find('\t');
    auto t2 = row.find('\t', t1 == std::string_view::npos ? row.size() : t1 + 1);
    if (t1 == std::string_view::npos) {
        throw std::runtime_error("GORZ row missing tab between chrom and pos");
    }
    outChrom.assign(row.substr(0, t1));
    auto posSv = row.substr(t1 + 1,
        (t2 == std::string_view::npos ? row.size() : t2) - t1 - 1);
    outPos = 0;
    auto r = std::from_chars(posSv.data(), posSv.data() + posSv.size(), outPos);
    if (r.ec != std::errc()) {
        throw std::runtime_error("GORZ row pos column not a valid integer");
    }
}

void GordReader::admit(const GordEntry& entry, std::size_t entryIdx) {
    auto it = std::make_unique<OpenIterator>();
    it->in = opener_(entry.path);
    if (!it->in) {
        throw std::runtime_error("GORD entry: cannot open " + entry.path);
    }
    try {
        // Path-aware + opener so the entry can seek within its own .gorz
        // (via .gori or binary search over the stream, incl. object stores).
        // Lazy: the sidecar / seek only happen if seekTo is actually called.
        it->reader = std::make_unique<Reader>(*it->in, entry.path, opener_);
    } catch (const std::exception& e) {
        throw std::runtime_error("GORD entry " + entry.path + ": " + e.what());
    }
    it->entryIdx = entryIdx;
    it->rowFilter = entryNeedsRowFilter(entry);
    it->deletedTags = entry.deletedTags;
    // A bucket already carries the source column; a primary needs its tag
    // appended so both come out at the logical N+1 width (GOR insertSource).
    it->appendSource = !entry.sourceInserted;
    if (it->appendSource) {
        it->sourceValue = entry.tags.empty() ? std::string() : entry.tags.front();
    }

    // Intra-entry seek: jump to (rangeChrom_, rangePosLo_) inside this entry
    // — but only when the declared range starts strictly before that target,
    // so there are leading rows to skip. If the entry already begins in-range
    // the seek would probe for nothing, so we read from the top instead.
    // Best-effort (seekTo is no-throw); on any failure the sequential skip
    // below still produces correct rows.
    if (rangeActive_ && declaredStartBeforeSeek(entry)) {
        try {
            it->reader->seekTo(rangeChrom_, rangePosLo_);
        } catch (const std::exception&) {
            // Fall through to the sequential skip — correctness preserved.
        }
    }

    // Populate first in-range row. Skip rows < range lower bound; bail
    // on first row > range upper bound. Without a range filter, the
    // first row is always accepted.
    std::string_view rowView;
    while (it->reader->nextRow(rowView)) {
        std::string chrom;
        int64_t pos = 0;
        parseKey(rowView, chrom, pos);
        if (rangeActive_) {
            if (chrom != rangeChrom_) {
                // Row outside the chrom — for a multi-chrom entry the
                // rows we care about might be later, keep scanning.
                continue;
            }
            if (pos < rangePosLo_) continue;
            if (pos > rangePosHi_) {
                it->exhausted = true;
                break;
            }
        }
        // Bucket row filter (GOR's POSSIBLE_TAG): drop rows whose source column
        // (last field) isn't requested, or belongs to a |D|-deleted tag. A hash
        // probe on the already-parsed row; only bucket entries pay for it.
        if (it->rowFilter) {
            std::string src(lastField(rowView));
            if (!tagFilter_.count(src) || it->deletedTags.count(src)) continue;
        }
        it->nextRow_.assign(rowView);
        if (it->appendSource) {
            it->nextRow_.push_back('\t');
            it->nextRow_.append(it->sourceValue);
        }
        it->nextChrom = std::move(chrom);
        it->nextPos = pos;
        // Push onto the heap. Heap is a min-heap, so we use a
        // greater-than comparator with std::push_heap.
        heap_.push_back(std::move(it));
        std::push_heap(heap_.begin(), heap_.end(),
            [](const std::unique_ptr<OpenIterator>& a,
               const std::unique_ptr<OpenIterator>& b) {
                return cmpKey(a->nextChrom, a->nextPos, a->entryIdx,
                              b->nextChrom, b->nextPos, b->entryIdx) > 0;
            });
        return;
    }
    // Reader ended with no in-range row — drop the iterator
    // (closes the ifstream + Reader as `it` goes out of scope).
}

void GordReader::advanceTop() {
    // Pop heap-top, advance its reader, push back if still has a row.
    std::pop_heap(heap_.begin(), heap_.end(),
        [](const std::unique_ptr<OpenIterator>& a,
           const std::unique_ptr<OpenIterator>& b) {
            return cmpKey(a->nextChrom, a->nextPos, a->entryIdx,
                          b->nextChrom, b->nextPos, b->entryIdx) > 0;
        });
    auto it = std::move(heap_.back());
    heap_.pop_back();

    std::string_view rowView;
    while (it->reader->nextRow(rowView)) {
        std::string chrom;
        int64_t pos = 0;
        parseKey(rowView, chrom, pos);
        if (rangeActive_) {
            if (chrom != rangeChrom_) continue;
            if (pos < rangePosLo_) continue;
            if (pos > rangePosHi_) { it.reset(); return; }  // close + drop
        }
        if (it->rowFilter) {
            std::string src(lastField(rowView));
            if (!tagFilter_.count(src) || it->deletedTags.count(src)) continue;  // non-requested / deleted
        }
        it->nextRow_.assign(rowView);
        if (it->appendSource) {
            it->nextRow_.push_back('\t');
            it->nextRow_.append(it->sourceValue);
        }
        it->nextChrom = std::move(chrom);
        it->nextPos = pos;
        heap_.push_back(std::move(it));
        std::push_heap(heap_.begin(), heap_.end(),
            [](const std::unique_ptr<OpenIterator>& a,
               const std::unique_ptr<OpenIterator>& b) {
                return cmpKey(a->nextChrom, a->nextPos, a->entryIdx,
                              b->nextChrom, b->nextPos, b->entryIdx) > 0;
            });
        return;
    }
    // Reader exhausted — drop it
}

bool GordReader::nextRow(std::string_view& outLine) {
    finalize();
    firstCallDone_ = true;

    while (true) {
        // Admission loop: open any candidate whose declared start is at
        // or below the current heap-min key. Empty heap → admit the next
        // candidate (it's the earliest unopened).
        while (candidateCursor_ < candidates_.size()) {
            const auto& cand = candidates_[candidateCursor_];
            if (!heap_.empty()) {
                const auto& top = heap_.front();
                if (cmpKey(cand.chromStart, cand.posStart, candidateCursor_,
                           top->nextChrom, top->nextPos, top->entryIdx) > 0) {
                    break;  // candidate starts after heap-min — can wait
                }
            }
            std::size_t idx = candidateCursor_;
            ++candidateCursor_;
            admit(cand, idx);
        }

        if (heap_.empty()) return false;  // no more rows anywhere

        // Lazy columns init from first opened entry, when GORD header
        // didn't carry COLUMNS. Done after the first admission so we have
        // a Reader to ask.
        if (columns_.empty()) {
            columns_ = !gord_.columns.empty()
                ? gord_.columns
                : heap_.front()->reader->header().columns;
        }

        // The heap-min is the next row to emit.
        auto& top = heap_.front();
        if (isPastRange(top->nextChrom, top->nextPos)) {
            // Whole stream is past the range — close everything and EOF.
            heap_.clear();
            candidateCursor_ = candidates_.size();
            return false;
        }
        // Copy the row into our own owned buffer BEFORE advanceTop
        // mutates the heap. The outLine view stays valid until the next
        // nextRow() call. Without this copy, advanceTop's pop + maybe
        // re-push overwrites or destroys top->nextRow_ and outLine
        // becomes a dangling view into freed / SSO-relocated storage.
        outBuf_.assign(top->nextRow_);
        outLine = outBuf_;
        advanceTop();
        return true;
    }
}

int GordReader::cmpKey(const std::string& aChr, int64_t aPos, std::size_t aIdx,
                       const std::string& bChr, int64_t bPos, std::size_t bIdx) {
    int c = aChr.compare(bChr);
    if (c != 0) return c;
    if (aPos != bPos) return aPos < bPos ? -1 : 1;
    if (aIdx != bIdx) return aIdx < bIdx ? -1 : 1;
    return 0;
}

}  // namespace gorz
