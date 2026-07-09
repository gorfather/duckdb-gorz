#include "gorz/BlockPacker.h"

#include <charconv>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace gorz {

namespace {

inline uint16_t readU16BE(const uint8_t* src, std::size_t pos) {
    return (uint16_t(src[pos]) << 8) | uint16_t(src[pos + 1]);
}
inline int16_t readI16BE(const uint8_t* src, std::size_t pos) {
    return int16_t(readU16BE(src, pos));
}
inline uint32_t readU32BE(const uint8_t* src, std::size_t pos) {
    return (uint32_t(src[pos]) << 24) | (uint32_t(src[pos + 1]) << 16) |
           (uint32_t(src[pos + 2]) << 8) | uint32_t(src[pos + 3]);
}
inline int64_t readI64BE(const uint8_t* src, std::size_t pos) {
    int64_t hi = int32_t(readU32BE(src, pos));
    int64_t lo = readU32BE(src, pos + 4);
    return (hi << 32) | lo;
}

// Append `value` formatted as base-10 ASCII to `out`. Equivalent to
// ByteTextBuilder.writeLong in the Java tree.
inline void appendLong(std::string& out, int64_t value) {
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, r.ptr);
}

// Append a zero-terminated UTF-8 string starting at src[pos] to out. Returns
// the byte position just past the terminator.
std::size_t appendCString(std::string& out, const uint8_t* src, std::size_t pos) {
    std::size_t start = pos;
    while (src[pos] != 0) ++pos;
    out.append(reinterpret_cast<const char*>(src + start), pos - start);
    return pos + 1;  // skip the terminator
}

}  // namespace

void parseExternalLookupTables(const uint8_t* src, std::size_t srcLen,
                               ExternalTablesMap& out) {
    if (srcLen < 2) {
        throw FormatError("external-lookup-table bytes < 2");
    }
    const uint16_t colCnt = readU16BE(src, 0);
    std::size_t pos = 2;
    int colIdx = 0;
    for (uint16_t i = 0; i < colCnt; ++i) {
        if (pos >= srcLen) throw FormatError("truncated external lookup table");
        const int colIdxDiff = src[pos++];
        colIdx += colIdxDiff;
        if (pos + 2 > srcLen) throw FormatError("truncated MAPCNT");
        const int mapCnt = readU16BE(src, pos);
        pos += 2;
        auto& colMap = out[colIdx];
        for (int j = 0; j < mapCnt; ++j) {
            const std::size_t begin = pos;
            while (pos < srcLen && src[pos] != 0) ++pos;
            if (pos == srcLen) throw FormatError("unterminated lookup string");
            colMap[j].assign(reinterpret_cast<const char*>(src + begin),
                             pos - begin);
            ++pos;  // skip terminator
        }
    }
}

namespace {

// Per-decoder state. Each impl carries its own working state (cursors, last
// values) and emits one row's column value per call.
struct ColDecoder {
    virtual ~ColDecoder() = default;
    virtual void emitNext(std::string& out) = 0;
    virtual std::size_t readLen() const = 0;
};

struct ByteOffsetDecoder final : ColDecoder {  // TYPEID 5
    const uint8_t* src;
    std::size_t start;
    int rowcnt;
    int64_t base;
    std::size_t cursor;
    ByteOffsetDecoder(const uint8_t* s, std::size_t p, int rc)
        : src(s), start(p + 8), rowcnt(rc), base(readI64BE(s, p)), cursor(p + 8) {}
    void emitNext(std::string& out) override {
        appendLong(out, base + src[cursor++]);
    }
    std::size_t readLen() const override { return 8 + std::size_t(rowcnt); }
};

struct IncrDecoder final : ColDecoder {  // TYPEID 7
    int64_t next;
    int32_t step;
    IncrDecoder(const uint8_t* s, std::size_t p)
        : next(readI64BE(s, p)), step(int32_t(readU32BE(s, p + 8))) {}
    void emitNext(std::string& out) override {
        next += step;
        appendLong(out, next);
    }
    std::size_t readLen() const override { return 8 + 4; }
};

struct ByteDiffDecoder final : ColDecoder {  // TYPEID 12
    const uint8_t* src;
    int rowcnt;
    int64_t last;
    std::size_t cursor;
    ByteDiffDecoder(const uint8_t* s, std::size_t p, int rc)
        : src(s), rowcnt(rc), last(readI64BE(s, p)), cursor(p + 8) {}
    void emitNext(std::string& out) override {
        last += static_cast<int8_t>(src[cursor++]);
        appendLong(out, last);
    }
    std::size_t readLen() const override { return 8 + std::size_t(rowcnt); }
};

struct ShortDiffDecoder final : ColDecoder {  // TYPEID 13
    const uint8_t* src;
    int rowcnt;
    int64_t last;
    std::size_t cursor;
    ShortDiffDecoder(const uint8_t* s, std::size_t p, int rc)
        : src(s), rowcnt(rc), last(readI64BE(s, p)), cursor(p + 8) {}
    void emitNext(std::string& out) override {
        last += readI16BE(src, cursor);
        cursor += 2;
        appendLong(out, last);
    }
    std::size_t readLen() const override { return 8 + 2 * std::size_t(rowcnt); }
};

struct VcSeqDecoder final : ColDecoder {  // TYPEID 21
    const uint8_t* src;
    uint32_t payloadLen;
    std::size_t cursor;
    VcSeqDecoder(const uint8_t* s, std::size_t p)
        : src(s), payloadLen(readU32BE(s, p)), cursor(p + 4) {}
    void emitNext(std::string& out) override {
        cursor = appendCString(out, src, cursor);
    }
    std::size_t readLen() const override { return 4 + payloadLen; }
};

struct TConstDecoder final : ColDecoder {  // TYPEID 23
    std::string value;
    std::size_t consumed;  // length + 1 for terminator
    TConstDecoder(const uint8_t* s, std::size_t p) {
        std::size_t start = p;
        while (s[p] != 0) ++p;
        value.assign(reinterpret_cast<const char*>(s + start), p - start);
        consumed = (p - start) + 1;
    }
    void emitNext(std::string& out) override { out.append(value); }
    std::size_t readLen() const override { return consumed; }
};

struct ExtTLookupDiffDecoder final : ColDecoder {  // TYPEID 25
    const uint8_t* src;
    int rowcnt;
    std::size_t cursor;
    int lastVal;
    const std::unordered_map<int, std::string>* tbl;
    ExtTLookupDiffDecoder(const uint8_t* s, std::size_t p, int rc,
                          const std::unordered_map<int, std::string>* table)
        : src(s), rowcnt(rc), cursor(p), lastVal(0), tbl(table) {}
    void emitNext(std::string& out) override {
        lastVal += static_cast<int8_t>(src[cursor++]);
        if (tbl) {
            auto it = tbl->find(lastVal);
            if (it != tbl->end()) out.append(it->second);
        }
    }
    std::size_t readLen() const override { return std::size_t(rowcnt); }
};

std::unique_ptr<ColDecoder>
makeDecoder(int typeId, const uint8_t* src, std::size_t pos, int rowcnt,
            const std::unordered_map<int, std::string>* extTable) {
    switch (typeId) {
        case 5:  return std::make_unique<ByteOffsetDecoder>(src, pos, rowcnt);
        case 7:  return std::make_unique<IncrDecoder>(src, pos);
        case 12: return std::make_unique<ByteDiffDecoder>(src, pos, rowcnt);
        case 13: return std::make_unique<ShortDiffDecoder>(src, pos, rowcnt);
        case 21: return std::make_unique<VcSeqDecoder>(src, pos);
        case 23: return std::make_unique<TConstDecoder>(src, pos);
        case 25: return std::make_unique<ExtTLookupDiffDecoder>(src, pos, rowcnt, extTable);
        default:
            throw UnsupportedTypeIdError(
                "columnar TYPEID " + std::to_string(typeId) +
                " not implemented yet (phase 3 covers 5, 7, 12, 13, 21, 23, 25;"
                " remaining 3, 4, 9, 10, 11, 19, 20, 22, 24, 27, 28, 29"
                " come in a follow-up)");
    }
}

}  // namespace

std::string decodeBlock(const uint8_t* src, std::size_t srcLen,
                        const ExternalTablesMap& externalTables) {
    if (srcLen < 2) throw FormatError("columnar block < 2 bytes");
    const int rowcnt = readU16BE(src, 0);
    std::size_t pos = 2;

    // TYPEID list, zero-terminated.
    std::vector<int> types;
    while (pos < srcLen && src[pos] != 0) {
        types.push_back(src[pos++]);
    }
    if (pos == srcLen) throw FormatError("columnar block missing TYPEID terminator");
    ++pos;  // skip zero terminator

    // Build per-column decoders.
    std::vector<std::unique_ptr<ColDecoder>> decoders;
    decoders.reserve(types.size());
    for (std::size_t i = 0; i < types.size(); ++i) {
        const std::unordered_map<int, std::string>* tbl = nullptr;
        auto it = externalTables.find(int(i));
        if (it != externalTables.end()) tbl = &it->second;
        auto d = makeDecoder(types[i], src, pos, rowcnt, tbl);
        pos += d->readLen();
        decoders.push_back(std::move(d));
    }

    // Emit rows.
    std::string out;
    out.reserve(std::size_t(rowcnt) * 64);
    for (int r = 0; r < rowcnt; ++r) {
        for (std::size_t c = 0; c < decoders.size(); ++c) {
            if (c) out.push_back('\t');
            decoders[c]->emitNext(out);
        }
        out.push_back('\n');
    }
    return out;
}

}  // namespace gorz
