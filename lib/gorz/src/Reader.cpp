#include "gorz/Reader.h"

#include "gorz/Base128.h"
#include "gorz/BlockPacker.h"
#include "gorz/Inflate.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace gorz {

namespace {

// Read bytes up to (and including) the next occurrence of delimiter.
// Returns false at EOF before delimiter is found. The returned buffer
// excludes the delimiter byte itself.
bool readUntil(std::istream& in, uint8_t delim, std::vector<uint8_t>& out) {
    out.clear();
    int c;
    while ((c = in.get()) != EOF) {
        if (static_cast<uint8_t>(c) == delim) return true;
        out.push_back(static_cast<uint8_t>(c));
    }
    return !out.empty();
}

std::vector<std::string> splitTabs(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\t') {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

// --- Binary-search seek (used when no .gori sidecar is present) ----------
//
// gor / gorz files are sorted by (chrom, pos), and every .gorz block on
// disk starts with an uncompressed `<chrom>\t<pos>\t` prefix where
// (chrom, pos) is the LAST row in that block (see GorZipLexOutputStream
// in the Java tree). So a chrom/pos seek without .gori reduces to a
// classic byte-range bisection: pick a midpoint, find the next block
// boundary at or after it, read just the 2-field prefix (no
// decompression needed), and narrow the range.

struct BlockPrefix {
    std::string chrom;
    int64_t pos = 0;
    bool ok = false;
};

int compareKey(const std::string& aChr, int64_t aPos,
               const std::string& bChr, int64_t bPos) {
    if (aChr < bChr) return -1;
    if (aChr > bChr) return 1;
    if (aPos < bPos) return -1;
    if (aPos > bPos) return 1;
    return 0;
}

// Read the `<chrom>\t<pos>\t` prefix at the given byte offset (assumed to
// be a block start). Best-effort: returns {ok=false} on any malformed
// input or premature EOF; callers fall back to sequential scan.
BlockPrefix readBlockPrefix(std::istream& in, int64_t blockStart) {
    BlockPrefix p;
    in.clear();
    in.seekg(blockStart, std::ios::beg);
    if (!in) return p;

    int c;
    while ((c = in.get()) != EOF) {
        if (c == '\t') break;
        if (c == '\n') return p;
        if (p.chrom.size() >= 64) return p;  // sanity bound — chroms are short
        p.chrom.push_back(static_cast<char>(c));
    }
    if (c == EOF) return p;

    int64_t pos = 0;
    int digits = 0;
    while ((c = in.get()) != EOF) {
        if (c == '\t') break;
        if (c < '0' || c > '9') return p;
        pos = pos * 10 + (c - '0');
        if (++digits > 18) return p;  // sanity bound — pos fits in int64
    }
    if (digits == 0) return p;
    p.pos = pos;
    p.ok = true;
    return p;
}

// Return the byte offset of the next block start strictly AFTER `fromByte`.
// Implemented by scanning forward for the block-terminating '\n'; the byte
// right after is the next block start. Returns `fileSize` when no further
// '\n' exists.
//
// Buffered read for speed: ~16 KiB chunks rather than one-byte at a time,
// which would otherwise dominate bisection wall time on cold pages.
int64_t findNextBlockStart(std::istream& in, int64_t fromByte, int64_t fileSize) {
    if (fromByte >= fileSize) return fileSize;
    in.clear();
    in.seekg(fromByte, std::ios::beg);
    if (!in) return fileSize;
    constexpr std::size_t kChunk = 16 * 1024;
    char buf[kChunk];
    int64_t cur = fromByte;
    while (cur < fileSize) {
        std::streamsize want = static_cast<std::streamsize>(
            std::min<int64_t>(kChunk, fileSize - cur));
        in.read(buf, want);
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        for (std::streamsize i = 0; i < got; ++i) {
            if (buf[i] == '\n') {
                return cur + i + 1;
            }
        }
        cur += got;
    }
    return fileSize;
}

// Bisect for the block whose LAST (chrom, pos) >= target. Returns the
// byte offset of that block's start, or `fileSize` when the target is past
// every block. Used when no .gori sidecar is available.
//
// Invariant during the loop:
//   `lo` is a block start known to have last-key < target (or `lo` is the
//   first block and the first-block check below already said "no match").
//   `hi` is either `fileSize` or a block start known to have last-key >=
//   target.
//
// On termination we walk forward from `lo` one block at a time until we
// find a block whose last-key >= target (or hit `hi`). That walk is
// linear but bounded by however coarsely the bisection landed — at most
// a handful of blocks in practice.
int64_t bisectBlockBracket(std::istream& in, int64_t fileSize, int64_t headerEnd,
                           const std::string& targetChrom, int64_t targetPos) {
    if (fileSize <= headerEnd) return fileSize;

    BlockPrefix p0 = readBlockPrefix(in, headerEnd);
    if (!p0.ok) return fileSize;  // malformed file; give up
    if (compareKey(p0.chrom, p0.pos, targetChrom, targetPos) >= 0) {
        return headerEnd;
    }

    int64_t lo = headerEnd;
    int64_t hi = fileSize;
    int safety = 64;  // log2(2^64) — well above any real file's block count
    while (hi - lo > 1 && --safety > 0) {
        int64_t mid = lo + (hi - lo) / 2;
        int64_t probe = findNextBlockStart(in, mid, fileSize);
        if (probe >= hi) break;          // can't make a useful probe
        if (probe <= lo) break;          // shouldn't happen, defensive
        BlockPrefix p = readBlockPrefix(in, probe);
        if (!p.ok) break;
        int cmp = compareKey(p.chrom, p.pos, targetChrom, targetPos);
        if (cmp >= 0) {
            hi = probe;
        } else {
            lo = probe;
        }
    }

    // Walk forward from lo's next block until we find one with last >= target.
    int64_t next = findNextBlockStart(in, lo, fileSize);
    int64_t walked = 0;
    constexpr int64_t kMaxLinearBlocks = 1024;
    while (next < hi && walked++ < kMaxLinearBlocks) {
        BlockPrefix p = readBlockPrefix(in, next);
        if (!p.ok) return next;  // hand the offset over; reader will surface
        if (compareKey(p.chrom, p.pos, targetChrom, targetPos) >= 0) {
            return next;
        }
        next = findNextBlockStart(in, next, fileSize);
    }
    return hi;
}

}  // namespace

Reader::Reader(std::istream& in) : in_(in) {
    readHeader();
}

Reader::Reader(std::istream& in, std::string sourcePath, Opener opener)
    : in_(in), sourcePath_(std::move(sourcePath)), opener_(std::move(opener)) {
    readHeader();
}

void Reader::ensureIndexLoaded() {
    if (index_.has_value()) return;
    if (sourcePath_.empty()) {
        // No path → no sidecar discovery. Mark as "attempted, none found"
        // so subsequent calls short-circuit.
        index_.emplace();
        return;
    }
    try {
        index_ = parseGoriIndex(sourcePath_, opener_);
    } catch (const std::exception&) {
        // Malformed sidecar: behave the same as a missing one. Sequential
        // scan still produces correct results; we just lose the seek.
        index_.emplace();
    }
}

bool Reader::hasIndex() {
    ensureIndexLoaded();
    return index_.has_value() && !index_->entries.empty();
}

int64_t Reader::fileSize() {
    if (fileSize_ >= 0) return fileSize_;
    auto cur = in_.tellg();
    in_.clear();
    in_.seekg(0, std::ios::end);
    auto end = in_.tellg();
    if (cur >= 0) {
        in_.clear();
        in_.seekg(cur, std::ios::beg);
    }
    fileSize_ = end >= 0 ? static_cast<int64_t>(end) : 0;
    return fileSize_;
}

void Reader::clearChromFilter() {
    chromFilterActive_ = false;
    chromFilter_.clear();
}

void Reader::setByteRange(int64_t startOffset, int64_t endOffset) {
    byteRangeActive_ = true;
    byteRangeAligned_ = false;
    byteRangeStart_ = startOffset;
    byteRangeEnd_ = endOffset;
}

bool Reader::seekTo(const std::string& chrom, int64_t pos) {
    ensureIndexLoaded();
    // Install the single-chrom filter regardless of how the underlying
    // seek resolves. nextRow() will refuse to cross into a later chrom
    // and silently skip stragglers from an earlier one.
    chromFilterActive_ = true;
    chromFilter_ = chrom;

    int64_t target = -1;  // block start to seek to; -1 = "no decision"
    bool indexHit = index_.has_value() && !index_->entries.empty();

    if (indexHit) {
        // Path A: .gori sidecar bracketed search. findBlockStart returns
        // the byte offset of the first block whose last-row >= (chrom, pos).
        auto offset = findBlockStart(*index_, chrom, pos, headerEndOffset_);
        if (offset.has_value()) {
            target = *offset;
        } else {
            // Past every indexed block.
            target = fileSize();
        }
    } else {
        // Path B: no sidecar — binary-search the block boundaries directly.
        // gor rows are sorted by (chrom, pos), and each block's
        // uncompressed prefix is the LAST row's (chrom, pos), so bisection
        // by byte offset converges on the bracketing block in O(log N)
        // probe reads.
        target = bisectBlockBracket(in_, fileSize(), headerEndOffset_, chrom, pos);
    }

    if (target >= fileSize()) {
        // Target past every block — position the reader at EOF so the next
        // nextRow() call returns false cleanly.
        in_.clear();
        in_.seekg(0, std::ios::end);
        blockBytes_.clear();
        blockCursor_ = blockEnd_ = 0;
        return true;
    }
    seekToBlockStart(target);
    return true;
}

void Reader::readHeader() {
    // Spec: the header is the first '\n'-terminated chunk. If it contains a
    // '\0', the bytes before are the text header and the bytes after are
    // the (compressed, encoded) external lookup table for columnar files.
    std::vector<uint8_t> buf;
    if (!readUntil(in_, '\n', buf)) {
        throw FormatError("empty file / no header line");
    }

    auto nulPos = std::find(buf.begin(), buf.end(), '\0');
    std::string headerText;
    if (nulPos != buf.end()) {
        headerText.assign(buf.begin(), nulPos);
        header_.columnarHeader = true;
        header_.externalLookupRaw.assign(nulPos + 1, buf.end());
    } else {
        headerText.assign(buf.begin(), buf.end());
    }
    if (!headerText.empty() && headerText.front() == '#') {
        headerText.erase(0, 1);  // strip leading '#' marker
    }
    header_.columns = splitTabs(headerText);

    // tellg may return -1 on non-seekable streams (e.g. pipes). That's
    // fine — seekToBlockStart will throw if the caller later tries to
    // use it.
    headerEndOffset_ = static_cast<int64_t>(in_.tellg());
}

void Reader::seekToBlockStart(int64_t offset) {
    if (offset < 0) {
        throw FormatError("seekToBlockStart: negative offset");
    }
    in_.clear();  // clear any EOF flags from previous reads
    in_.seekg(offset, std::ios::beg);
    if (!in_) {
        throw FormatError("seekToBlockStart: underlying stream is not seekable");
    }
    // Invalidate the current block — the next nextRow() will read fresh.
    blockBytes_.clear();
    blockCursor_ = 0;
    blockEnd_ = 0;
}

bool Reader::readNextBlock() {
    // Byte-range restriction for parallel reads (see setByteRange). Realign to
    // a block boundary on the first read, then stop once the next block would
    // begin at/after the range end.
    if (byteRangeActive_) {
        if (!byteRangeAligned_) {
            byteRangeAligned_ = true;
            in_.clear();
            if (byteRangeStart_ <= headerEndOffset_) {
                in_.seekg(headerEndOffset_);  // first partition → first block
            } else {
                // Skip the partial block owned by the previous partition:
                // consume up to (and past) its terminating '\n'. The next byte
                // is our first block's start.
                in_.seekg(static_cast<std::streamoff>(byteRangeStart_));
                std::vector<uint8_t> discard;
                if (!readUntil(in_, '\n', discard)) return false;  // no boundary before EOF
            }
        }
        // Own only blocks whose start byte is < end (tellg() is the next
        // block's start). A block straddling `end` still belongs to us.
        std::streampos cur = in_.tellg();
        if (cur == std::streampos(-1) ||
            static_cast<int64_t>(cur) >= byteRangeEnd_) {
            return false;
        }
    }

    // Block on disk: <chr>\t<pos>\t<flag-byte><encoded-payload>\n
    std::vector<uint8_t> chr, pos;
    if (!readUntil(in_, '\t', chr)) return false;  // clean EOF
    if (!readUntil(in_, '\t', pos)) {
        throw FormatError("truncated block: missing pos field");
    }

    int flagInt = in_.get();
    if (flagInt == EOF) {
        throw FormatError("truncated block: missing flag byte");
    }
    const uint8_t flagByte = static_cast<uint8_t>(flagInt);
    const bool columnar = (flagByte & 0x01) != 0;
    const bool zstd = (flagByte & 0x02) != 0;

    std::vector<uint8_t> encoded;
    if (!readUntil(in_, '\n', encoded)) {
        throw FormatError("truncated block: missing payload / terminator");
    }
    if (encoded.empty()) {
        throw FormatError("empty block payload");
    }

    std::vector<uint8_t> compressed =
        base128_decode(encoded.data(), encoded.size());
    std::vector<uint8_t> decompressed;
    if (zstd) {
        decompressed = inflate_zstd(compressed.data(), compressed.size());
    } else {
        decompressed = inflate_zlib(compressed.data(), compressed.size());
    }

    if (columnar) {
        // Parse the external lookup tables on first use. The header stashed
        // the compressed bytes; decompress + parse here once.
        if (!externalTablesParsed_) {
            if (!header_.externalLookupRaw.empty()) {
                std::vector<uint8_t> rawLookup = base128_decode(
                    header_.externalLookupRaw.data(),
                    header_.externalLookupRaw.size());
                std::vector<uint8_t> lookupBytes;
                try {
                    lookupBytes = zstd ? inflate_zstd(rawLookup.data(), rawLookup.size())
                                       : inflate_zlib(rawLookup.data(), rawLookup.size());
                } catch (const InflateError& e) {
                    throw FormatError(std::string("external lookup table inflate failed: ")
                        + e.what());
                }
                parseExternalLookupTables(lookupBytes.data(), lookupBytes.size(),
                                          externalTables_);
            }
            externalTablesParsed_ = true;
        }
        std::string decoded = decodeBlock(decompressed.data(),
                                          decompressed.size(),
                                          externalTables_);
        blockBytes_.assign(decoded.begin(), decoded.end());
    } else {
        blockBytes_ = std::move(decompressed);
    }
    blockCursor_ = 0;
    blockEnd_ = blockBytes_.size();
    return true;
}

bool Reader::nextRow(std::string_view& outLine) {
    // Loop in case the chrom filter rejects (skips earlier-chrom stragglers).
    while (true) {
        while (blockCursor_ >= blockEnd_) {
            if (!readNextBlock()) return false;
        }
        // Find the next '\n' in the current block; this delimits one row.
        std::size_t lineStart = blockCursor_;
        while (blockCursor_ < blockEnd_ && blockBytes_[blockCursor_] != '\n') {
            ++blockCursor_;
        }
        const char* base = reinterpret_cast<const char*>(blockBytes_.data());
        std::string_view candidate(base + lineStart, blockCursor_ - lineStart);
        // Position of the consumed newline (so we can rewind if we need
        // to "un-read" — see the past-chrom branch below).
        std::size_t afterNewline = (blockCursor_ < blockEnd_)
                ? blockCursor_ + 1 : blockCursor_;

        if (chromFilterActive_) {
            auto tab = candidate.find('\t');
            std::string_view rowChrom = (tab == std::string_view::npos)
                    ? candidate : candidate.substr(0, tab);
            if (rowChrom > chromFilter_) {
                // Crossed into a later chrom: stop iteration. Leave the
                // cursor at the start of this row so subsequent nextRow()
                // calls keep returning false rather than silently
                // resuming with the later-chrom row.
                blockCursor_ = lineStart;
                return false;
            }
            if (rowChrom < chromFilter_) {
                // A straggler from an earlier chrom — can happen in the
                // first block we land on after a coarse seek. Skip and
                // keep looking.
                blockCursor_ = afterNewline;
                continue;
            }
        }
        blockCursor_ = afterNewline;
        outLine = candidate;
        return true;
    }
}

}  // namespace gorz
