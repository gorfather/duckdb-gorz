#include "gorz/Base128.h"

namespace gorz {

// Port of ByteArray.to8Bit (Java, model/util) — see Base128.h for the
// reference. Two source bytes (each holding 7 payload bits, offset by 33)
// pack into one 8-bit output. After 7 outputs the bit cursor wraps and one
// source byte is skipped (the carry).
std::vector<uint8_t> base128_decode(const uint8_t* src, std::size_t srcLen) {
    std::vector<uint8_t> out;
    if (srcLen < 2) return out;
    out.resize((srcLen * 7) / 8);

    int bit = 0;
    std::size_t readPos = 0;
    std::size_t writePos = 0;
    while (readPos < srcLen - 1 && writePos < out.size()) {
        const uint8_t b1 = static_cast<uint8_t>(src[readPos] - 33);
        const uint8_t b2 = static_cast<uint8_t>(src[++readPos] - 33);
        out[writePos++] =
            static_cast<uint8_t>((b1 >> bit) | (b2 << (7 - bit)));
        if (++bit == 7) {
            bit = 0;
            ++readPos;
        }
    }
    return out;
}

// Inverse of base128_decode. Walks the input bit-by-bit and emits one
// 7-bit byte (plus the +33 offset) each time we've accumulated 7 payload
// bits. After 7 inputs / 8 outputs the cursor wraps and we emit one extra
// carry byte. Final partial flush at end-of-input.
std::vector<uint8_t> base128_encode(const uint8_t* src, std::size_t srcLen) {
    std::vector<uint8_t> out;
    if (srcLen == 0) return out;
    // 7 input bytes → 8 output bytes; ceil for the trailing partial.
    out.reserve((srcLen * 8 + 6) / 7 + 1);

    int bit = 0;
    unsigned int carry = 0;
    for (std::size_t i = 0; i < srcLen; ++i) {
        unsigned int b = src[i];
        out.push_back(static_cast<uint8_t>(((carry | (b << bit)) & 0x7F) + 33));
        carry = b >> (7 - bit);
        if (++bit == 7) {
            // After 7 inputs the accumulator has a full extra byte — flush.
            out.push_back(static_cast<uint8_t>((carry & 0x7F) + 33));
            carry = 0;
            bit = 0;
        }
    }
    if (bit > 0) {
        // Trailing partial: emit whatever payload bits are left.
        out.push_back(static_cast<uint8_t>((carry & 0x7F) + 33));
    }
    return out;
}

}  // namespace gorz
