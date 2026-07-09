#include "gorz/MetaFile.h"

#include <fstream>
#include <stdexcept>
#include <string>

namespace gorz {

namespace {

// Trim leading/trailing ASCII whitespace.
std::string trim(std::string s) {
    auto isws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && isws(s.back())) s.pop_back();
    std::size_t lead = 0;
    while (lead < s.size() && isws(s[lead])) ++lead;
    return s.substr(lead);
}

GorColumnType parseColumnType(char c) {
    switch (c) {
        case 'S': case 's': return GorColumnType::String;
        case 'I': case 'i': return GorColumnType::Integer;
        case 'L': case 'l': return GorColumnType::Long;
        case 'D': case 'd': return GorColumnType::Double;
        default:
            throw std::runtime_error(
                std::string("unknown SCHEMA letter '") + c +
                "': expected one of S, I, L, D");
    }
}

std::vector<std::optional<GorColumnType>> parseSchema(const std::string& csv) {
    std::vector<std::optional<GorColumnType>> out;
    auto pushOne = [&](std::string cur) {
        cur = trim(cur);
        if (cur.empty()) return;
        // `null` means "WRITE didn't infer this column's type" (no
        // -inferschema flag). Push std::nullopt — the extension's
        // resolveColumnType then applies the gor convention for that
        // column (col 1 → INTEGER, rest VARCHAR).
        if (cur == "null" || cur == "NULL" || cur == "Null") {
            out.emplace_back(std::nullopt);
        } else {
            out.emplace_back(parseColumnType(cur[0]));
        }
    };
    std::string cur;
    for (char c : csv) {
        if (c == ',') { pushOne(cur); cur.clear(); }
        else { cur.push_back(c); }
    }
    pushOne(cur);
    return out;
}

}  // namespace

GorMeta parseMetaFile(const std::string& path, const Opener& opener) {
    GorMeta meta;
    auto inPtr = opener(path);
    if (!inPtr) return meta;  // missing → empty GorMeta is fine
    std::istream& in = *inPtr;

    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 3 || line[0] != '#' || line[1] != '#') continue;
        // strip "## "
        std::size_t pos = 2;
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
        std::string body = line.substr(pos);
        auto eq = body.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(body.substr(0, eq));
        std::string value = trim(body.substr(eq + 1));
        if (key == "SCHEMA") {
            meta.schema = parseSchema(value);
        } else if (key == "LINE_COUNT") {
            try {
                meta.lineCount = std::stoll(value);
            } catch (...) {
                // ignore malformed line count
            }
        }
        // Other keys (QUERY, COLUMNS, MD5, RANGE, TAGS) ignored — not needed
        // for the extension's bind/execute path. RANGE will come in phase 4
        // for seek pushdown when the .gori isn't present.
    }
    return meta;
}

}  // namespace gorz
