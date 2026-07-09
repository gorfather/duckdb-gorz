#include "gorz/Writer.h"

#include "gorz/Base128.h"
#include "gorz/Inflate.h"

#include <algorithm>
#include <stdexcept>

namespace gorz {

namespace {

// Write a raw byte buffer to the output stream and bump our running
// counter. We don't trust tellp() — some istream/ostream subclasses
// (notably stringstream after readback) return -1.
void writeRaw(std::ostream& out, const char* data, std::size_t n, int64_t& counter) {
    out.write(data, static_cast<std::streamsize>(n));
    counter += static_cast<int64_t>(n);
}

void writeRaw(std::ostream& out, const std::string& s, int64_t& counter) {
    writeRaw(out, s.data(), s.size(), counter);
}

}  // namespace

Writer::Writer(std::ostream& out,
               const std::vector<std::string>& columns,
               std::ostream* goriOut,
               std::size_t targetBlockBytes)
    : out_(out), goriOut_(goriOut), targetBlockBytes_(targetBlockBytes) {
    // Header: `<col0>\t<col1>\t...<colN>\n` — column names verbatim, no
    // leading marker. Matches GorZipLexOutputStream (which writes no '#').
    // Both readers tolerate an optional leading '#' (Reader::readHeader strips
    // it if present), but the canonical form omits it.
    std::string header;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) header.push_back('\t');
        header.append(columns[i]);
    }
    header.push_back('\n');
    writeRaw(out_, header, bytesWritten_);
    headerWritten_ = true;

    if (goriOut_) {
        // .gori format-marker line (parsed by lib/gorz/src/GoriIndex.cpp).
        const char kMarker[] = "## fileformat=GORIv2\n";
        goriOut_->write(kMarker, sizeof(kMarker) - 1);
    }
}

Writer::~Writer() {
    try {
        close();
    } catch (...) {
        // Destructors must not throw. Errors should have been surfaced
        // via explicit close() — silently swallow on stack unwind.
    }
}

void Writer::writeRow(std::string_view chrom, int64_t pos, std::string_view rest) {
    if (closed_) {
        throw std::logic_error("gorz::Writer::writeRow after close()");
    }
    // Append `<chrom>\t<pos>\t<rest>\n` (no rest tab when rest is empty).
    blockBuf_.append(chrom.data(), chrom.size());
    blockBuf_.push_back('\t');
    blockBuf_.append(std::to_string(pos));
    if (!rest.empty()) {
        blockBuf_.push_back('\t');
        blockBuf_.append(rest.data(), rest.size());
    }
    blockBuf_.push_back('\n');

    lastChrom_.assign(chrom.data(), chrom.size());
    lastPos_ = pos;

    if (blockBuf_.size() >= targetBlockBytes_) {
        flushBlock();
    }
}

void Writer::flushBlock() {
    if (blockBuf_.empty()) return;

    // Trim the trailing '\n' — the on-disk block payload (when
    // decompressed) is rows separated by '\n', no terminator on the
    // last one. (Matches what Reader::readNextBlock produces during
    // decode.)
    if (!blockBuf_.empty() && blockBuf_.back() == '\n') {
        blockBuf_.pop_back();
    }

    auto compressed = deflate_zlib(
        reinterpret_cast<const uint8_t*>(blockBuf_.data()), blockBuf_.size());
    auto encoded = base128_encode(compressed.data(), compressed.size());

    // Block prefix: `<lastChrom>\t<lastPos>\t<flag><encoded>\n`
    // flag = 0x00: row-oriented zlib (the format flavour Reader handles
    // without external lookup tables).
    writeRaw(out_, lastChrom_, bytesWritten_);
    writeRaw(out_, "\t", 1, bytesWritten_);
    std::string posStr = std::to_string(lastPos_);
    writeRaw(out_, posStr, bytesWritten_);
    writeRaw(out_, "\t", 1, bytesWritten_);
    const char flag = 0x00;
    writeRaw(out_, &flag, 1, bytesWritten_);
    writeRaw(out_, reinterpret_cast<const char*>(encoded.data()),
             encoded.size(), bytesWritten_);
    writeRaw(out_, "\n", 1, bytesWritten_);

    if (goriOut_) {
        // `<chrom>\t<pos>\t<offset>` where offset is the byte right AFTER
        // the block we just wrote — i.e. start of the next block.
        std::string line;
        line.append(lastChrom_);
        line.push_back('\t');
        line.append(posStr);
        line.push_back('\t');
        line.append(std::to_string(bytesWritten_));
        line.push_back('\n');
        goriOut_->write(line.data(), static_cast<std::streamsize>(line.size()));
    }

    blockBuf_.clear();
}

void Writer::close() {
    if (closed_) return;
    flushBlock();
    closed_ = true;
}

}  // namespace gorz
