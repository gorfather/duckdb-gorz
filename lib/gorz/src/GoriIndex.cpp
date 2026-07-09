#include "gorz/GoriIndex.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

namespace gorz {

namespace {

// String-then-int compare so (chr1, 200) < (chr1, 480) and
// (chr1, 999999) < (chr2, 1). Matches the gorz writer's row ordering.
int cmpKey(const std::string& aChr, int64_t aPos,
           const std::string& bChr, int64_t bPos) {
    int c = aChr.compare(bChr);
    if (c != 0) return c;
    if (aPos < bPos) return -1;
    if (aPos > bPos) return 1;
    return 0;
}

}  // namespace

GoriIndex parseGoriIndex(const std::string& gorzPath, const Opener& opener) {
    GoriIndex idx;
    auto inPtr = opener(gorzPath + ".gori");
    if (!inPtr) return idx;
    std::istream& in = *inPtr;

    std::string line;
    bool sawHeader = false;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') {
            // ## fileformat=GORIv2 — currently the only header we support.
            // GORIv1 is documented as "ignore" in the Java reader; treat
            // the index as empty (so seek falls back to scan).
            if (line.find("GORIv1") != std::string::npos) {
                return GoriIndex{};
            }
            sawHeader = true;
            continue;
        }
        // <chrom>\t<pos>\t<offset>
        std::size_t tab1 = line.find('\t');
        std::size_t tab2 = line.find('\t', tab1 + 1);
        if (tab1 == std::string::npos || tab2 == std::string::npos) {
            throw std::runtime_error(
                "malformed .gori line (need 3 tab-separated fields): " + line);
        }
        GoriEntry e;
        e.chrom = line.substr(0, tab1);
        try {
            e.pos = std::stoll(line.substr(tab1 + 1, tab2 - tab1 - 1));
            e.offset = std::stoll(line.substr(tab2 + 1));
        } catch (const std::exception& ex) {
            throw std::runtime_error(
                std::string("malformed .gori numeric field: ") + line);
        }
        idx.entries.push_back(std::move(e));
    }
    (void)sawHeader;
    return idx;
}

std::optional<int64_t> findBlockStart(const GoriIndex& idx,
                                      const std::string& chrom, int64_t pos,
                                      int64_t headerEndOffset) {
    if (idx.entries.empty()) return std::nullopt;

    // lower_bound: first entry where (entry.chrom, entry.pos) >= (chrom, pos).
    auto it = std::lower_bound(
        idx.entries.begin(), idx.entries.end(), GoriEntry{chrom, pos, 0},
        [](const GoriEntry& a, const GoriEntry& b) {
            return cmpKey(a.chrom, a.pos, b.chrom, b.pos) < 0;
        });

    if (it == idx.entries.end()) {
        // Past the last block. Caller can EOF.
        return std::nullopt;
    }
    if (it == idx.entries.begin()) {
        // The first block (whose LAST row is *it) contains (chrom, pos).
        // The first block starts right after the file header.
        return headerEndOffset;
    }
    // Otherwise the block we want is the one whose previous entry's offset
    // is its start.
    return (it - 1)->offset;
}

}  // namespace gorz
