#pragma once

// Parser for a flat (file-form) GOR dictionary. PGOR-produced cache
// dictionaries and `WRITE -d` flat-file output land here. Folder variants
// (`thedict.gord` inside a `.gord/` directory), versioned `.gord.link`,
// tag columns, pipe-separated bucket/flag info, and deletion markers are
// **out of scope for phase 3.5** — they all become switch-arm additions
// once we have a stable simple-GORD pipeline.
//
// Line shape (6 tab-separated columns; lines starting with '#' are
// metadata, skip):
//
//     <gorz-path>\t<partition-id>\t<chrom_start>\t<pos_start>\t<chrom_end>\t<pos_end>\n
//
// `gorz-path` is taken absolute when it starts with '/', otherwise it
// resolves against the `.gord` file's parent directory.
// `partition-id` (col 1) is ignored at this phase — it's a per-partition
// counter; we don't need it for ordering or filtering.

#include "gorz/Io.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gorz {

struct GordEntry {
    std::string path;        // resolved absolute path to the .gorz
    std::string chromStart;  // empty when the line carries no declared range
    int64_t posStart = 0;
    std::string chromEnd;    // empty when the line carries no declared range
    int64_t posEnd = 0;
    bool hasRange = false;   // false → range columns absent (2-col form)
    // Tags this entry's rows can belong to (GOR's -f/-ff filtering key). For a
    // plain entry it's the single alias (col 1); for a bucket it's the members
    // of the trailing tag-list column. Empty when the line has no alias/tags.
    std::vector<std::string> tags;
    // True when a trailing comma-separated tag-list column was present — i.e.
    // the file is a *bucket* holding rows for several tags, distinguished by a
    // trailing source column (needs row-level filtering, GOR's POSSIBLE_TAG).
    bool isBucket = false;
};

struct Gord {
    std::string dictPath;            // path to the .gord file itself
    std::vector<GordEntry> entries;  // in dictionary order (input)
    // ## COLUMNS = a,b,c from the header. May be empty if not present —
    // the GordReader falls back to the first entry's .gorz header in
    // that case.
    std::vector<std::string> columns;
    bool hasTags = false;            // any entry carries a tag/alias
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

}  // namespace gorz
