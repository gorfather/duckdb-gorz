#include "gorz/Inflate.h"

#include <zlib.h>
#include <zstd.h>

#include <cstring>

namespace gorz {

std::vector<uint8_t> inflate_zlib(const uint8_t* src, std::size_t srcLen,
                                  std::size_t hintUncompressedLen) {
    std::vector<uint8_t> out;
    out.resize(hintUncompressedLen);

    z_stream strm{};
    strm.next_in = const_cast<Bytef*>(src);
    strm.avail_in = static_cast<uInt>(srcLen);

    // windowBits = 15 → standard zlib wrapper. The Java writer uses
    // `new Deflater(level)` which defaults to nowrap=false, so the on-disk
    // bytes carry the 2-byte zlib header + Adler32 checksum.
    int rc = inflateInit2(&strm, 15);
    if (rc != Z_OK) {
        throw InflateError("inflateInit2 failed: " + std::to_string(rc));
    }

    std::size_t outPos = 0;
    while (true) {
        strm.next_out = out.data() + outPos;
        strm.avail_out = static_cast<uInt>(out.size() - outPos);

        rc = inflate(&strm, Z_NO_FLUSH);
        outPos = out.size() - strm.avail_out;

        if (rc == Z_STREAM_END) {
            inflateEnd(&strm);
            out.resize(outPos);
            return out;
        }
        if (rc != Z_OK) {
            inflateEnd(&strm);
            throw InflateError("inflate failed: " + std::to_string(rc)
                + (strm.msg ? std::string(": ") + strm.msg : std::string()));
        }
        if (strm.avail_out == 0) {
            // Need more output space. Grow.
            std::size_t newCap = out.size() * 2;
            out.resize(newCap);
        }
    }
}

std::vector<uint8_t> inflate_zstd(const uint8_t* src, std::size_t srcLen) {
    // Two frame shapes appear in real .gorz files:
    //   - sized: the writer flushed `ZSTD_compress(...)` which records the
    //     uncompressed size in the frame header. ZSTD_decompress() handles
    //     this in one shot.
    //   - streaming: Java's `ZstdOutputStream` emits the streaming form
    //     (no content-size header). We have to use the streaming decoder
    //     for those.
    // ZSTD_decompress works for both shapes when the dst is large enough,
    // but for streaming frames we don't know the size up front. The
    // streaming API handles both transparently — use it unconditionally.
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx) throw InflateError("zstd: ZSTD_createDCtx failed");

    // Start with a reasonable guess; grow as needed.
    std::vector<uint8_t> out;
    out.resize(srcLen * 4 + 64);

    ZSTD_inBuffer in_{src, srcLen, 0};
    std::size_t produced = 0;
    while (true) {
        ZSTD_outBuffer outBuf{out.data() + produced, out.size() - produced, 0};
        std::size_t const ret = ZSTD_decompressStream(dctx, &outBuf, &in_);
        produced += outBuf.pos;
        if (ZSTD_isError(ret)) {
            std::string msg = ZSTD_getErrorName(ret);
            ZSTD_freeDCtx(dctx);
            throw InflateError(std::string("zstd: ") + msg);
        }
        if (ret == 0) break;          // frame fully consumed
        if (in_.pos >= in_.size) {
            // Frame asked for more input than we have — malformed.
            ZSTD_freeDCtx(dctx);
            throw InflateError("zstd: truncated frame");
        }
        if (outBuf.pos == outBuf.size) {
            // Output buffer full — grow and continue.
            out.resize(out.size() * 2);
        }
    }
    ZSTD_freeDCtx(dctx);
    out.resize(produced);
    return out;
}

std::vector<uint8_t> deflate_zlib(const uint8_t* src, std::size_t srcLen, int level) {
    z_stream strm{};
    if (deflateInit2(&strm, level, Z_DEFLATED, /*windowBits=*/15,
                     /*memLevel=*/8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw InflateError("deflateInit2 failed");
    }
    strm.next_in = const_cast<Bytef*>(src);
    strm.avail_in = static_cast<uInt>(srcLen);

    std::vector<uint8_t> out;
    out.resize(std::max<std::size_t>(64, srcLen / 4 + 64));
    std::size_t produced = 0;
    while (true) {
        strm.next_out = out.data() + produced;
        strm.avail_out = static_cast<uInt>(out.size() - produced);
        int rc = deflate(&strm, Z_FINISH);
        produced = out.size() - strm.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            deflateEnd(&strm);
            throw InflateError("deflate failed: " + std::to_string(rc));
        }
        // Grow output if the codec is asking for room.
        out.resize(out.size() * 2);
    }
    deflateEnd(&strm);
    out.resize(produced);
    return out;
}

}  // namespace gorz
