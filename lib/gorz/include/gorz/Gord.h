#pragma once

// Parser for a flat (file-form) GOR dictionary. PGOR-produced cache
// dictionaries and `WRITE -d` flat-file output land here.
//
// Column 0 (the file column) may carry two-level bucketization info via pipes:
//     <sourcefile>                 plain / un-bucketized
//     <sourcefile>|<bucketfile>    source's rows are also stored in <bucketfile>
//     <sourcefile>|D|<bucketfile>  source is DELETED from the bucket (stale rows)
// Multiple lines can share one <bucketfile> — they were bucketized together. The
// reader can then open the bucket once (row-filtered by tag) instead of each
// source file. `partition-id` (col 1) is the entry's tag/alias.
//
// Folder variants (`thedict.gord` inside a `.gord/` directory) and versioned
// `.gord.link` remain out of scope.
//
// Paths are absolute when they start with '/', otherwise they resolve against
// the `.gord` file's parent directory.

#include "gorz/Io.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gorz {

struct GordEntry {
    std::string path;        // resolved .gorz path. For a plain/source line this
                             // is the source file; for a *selected bucket* entry
                             // (built during optimisation) it's the bucket file.
    std::string chromStart;  // empty when the line carries no declared range
    int64_t posStart = 0;
    std::string chromEnd;    // empty when the line carries no declared range
    int64_t posEnd = 0;
    bool hasRange = false;   // false → range columns absent (2-col form)
    // Tags this entry's rows can belong to (GOR's -f/-ff filtering key). For a
    // plain/source line it's the single alias (col 1); for a selected bucket
    // entry it's the bucket's member tags that were actually requested. Empty
    // when the line has no alias/tags.
    std::vector<std::string> tags;
    // ---- bucketization (real GOR `file|bucket` / `file|D|bucket` form) ----
    // The bucket file this source line was bucketized into (parsed from the
    // pipe suffix in col 0); empty when the line is not bucketized.
    std::string bucketPath;
    // The `|D|` delete marker: this source's rows are stale in the bucket and
    // must be filtered out. A deleted line contributes its tag to the bucket's
    // deleted-tag set but is never read as an individual file.
    bool isDeleted = false;
    // True when this entry is read AS A BUCKET: its rows already carry a
    // trailing `Source` column identifying each row's origin tag, so a per-row
    // hash filter (GOR's POSSIBLE_TAG) applies. A primary (sourceInserted=false)
    // has no such column, so GordReader appends its tag to match the bucket
    // width (see GordReader OpenIterator::appendSource). Set for a selected
    // bucket entry, and for the legacy 7-col inline-tag-list form.
    bool sourceInserted = false;
    // For a selected bucket entry: tags marked `|D|` for this bucket, so their
    // (stale) rows are dropped even if the caller's -f names them.
    std::unordered_set<std::string> deletedTags;
};

// Aggregate of all source lines that share one bucket file. Built at parse
// time; consumed by the bucket-vs-primary selection heuristic.
struct BucketInfo {
    std::string path;                              // resolved bucket .gorz path
    std::unordered_set<std::string> memberTags;    // every tag mapped here (incl. deleted)
    std::unordered_set<std::string> deletedTags;   // tags marked `|D|` for this bucket
    std::vector<std::size_t> memberEntryIdxs;      // entries[] indices of live (non-deleted) source lines
    // Union of member ranges, for the synthetic bucket entry's candidate key.
    std::string chromStart;
    int64_t posStart = 0;
    std::string chromEnd;
    int64_t posEnd = 0;
    bool hasRange = false;
};

struct Gord {
    std::string dictPath;            // path to the .gord file itself
    std::vector<GordEntry> entries;  // in dictionary order (input)
    // ## COLUMNS = a,b,c from the header. May be empty if not present —
    // the GordReader falls back to the first entry's .gorz header in
    // that case.
    std::vector<std::string> columns;
    bool hasTags = false;            // any entry carries a tag/alias
    // Bucket index (keyed by resolved bucket path). Empty when the dictionary
    // has no `file|bucket` lines.
    std::unordered_map<std::string, BucketInfo> buckets;
    // All non-deleted tags in the dictionary — the implicit requested-tag set
    // for a full scan, and the `force-all-tags` set when deletes are present
    // but no -f/-ff was given.
    std::unordered_set<std::string> validTags;
    bool anyBucketHasDeletedFile = false;
    // Source-column name carried by the dictionary itself: from a
    // `## SOURCE_COLUMN = <name>` metadata line, else the 2nd column of a
    // single-`#` header line (`#file\t<name>\t...`). Empty when neither is
    // present. The final name resolves (in the driver) as:
    //   explicit -s / source param  >  this  >  <dict>.meta SOURCE_COLUMN  >  "Source".
    std::string sourceColumnName;
};

// Parse a flat GORD file. Accepts the GOR dictionary forms we support:
//   * 2 cols: <file> <alias>                                  (real GOR dicts)
//   * 6 cols: <file> <alias> <cStart> <pStart> <cEnd> <pEnd>  (with range)
//   * 7 cols: 6-col form + a trailing comma-separated tag list (bucket)
// col 1 (alias) is the entry's tag; the optional 7th column carries a bucket's
// member tags. Throws on: file missing / unreadable; an unsupported column
// count; non-numeric pos_start / pos_end.
// `opener` routes the open (default: local std::ifstream).
Gord parseGordFile(const std::string& path, const Opener& opener = defaultOpener());

// Bucket-vs-primary selection — GOR's getOptimizedFileList. Given the set of
// requested tags (an explicit -f/-ff set, or the dictionary's validTags for a
// full scan), return the effective entry list: individual source entries for
// tags whose bucket isn't worth opening, plus one synthetic sourceInserted
// bucket entry per bucket the heuristic chose (covering many tags in one open).
// Deleted source lines are never emitted as individual files; a selected bucket
// entry carries the bucket's deletedTags so its stale rows are row-filtered out.
// Non-bucketized dictionaries pass through unchanged (tag-pruned).
//
// The heuristic (initial 40% usage threshold, halved iteratively; 100-file cap;
// 10:1 single-to-bucket ratio) is overridable via the same env vars GOR uses:
//   GOR_BUCKET_FILE_COUNT_THRESHOLD / gor.bucket.file.count.threshold (100)
//   GOR_BUCKET_INITIAL_USAGE_THRESHOLD / gor.bucket.initial.usage.threshold (40)
std::vector<GordEntry> optimizedFileList(const Gord& gord,
                                         const std::unordered_set<std::string>& requestedTags);

}  // namespace gorz
