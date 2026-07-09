#include "gorz/Gord.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

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
        e.path = resolvePath(fields[0], dictParent);
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
        // Tags: the trailing 7th column (comma-separated bucket members) when
        // present, otherwise the single alias. GOR treats col 1 as a tag, so a
        // plain entry contributes exactly its alias.
        if (fields.size() == 7 && !fields[6].empty()) {
            std::size_t start = 0;
            for (std::size_t i = 0; i <= fields[6].size(); ++i) {
                if (i == fields[6].size() || fields[6][i] == ',') {
                    if (i > start) e.tags.emplace_back(fields[6].substr(start, i - start));
                    start = i + 1;
                }
            }
            e.isBucket = e.tags.size() > 1;  // >1 member → rows carry a source col
            if (e.tags.empty() && !alias.empty()) e.tags.push_back(alias);
        } else if (!alias.empty()) {
            e.tags.push_back(alias);
        }
        if (!e.tags.empty()) g.hasTags = true;
        g.entries.push_back(std::move(e));
    }
    return g;
}

}  // namespace gorz
