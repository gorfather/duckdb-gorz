#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace gorz {

// Minimal .gorz writer. Used by tests and lightweight tools — production
// writes typically go through the Java GorZipLexOutputStream.
//
// Writes a canonical GOR header (`<col0>\t...\n`, no leading '#') followed by
// row-oriented (non-columnar) zlib-wrapped deflate blocks — the same on-disk
// format the reader's row-oriented decode path consumes (flag byte 0x00) and
// that gorpipe reads/seeks natively. Each block carries an uncompressed prefix
// `<chrom>\t<pos>\t` where (chrom, pos) is the LAST row in the block; the
// reader uses that for both .gori-driven and binary-search seek.
//
// Optional .gori sidecar: pass a non-null `goriOut` to also emit
// `<chrom>\t<pos>\t<offset_after_block>` per flushed block (preceded by
// the format-marker header) — the same shape the production tooling
// produces. See GorIndexFile in the gormore Java tree.
class Writer {
public:
    Writer(std::ostream& out,
           const std::vector<std::string>& columns,
           std::ostream* goriOut = nullptr,
           std::size_t targetBlockBytes = 32 * 1024);
    ~Writer();

    // Append a row. `chrom` and `pos` are the key columns and must arrive
    // in (chrom, pos) sorted order; the writer trusts the caller and does
    // not re-sort. `rest` is the tab-separated trailing fields with NO
    // leading or trailing '\t' — i.e. `"alt\tac\t..."` for the gnomAD
    // schema. May be empty when the row is exactly (chrom, pos).
    void writeRow(std::string_view chrom, int64_t pos, std::string_view rest = "");

    // Flush any remaining rows + close the .gori (if any). Idempotent.
    void close();

private:
    void flushBlock();

    std::ostream& out_;
    std::ostream* goriOut_;
    std::size_t targetBlockBytes_;

    // Raw block buffer — rows joined by '\n'. Holds at most one block's
    // worth of payload (configurable; default 32 KiB).
    std::string blockBuf_;
    std::string lastChrom_;
    int64_t lastPos_ = 0;

    bool headerWritten_ = false;
    bool closed_ = false;

    // Running byte offset into `out_`. Tracked so we don't have to call
    // tellp() (which is unreliable on some std::ostream subclasses) for
    // the .gori entries.
    int64_t bytesWritten_ = 0;
};

}  // namespace gorz
