#pragma once

// Base-128 (7-bit) encoding used by GORZ for the compressed block payload.
// Source: org.gorpipe.util.collection.ByteArray.to7Bit/to8Bit in the gormore
// Java tree. Every input byte 0..255 ends up as a byte in 33..160, so the
// encoded stream never contains '\t' (0x09) or '\n' (0x0A) — the GORZ block
// delimiters can be scanned without decoding.
//
// Encoding ratio: 8 input bytes → 7 output bytes (i.e. encoded is 8/7 longer).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gorz {

// Decode N 7-bit bytes back to floor(N * 7 / 8) 8-bit bytes.
// Out is sized to fit; caller can rely on the returned vector's size().
std::vector<uint8_t> base128_decode(const uint8_t* src, std::size_t srcLen);

// Inverse of base128_decode: pack N 8-bit input bytes into
// ceil(N * 8 / 7) output bytes, each in the range 33..160 (so the encoded
// stream never contains '\t' or '\n' — the GORZ block delimiters). Used
// by the writer side.
std::vector<uint8_t> base128_encode(const uint8_t* src, std::size_t srcLen);

}  // namespace gorz
