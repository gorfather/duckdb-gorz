#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace gorz {

// Thrown on any decoder error (corrupt block, truncated input, codec error).
class InflateError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Raw deflate (no zlib/gzip wrapper) — matches Java's Deflater(nowrap=false)
// output that gormore writes via GorZipLexOutputStream.zipItZLib. Despite the
// nowrap=false flag, the writer feeds raw bytes to Deflater without enabling
// a wrapper, so the on-disk stream is raw deflate.
//
// hintUncompressedLen is a starting capacity for the output buffer; a typical
// GORZ block uncompressed is 32 KB, pass 64 * 1024 for headroom.
std::vector<uint8_t> inflate_zlib(const uint8_t* src, std::size_t srcLen,
                                  std::size_t hintUncompressedLen = 64 * 1024);

// zstd single-frame decompress. The frame includes its own uncompressed-size
// header so we don't need a hint.
std::vector<uint8_t> inflate_zstd(const uint8_t* src, std::size_t srcLen);

// Inverse of {@link inflate_zlib}: zlib-wrapped deflate (windowBits = 15)
// so the output is byte-identical to what Java's `new Deflater(level)` /
// {@code Inflater()} pair would round-trip. Used by the .gorz writer side.
std::vector<uint8_t> deflate_zlib(const uint8_t* src, std::size_t srcLen,
                                  int level = 6);

}  // namespace gorz
