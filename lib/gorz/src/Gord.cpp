#include "gorz/Gord.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace gorz {

namespace {

// Tab-split with field count check. Re-uses the returned vector across
// calls would matter if this were hot — it isn't (called once per entry
// at parse time).
std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == '\t') {
            out.emplace_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

// Trim trailing CR (Windows line endings) — getline on a CRLF file
// leaves a trailing '\r' in the line.
void rstripCR(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
}

// Resolve a path against `dictParent` if relative; pass through if absolute.
std::string resolvePath(const std::string& raw, const std::string& dictParent) {
    namespace fs = std::filesystem;
    fs::path p(raw);
    if (p.is_absolute()) return raw;
    return (fs::path(dictParent) / p).lexically_normal().string();
}

// Split the file column into (source, bucket, isDeleted). GOR forms:
//   <src>              -> {src, "", false}
//   <src>|<bucket>     -> {src, bucket, false}
//   <src>|D|<bucket>   -> {src, bucket, true}   (D case-insensitive)
struct FileColumn {
    std::string source;
    std::string bucket;  // empty when un-bucketized
    bool deleted = false;
};
FileColumn splitFileColumn(const std::string& col0) {
    FileColumn r;
    std::size_t pipe = col0.find('|');
    if (pipe == std::string::npos) {
        r.source = col0;
        return r;
    }
    r.source = col0.substr(0, pipe);
    std::string rest = col0.substr(pipe + 1);
    if (rest.size() >= 2 && (rest[0] == 'D' || rest[0] == 'd') && rest[1] == '|') {
        r.deleted = true;
        r.bucket = rest.substr(2);
    } else {
        r.bucket = rest;
    }
    return r;
}

// Extend a bucket's union range by one member entry's declared range.
void extendBucketRange(BucketInfo& b, const GordEntry& e) {
    if (!e.hasRange) return;
    if (!b.hasRange) {
        b.hasRange = true;
        b.chromStart = e.chromStart;
        b.posStart = e.posStart;
        b.chromEnd = e.chromEnd;
        b.posEnd = e.posEnd;
        return;
    }
    if (e.chromStart < b.chromStart ||
        (e.chromStart == b.chromStart && e.posStart < b.posStart)) {
        b.chromStart = e.chromStart;
        b.posStart = e.posStart;
    }
    if (e.chromEnd > b.chromEnd ||
        (e.chromEnd == b.chromEnd && e.posEnd > b.posEnd)) {
        b.chromEnd = e.chromEnd;
        b.posEnd = e.posEnd;
    }
}

// Parse a "## KEY = VALUE" header line into (key, value). Whitespace
// around '=' is tolerated. Returns false for non-header lines.
bool parseHeader(const std::string& line, std::string& key, std::string& value) {
    if (line.size() < 3 || line[0] != '#' || line[1] != '#') return false;
    std::size_t i = 2;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    std::size_t keyStart = i;
    while (i < line.size() && line[i] != '=' && line[i] != ' ' && line[i] != '\t') ++i;
    if (i == keyStart) return false;
    key = line.substr(keyStart, i - keyStart);
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i == line.size() || line[i] != '=') return false;
    ++i;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    value = line.substr(i);
    return true;
}

}  // namespace

Gord parseGordFile(const std::string& path, const Opener& opener) {
    namespace fs = std::filesystem;
    auto inPtr = opener(path);
    if (!inPtr) {
        throw std::runtime_error("cannot open .gord: " + path);
    }
    std::istream& in = *inPtr;
    const std::string dictParent =
        fs::path(path).parent_path().string();

    Gord g;
    g.dictPath = path;

    std::string line;
    std::size_t lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        rstripCR(line);
        if (line.empty()) continue;
        if (line[0] == '#') {
            // ## COLUMNS = a,b,c... → split comma-separated names
            std::string key, value;
            if (parseHeader(line, key, value) && key == "COLUMNS") {
                g.columns.clear();
                std::string cur;
                for (char c : value) {
                    if (c == ',') {
                        if (!cur.empty()) g.columns.push_back(cur);
                        cur.clear();
                    } else {
                        cur.push_back(c);
                    }
                }
                if (!cur.empty()) g.columns.push_back(cur);
            }
            continue;
        }
        auto fields = splitTabs(line);
        // Supported forms:
        //   2 cols: <file> <alias>                                  (no range)
        //   6 cols: <file> <alias> <cStart> <pStart> <cEnd> <pEnd>
        //   7 cols: 6-col + trailing comma-separated bucket tag list
        if (fields.size() != 2 && fields.size() != 6 && fields.size() != 7) {
            throw std::runtime_error(
                "GORD line " + std::to_string(lineNo) +
                ": expected 2, 6 or 7 tab-separated columns "
                "(<file> <alias> [<cStart> <pStart> <cEnd> <pEnd> [<tags>]]), got " +
                std::to_string(fields.size()));
        }
        GordEntry e;
        // Col 0 may carry `<src>|<bucket>` or `<src>|D|<bucket>`.
        FileColumn fc = splitFileColumn(fields[0]);
        e.path = resolvePath(fc.source, dictParent);
        e.isDeleted = fc.deleted;
        if (!fc.bucket.empty()) {
            e.bucketPath = resolvePath(fc.bucket, dictParent);
        }
        const std::string& alias = fields[1];
        if (fields.size() >= 6) {
            e.hasRange = true;
            e.chromStart = fields[2];
            try {
                e.posStart = std::stoll(fields[3]);
                e.chromEnd = fields[4];
                e.posEnd = std::stoll(fields[5]);
            } catch (const std::exception&) {
                throw std::runtime_error(
                    "GORD line " + std::to_string(lineNo) +
                    ": non-numeric pos_start / pos_end");
            }
        }
        // Tags: the trailing 7th column (legacy comma-separated inline bucket
        // members) when present, otherwise the single alias. GOR treats col 1
        // as a tag, so a plain entry contributes exactly its alias.
        if (fields.size() == 7 && !fields[6].empty()) {
            std::size_t start = 0;
            for (std::size_t i = 0; i <= fields[6].size(); ++i) {
                if (i == fields[6].size() || fields[6][i] == ',') {
                    if (i > start) e.tags.emplace_back(fields[6].substr(start, i - start));
                    start = i + 1;
                }
            }
            // Legacy inline-list bucket: >1 member → rows carry a source col.
            e.sourceInserted = e.tags.size() > 1;
            if (e.tags.empty() && !alias.empty()) e.tags.push_back(alias);
        } else if (!alias.empty()) {
            e.tags.push_back(alias);
        }
        if (!e.tags.empty()) g.hasTags = true;
        g.entries.push_back(std::move(e));
    }

    // Second pass: build the bucket index + valid-tag set from the parsed lines.
    for (std::size_t i = 0; i < g.entries.size(); ++i) {
        const GordEntry& e = g.entries[i];
        if (!e.isDeleted) {
            for (const auto& t : e.tags) g.validTags.insert(t);
        }
        if (e.bucketPath.empty()) continue;
        BucketInfo& b = g.buckets[e.bucketPath];
        b.path = e.bucketPath;
        for (const auto& t : e.tags) {
            b.memberTags.insert(t);
            if (e.isDeleted) {
                b.deletedTags.insert(t);
                g.anyBucketHasDeletedFile = true;
            }
        }
        if (!e.isDeleted) {
            b.memberEntryIdxs.push_back(i);
            extendBucketRange(b, e);
        }
    }
    return g;
}

namespace {

// Env override matching GOR's System.getProperty knobs (accept the GOR_… and
// gor.… spellings). Returns `dflt` when unset / unparseable.
int envInt(const char* upper, const char* dotted, int dflt) {
    const char* v = std::getenv(upper);
    if (!v) {
        // dotted form isn't a legal env var name, but honour it if exported.
        v = std::getenv(dotted);
    }
    if (!v || !*v) return dflt;
    try {
        return std::stoi(v);
    } catch (...) {
        return dflt;
    }
}

}  // namespace

std::vector<GordEntry> optimizedFileList(const Gord& gord,
                                         const std::unordered_set<std::string>& requested) {
    // 1. Collect matching *active* entries (tag ∈ requested, not deleted) and,
    //    per bucket, count how many are requested (used) vs total members.
    std::vector<std::size_t> matching;  // indices into gord.entries
    std::vector<std::string> bucketPaths;
    std::unordered_map<std::string, std::size_t> bucketToIdx;
    std::vector<int> bucketTotal, bucketUsed;
    std::size_t numFilesWithoutBucket = 0;

    auto anyRequested = [&](const std::vector<std::string>& tags) {
        for (const auto& t : tags) {
            if (requested.count(t)) return true;
        }
        return false;
    };

    for (std::size_t i = 0; i < gord.entries.size(); ++i) {
        const GordEntry& e = gord.entries[i];
        if (e.isDeleted) continue;                 // deleted → never read individually
        if (!anyRequested(e.tags)) continue;       // NO_TAG → skip this file
        matching.push_back(i);
        if (e.bucketPath.empty()) {
            ++numFilesWithoutBucket;
            continue;
        }
        auto it = bucketToIdx.find(e.bucketPath);
        std::size_t bi;
        if (it == bucketToIdx.end()) {
            bi = bucketPaths.size();
            bucketToIdx.emplace(e.bucketPath, bi);
            bucketPaths.push_back(e.bucketPath);
            bucketTotal.push_back(static_cast<int>(gord.buckets.at(e.bucketPath).memberTags.size()));
            bucketUsed.push_back(0);
        } else {
            bi = it->second;
        }
        ++bucketUsed[bi];
    }

    const std::size_t numBuckets = bucketPaths.size();

    // 2. Threshold-halving selection loop (GOR getOptimizedFileList).
    std::vector<bool> include(numBuckets, true);   // read this bucket's files individually
    std::vector<bool> replace(numBuckets, false);  // read the bucket file instead
    int totalFileReads = static_cast<int>(matching.size());
    int numBucketsToBeAccessed = 0;
    const int FILE_COUNT_THRESHOLD =
        envInt("GOR_BUCKET_FILE_COUNT_THRESHOLD", "gor.bucket.file.count.threshold", 100);
    float threshold =
        envInt("GOR_BUCKET_INITIAL_USAGE_THRESHOLD", "gor.bucket.initial.usage.threshold", 40) / 100.0f;
    const int singleFilesBucketCountThresholdRatio = 10;
    const int minNumberOfFilesToAccess =
        static_cast<int>(numFilesWithoutBucket) + static_cast<int>(numBuckets);

    do {
        for (std::size_t i = 0; i < numBuckets; ++i) {
            int count = bucketUsed[i];
            if (include[i] && static_cast<float>(count) / bucketTotal[i] > threshold && count != 1) {
                include[i] = false;
                replace[i] = true;
                totalFileReads -= (count - 1);
                ++numBucketsToBeAccessed;
            }
        }
        threshold /= 2;
    } while (totalFileReads > FILE_COUNT_THRESHOLD &&
             totalFileReads > minNumberOfFilesToAccess &&
             totalFileReads > (singleFilesBucketCountThresholdRatio + 1) * numBucketsToBeAccessed);

    // 3. Rebuild the file list: bucket entry (once) for a replaced bucket,
    //    individual file for an included one, skip files of a replaced bucket.
    std::vector<GordEntry> out;
    out.reserve(matching.size());
    for (std::size_t fi : matching) {
        const GordEntry& e = gord.entries[fi];
        if (!e.bucketPath.empty()) {
            std::size_t bi = bucketToIdx.at(e.bucketPath);
            if (replace[bi]) {
                replace[bi] = false;  // emit the synthetic bucket entry just once
                const BucketInfo& b = gord.buckets.at(e.bucketPath);
                GordEntry be;
                be.path = b.path;
                be.sourceInserted = true;
                be.tags.assign(b.memberTags.begin(), b.memberTags.end());
                be.deletedTags = b.deletedTags;
                be.hasRange = b.hasRange;
                be.chromStart = b.chromStart;
                be.posStart = b.posStart;
                be.chromEnd = b.chromEnd;
                be.posEnd = b.posEnd;
                out.push_back(std::move(be));
                continue;
            }
            if (!include[bi]) continue;  // bucket read elsewhere → drop this file
        }
        out.push_back(e);  // individual source file (or un-bucketized)
    }
    return out;
}

}  // namespace gorz
