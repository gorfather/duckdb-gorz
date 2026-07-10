#define DUCKDB_EXTENSION_MAIN

// gorz DuckDB extension — read/write GORpipe .gorz files and .gord dictionaries
// as native DuckDB tables. Packaged as a DuckDB community extension
// (INSTALL gorz FROM community; LOAD gorz;).
//
// Registers at LOAD time:
//   * read_gor / read_gorz / read_gord table functions (projection pushdown,
//     parallel scan, f/ff partition filters, range := 'chrN:a-b' hard filter,
//     WHERE->block-seek pushdown).
//   * a replacement scan rewriting 'foo.gorz' / 'foo.gord' literals to
//     read_gor(...), like DuckDB's .csv / .parquet auto-detect.
//   * COPY TO (FORMAT gorz | gor) write functions.

// DuckDB header forward-declares BoundFunctionExpression in several
// places and then uses unique_ptr<BoundFunctionExpression> in inline
// destructors. We pull the full definition in up front so libc++'s
// default_delete instantiation has a complete type to delete.
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"

#include "gorz_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

#include "gorz/Gord.h"
#include "gorz/GordReader.h"
#include "gorz/MetaFile.h"
#include "gorz/Reader.h"
#include "gorz/Writer.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string>

namespace duckdb {

// Holds the file path + header parsed at bind time. Lives for the duration
// of the bound logical plan. We re-open the file in init_global because
// FunctionData has to be copyable / serialisable and an open ifstream isn't.
// Hint extracted from the planner-level filter expressions in our
// `pushdown_complex_filter` callback. Used by init_global to seek to the
// right block in the .gorz / .gord; ignored by Execute (DuckDB's post-scan
// filter operator does the actual row-level filtering, so correctness is
// preserved regardless of what the seek hint says).
//
// Empty chrom = no seek (no chrom equality in the predicate). posLo /
// posHi default to the unbounded sentinels; init_global decides what to
// do when only one bound is present.
struct SeekHint {
	std::string chrom;
	bool haveChrom = false;
	int64_t posLo = std::numeric_limits<int64_t>::min();
	int64_t posHi = std::numeric_limits<int64_t>::max();
	bool haveLo = false;
	bool haveHi = false;
};

// Which concrete reader a bound query uses. read_gor dispatches on the file
// extension; read_gorz / read_gord force the kind (see the bind shims).
enum class GorKind { GORZ, GORD };

// Unified bind data for read_gor / read_gorz / read_gord. Lives for the bound
// logical plan. The reader is (re)opened in init_global because FunctionData
// must be copyable / serialisable and an open stream isn't.
struct GorBindData : public TableFunctionData {
	GorKind kind = GorKind::GORZ;
	std::string path;
	std::vector<std::string> columnNames;
	std::vector<LogicalType> columnTypes; // VARCHAR if no .meta SCHEMA
	// Mutable so pushdown_complex_filter can stash the seek hint on it
	// before init_global runs. DuckDB's API passes bind_data as
	// FunctionData* (non-const) to the callback; we treat it as the
	// designated handoff slot rather than introducing a side channel.
	mutable SeekHint seek;
	// GOR -f / -ff partition filter: the union of the requested tags. Empty →
	// no tag filtering. Only valid for the GORD kind (validated at bind).
	std::vector<std::string> tagFilter;
	// GOR -p style HARD range from the `range` named parameter
	// ('chrN' | 'chrN:start-end' | 'chrN:start-' | 'chrN:pos'). Unlike `seek`
	// (a soft pushdown hint that DuckDB re-checks), this bounds which rows the
	// scan emits. Intersected with `seek` in init_global (see effectiveSeek).
	bool haveRangeParam = false;
	SeekHint rangeParam;
	// Explicit source-column name from the `source` named parameter (GOR's -s).
	// Empty → resolve from the dictionary (## SOURCE_COLUMN / header) then
	// "Source". Only meaningful for the GORD kind.
	std::string sourceName;
};

// One-per-query state: holds the open file + reader. DuckDB calls Execute
// repeatedly into this until the reader hits EOF. Only the fields for the
// bound kind are populated (GORZ → in/reader + range state; GORD → gordReader).
struct GorGlobalState : public GlobalTableFunctionState {
	// GORZ: generic istream so `in` can hold either a std::ifstream (CLI/tests)
	// or the DuckDB FileSystem-backed stream (see openViaFileSystem) that lets
	// read_gorz read from local FS *and* object stores (S3/GCS via httpfs).
	std::unique_ptr<std::istream> in;
	std::unique_ptr<gorz::Reader> reader;
	bool exhausted = false;
	// GORZ range short-circuit (from the seek hint). When rangeActive is set,
	// Execute skips rows outside the window and stops once a row is strictly
	// past it (rows are (chrom,pos)-sorted). GORD does its own range filtering
	// inside GordReader, so these stay unset for the GORD kind.
	bool rangeActive = false;
	bool haveChromEq = false;
	std::string chromEq;
	int64_t posLo = std::numeric_limits<int64_t>::min();
	int64_t posHi = std::numeric_limits<int64_t>::max();
	// Projection pushdown: the original column indices DuckDB actually wants,
	// in output order (from TableFunctionInitInput::column_ids). Execute only
	// materialises these, skipping the from_chars / string-alloc for the rest.
	std::vector<column_t> projection;

	// ---- GORD parallel read (Part 3) ----------------------------------------
	// The surviving entries (after range + tag pruning) are partitioned into
	// range-contiguous thread groups; each parallel local state claims groups
	// from `nextGroup` and runs a single-threaded GordReader over its group's
	// files. Only populated for the GORD kind.
	GorKind kind = GorKind::GORZ;
	std::vector<std::vector<gorz::GordEntry>> groups;
	std::atomic<idx_t> nextGroup {0};
	// Context for building a per-group GordReader (same filters as the plan).
	gorz::Opener opener;
	std::vector<std::string> gordColumns;
	bool gordHasTags = false;
	bool gordHaveRange = false;
	std::string gordChrom;
	int64_t gordPosLo = 0;
	int64_t gordPosHi = 0;
	std::vector<std::string> gordTags;

	// ---- GORZ full-scan parallel read ---------------------------------------
	// A single large .gorz is split into byte ranges (one .gorz block is a
	// '\n'-delimited record, so this is the CSV-style byte-range split); each
	// parallel local state claims a range from `nextGroup` and reads it with a
	// bounded gorz::Reader. Only populated for the GORZ kind on a full scan
	// (no seek hint) of a file large enough to be worth it — a range/seek
	// query keeps the single-threaded seek path (`reader` above).
	bool gorzParallel = false;
	std::string gorzPath;
	std::vector<std::pair<int64_t, int64_t>> byteRanges;

	idx_t MaxThreads() const override {
		if (kind == GorKind::GORD)
			return std::max<idx_t>(groups.size(), 1);
		if (gorzParallel)
			return std::max<idx_t>(byteRanges.size(), 1);
		return 1; // GORZ seek/range query or small file → single-threaded
	}
};

// Per-thread state. GORD: a reader over the claimed dictionary group. GORZ
// full-scan parallel: an istream + bounded reader over the claimed byte range.
// (The GORZ single-threaded seek path reads from the global reader instead.)
struct GorLocalState : public LocalTableFunctionState {
	std::unique_ptr<gorz::GordReader> reader;
	std::unique_ptr<std::istream> gorzIn;
	std::unique_ptr<gorz::Reader> gorzReader;
};

namespace {

// Resolve a user-supplied file path against DuckDB's `file_search_path`
// setting when relative. DuckDB's own file resolution (e.g. for
// 'foo.parquet') walks this comma-separated list, but my read_gorz /
// read_gord open the file via std::ifstream which is CWD-relative.
// Without this helper, `select * from 'cache/38/foo.gord'` fails inside
// gormore (where the worker JVM's CWD isn't the project root) even
// though SET file_search_path='/project/root' was honoured for normal
// DuckDB file reads.
//
// Returns the path unchanged when it's already absolute, or when no
// file_search_path entry resolves to an existing file. parseGordFile /
// std::ifstream then handle the absolute path (or surface a clean
// "cannot open" error).
std::string resolvePathViaFileSearch(ClientContext &context, const std::string &p) {
	namespace fs = std::filesystem;
	if (fs::path(p).is_absolute())
		return p;
	// Object-store / URI paths (s3://, gcs://, http://, …) are not local
	// filesystem paths — joining them onto file_search_path entries and
	// probing std::filesystem::exists is meaningless. Hand them straight to
	// DuckDB's FileSystem, which routes them to httpfs/S3 when loaded.
	if (p.find("://") != std::string::npos)
		return p;
	Value v;
	if (!context.TryGetCurrentSetting("file_search_path", v))
		return p;
	std::string searchPath;
	try {
		searchPath = v.GetValue<std::string>();
	} catch (...) {
		return p;
	}
	if (searchPath.empty())
		return p;
	// Comma-separated entries. Walk in order, take the first one whose
	// join with the relative path actually exists.
	std::size_t start = 0;
	while (start <= searchPath.size()) {
		std::size_t comma = searchPath.find(',', start);
		std::string entry =
		    searchPath.substr(start, comma == std::string::npos ? searchPath.size() - start : comma - start);
		if (!entry.empty()) {
			fs::path candidate = fs::path(entry) / fs::path(p);
			if (fs::exists(candidate))
				return candidate.string();
		}
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	return p; // no match — let the downstream open fail with a clean error
}

// ---- DuckDB FileSystem-backed input stream --------------------------------
//
// gorz::Reader consumes a *seekable* std::istream. To let read_gorz read from
// any storage DuckDB knows about — local, and (once httpfs/S3 is loaded)
// object stores — we bridge a DuckDB FileHandle into a std::streambuf rather
// than opening files with std::ifstream (which is CWD/local-only). The
// streambuf implements exactly the operations Reader's binary-search seek
// relies on: FileHandle::Seek + GetFileSize give O(1) positioning and ranged
// reads (an HTTP range GET over httpfs).
class FileHandleStreamBuf : public std::streambuf {
public:
	explicit FileHandleStreamBuf(unique_ptr<FileHandle> handle, std::size_t bufSize = 1u << 16)
	    : handle_(std::move(handle)), buffer_(bufSize) {
		fileSize_ = static_cast<int64_t>(handle_->GetFileSize());
		// Empty get area → first access triggers underflow() at offset 0.
		setg(buffer_.data(), buffer_.data(), buffer_.data());
	}

protected:
	int_type underflow() override {
		if (gptr() < egptr()) {
			return traits_type::to_int_type(*gptr());
		}
		// The next bytes begin right after the buffer we just exhausted; the
		// handle's own cursor is already there (reads are sequential except
		// across a seekoff, which repositions it).
		const int64_t start = bufFileStart_ + static_cast<int64_t>(egptr() - eback());
		const int64_t n = handle_->Read(buffer_.data(), static_cast<idx_t>(buffer_.size()));
		if (n <= 0) {
			return traits_type::eof();
		}
		bufFileStart_ = start;
		setg(buffer_.data(), buffer_.data(), buffer_.data() + n);
		return traits_type::to_int_type(*gptr());
	}

	pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override {
		if ((which & std::ios_base::in) == 0) {
			return pos_type(off_type(-1));
		}
		int64_t target;
		switch (dir) {
		case std::ios_base::beg:
			target = off;
			break;
		case std::ios_base::cur:
			target = bufFileStart_ + static_cast<int64_t>(gptr() - eback()) + off;
			break;
		case std::ios_base::end:
			target = fileSize_ + off;
			break;
		default:
			return pos_type(off_type(-1));
		}
		if (target < 0 || target > fileSize_) {
			return pos_type(off_type(-1));
		}
		handle_->Seek(static_cast<idx_t>(target));
		bufFileStart_ = target;
		setg(buffer_.data(), buffer_.data(), buffer_.data()); // force reload
		return pos_type(target);
	}

	pos_type seekpos(pos_type pos, std::ios_base::openmode which) override {
		return seekoff(off_type(pos), std::ios_base::beg, which);
	}

private:
	unique_ptr<FileHandle> handle_;
	std::vector<char> buffer_;
	int64_t bufFileStart_ = 0; // file offset of buffer_[0]
	int64_t fileSize_ = 0;
};

// Owns the streambuf and presents it as an istream. The buf must outlive the
// istream base, so it's a member initialised before we point rdbuf at it.
class FileHandleInputStream : public std::istream {
public:
	explicit FileHandleInputStream(unique_ptr<FileHandle> handle) : std::istream(nullptr), buf_(std::move(handle)) {
		rdbuf(&buf_);
	}

private:
	FileHandleStreamBuf buf_;
};

// Write-side counterpart of FileHandleStreamBuf: an ostream streambuf that
// forwards bytes to a DuckDB FileHandle's sequential Write. Used by the COPY TO
// path so writes go through the same FileSystem abstraction as reads (local,
// and — later — object stores). Sequential append only; no seeking.
class FileHandleOutStreamBuf : public std::streambuf {
public:
	explicit FileHandleOutStreamBuf(unique_ptr<FileHandle> handle) : handle_(std::move(handle)) {
	}

protected:
	std::streamsize xsputn(const char *s, std::streamsize n) override {
		if (n > 0) {
			handle_->Write(const_cast<char *>(s), static_cast<idx_t>(n));
		}
		return n;
	}
	int_type overflow(int_type ch) override {
		if (ch != traits_type::eof()) {
			char c = static_cast<char>(ch);
			handle_->Write(&c, 1);
		}
		return ch;
	}

private:
	unique_ptr<FileHandle> handle_;
};

// Owns the streambuf and presents it as an ostream (buf initialised before we
// point rdbuf at it, so it outlives the ostream base).
class FileHandleOutputStream : public std::ostream {
public:
	explicit FileHandleOutputStream(unique_ptr<FileHandle> handle) : std::ostream(nullptr), buf_(std::move(handle)) {
		rdbuf(&buf_);
	}

private:
	FileHandleOutStreamBuf buf_;
};

// Open `path` through DuckDB's FileSystem and wrap it as a seekable istream.
// `who` labels the caller for error messages (matches the previous ifstream
// "cannot open" phrasing). Throws IOException on failure.
std::unique_ptr<std::istream> openViaFileSystem(ClientContext &context, const std::string &path, const char *who) {
	auto &fs = FileSystem::GetFileSystem(context);
	unique_ptr<FileHandle> handle;
	try {
		handle = fs.OpenFile(path, FileOpenFlags::FILE_FLAGS_READ);
	} catch (const std::exception &e) {
		throw IOException(std::string(who) + ": cannot open " + path + ": " + e.what());
	}
	if (!handle) {
		throw IOException(std::string(who) + ": cannot open " + path);
	}
	return std::make_unique<FileHandleInputStream>(std::move(handle));
}

// Build a gorz::Opener backed by DuckDB's FileSystem, for the sidecar reads
// inside lib/gorz (.meta, .gori, GORD dictionary + entries). Unlike
// openViaFileSystem this returns nullptr (not throw) on failure, because the
// lib treats a missing sidecar as a soft fallback (empty .meta → gor type
// convention; empty .gori → sequential scan). `context` outlives the readers
// stored in per-query global state, so capturing it by reference is safe.
gorz::Opener makeFsOpener(ClientContext &context) {
	return [&context](const std::string &path) -> std::unique_ptr<std::istream> {
		auto &fs = FileSystem::GetFileSystem(context);
		try {
			auto handle = fs.OpenFile(path, FileOpenFlags::FILE_FLAGS_READ);
			if (!handle)
				return nullptr;
			return std::make_unique<FileHandleInputStream>(std::move(handle));
		} catch (const std::exception &) {
			return nullptr; // missing/unreadable sidecar → caller degrades
		}
	};
}

// Resolve the LogicalType for column `idx` given column names + an optional
// SCHEMA from .meta.
//
//   1. If SCHEMA is present and its length matches the column count, honour
//      it verbatim (S → VARCHAR, I → INTEGER, L → BIGINT, D → DOUBLE).
//   2. Otherwise apply the **gor convention**: col 0 is chrom (VARCHAR),
//      col 1 is pos (INTEGER), every other column is VARCHAR by default.
//      This matches the contract every .gorz / .gord file follows by
//      definition (PGOR cache entries don't carry SCHEMA in their .meta —
//      only COLUMNS — so without this default, downstream `gor [x]` would
//      fail with BinaryValue→IntegerValue cast errors on pos. User can
//      always wrap in CAST to override.)
LogicalType resolveColumnType(std::size_t idx, const std::vector<std::string> &columnNames,
                              const std::optional<std::vector<std::optional<gorz::GorColumnType>>> &schema) {
	// Honour an explicit SCHEMA entry when present (S/I/L/D). A nullopt
	// entry — written as `null` in the .meta — means WRITE didn't infer
	// that column's type (no -inferschema flag, the common case for cache
	// and PGOR outputs). Fall through to the gor convention then.
	if (schema.has_value() && schema->size() == columnNames.size()) {
		const auto &entry = (*schema)[idx];
		if (entry.has_value()) {
			switch (*entry) {
			case gorz::GorColumnType::String:
				return LogicalType::VARCHAR;
			case gorz::GorColumnType::Integer:
				return LogicalType::INTEGER;
			case gorz::GorColumnType::Long:
				return LogicalType::BIGINT;
			case gorz::GorColumnType::Double:
				return LogicalType::DOUBLE;
			}
		}
	}
	if (idx == 1)
		return LogicalType::INTEGER; // gor convention: pos
	return LogicalType::VARCHAR;
}

// Parse a tab-separated line into individual fields. No allocations: each
// returned string_view points into the source string.
void splitTabs(std::string_view line, std::vector<std::string_view> &out) {
	out.clear();
	std::size_t start = 0;
	for (std::size_t i = 0; i <= line.size(); ++i) {
		if (i == line.size() || line[i] == '\t') {
			out.emplace_back(line.substr(start, i - start));
			start = i + 1;
		}
	}
}

// -------- Planner-Expression extractor used by pushdown_complex_filter --------
//
// We do NOT enable `filter_pushdown = true` on the table function: that
// would elide DuckDB's post-scan filter operator for the pushed columns
// and surface as wrong results whenever there's a filter on a column we
// don't recognise (e.g. `WHERE ref = 'G'`). Instead, pushdown_complex_filter
// is a *peek*-only callback — we walk the planner-level Expression list to
// extract chrom-equality and pos-bound seek hints, leave the `filters`
// vector untouched, and DuckDB keeps the full filter set in a post-scan
// operator. The seek is then a pure optimisation: if the hint is wrong
// (or partial, or absent) the rows are still correctly filtered.
//
// Recognised shapes (col 0 = chrom, col 1 = pos by gor convention):
//   * chrom = 'chrX'           → seek.chrom = "chrX"
//   * pos <op> N (>, >=, <, <=, =)  → seek.posLo / seek.posHi
//   * AND of the above         → recursed via BoundConjunctionExpression
// Anything else (OR, IN, ref filters, ...) is silently skipped — leaves
// the seek hint partial; DuckDB will still produce correct results.

bool tryExtractChromOrPos(const Expression &e, SeekHint &hint, const std::vector<idx_t> &boundIdxToTableCol) {
	if (e.expression_class != ExpressionClass::BOUND_COMPARISON)
		return false;
	auto &cmp = e.Cast<BoundComparisonExpression>();
	auto cmpType = cmp.type;

	// Normalise: column on the left, constant on the right. If reversed,
	// flip the comparison so we don't have to handle both shapes below.
	const Expression *colExpr = cmp.left.get();
	const Expression *constExpr = cmp.right.get();
	bool flipped = false;
	if (colExpr->expression_class != ExpressionClass::BOUND_COLUMN_REF &&
	    constExpr->expression_class == ExpressionClass::BOUND_COLUMN_REF) {
		std::swap(colExpr, constExpr);
		flipped = true;
	}
	if (colExpr->expression_class != ExpressionClass::BOUND_COLUMN_REF)
		return false;
	if (constExpr->expression_class != ExpressionClass::BOUND_CONSTANT)
		return false;

	auto &colRef = colExpr->Cast<BoundColumnRefExpression>();
	auto &constRef = constExpr->Cast<BoundConstantExpression>();
	// BoundColumnRef.binding.column_index is the POST-projection slot, not
	// the original table column. Translate via column_ids — without this,
	// `WHERE ref = 'G'` on a query that only references ref looks like
	// column_index == 0 (= chrom by gor convention), and we'd mistakenly
	// stash 'G' as the chrom hint.
	const idx_t boundIdx = colRef.binding.column_index;
	if (boundIdx >= boundIdxToTableCol.size())
		return false;
	const idx_t col = boundIdxToTableCol[boundIdx];
	if (constRef.value.IsNull())
		return false;

	// Reflect the comparison when we swapped sides: `5 < pos` ≡ `pos > 5`.
	if (flipped) {
		switch (cmpType) {
		case ExpressionType::COMPARE_GREATERTHAN:
			cmpType = ExpressionType::COMPARE_LESSTHAN;
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			cmpType = ExpressionType::COMPARE_LESSTHANOREQUALTO;
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			cmpType = ExpressionType::COMPARE_GREATERTHAN;
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			cmpType = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
			break;
		case ExpressionType::COMPARE_EQUAL:
			break; // symmetric
		default:
			return false;
		}
	}

	if (col == 0) {
		// chrom column: equality only.
		if (cmpType != ExpressionType::COMPARE_EQUAL)
			return false;
		try {
			hint.chrom = constRef.value.GetValue<std::string>();
			hint.haveChrom = true;
			return true;
		} catch (...) {
			return false;
		}
	}
	if (col == 1) {
		// pos column: arithmetic comparison.
		try {
			int64_t v = constRef.value.GetValue<int64_t>();
			switch (cmpType) {
			case ExpressionType::COMPARE_EQUAL:
				hint.posLo = std::max(hint.posLo, v);
				hint.posHi = std::min(hint.posHi, v);
				hint.haveLo = hint.haveHi = true;
				return true;
			case ExpressionType::COMPARE_GREATERTHAN:
				hint.posLo = std::max(hint.posLo, v + 1);
				hint.haveLo = true;
				return true;
			case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
				hint.posLo = std::max(hint.posLo, v);
				hint.haveLo = true;
				return true;
			case ExpressionType::COMPARE_LESSTHAN:
				hint.posHi = std::min(hint.posHi, v - 1);
				hint.haveHi = true;
				return true;
			case ExpressionType::COMPARE_LESSTHANOREQUALTO:
				hint.posHi = std::min(hint.posHi, v);
				hint.haveHi = true;
				return true;
			default:
				return false;
			}
		} catch (...) {
			return false;
		}
	}
	return false;
}

// Extract from a BoundBetweenExpression — DuckDB folds
// `pos >= A AND pos <= B` (and the SQL BETWEEN form) into this single
// node before pushdown. We split it into the two implied bounds and
// reuse the comparison-extractor.
bool tryExtractBetween(const Expression &e, SeekHint &hint, const std::vector<idx_t> &boundIdxToTableCol) {
	if (e.expression_class != ExpressionClass::BOUND_BETWEEN)
		return false;
	auto &bet = e.Cast<BoundBetweenExpression>();
	// input compared against lower/upper. Each end can be inclusive or not.
	const Expression *colExpr = bet.input.get();
	if (colExpr->expression_class != ExpressionClass::BOUND_COLUMN_REF)
		return false;
	auto &colRef = colExpr->Cast<BoundColumnRefExpression>();
	const idx_t boundIdx = colRef.binding.column_index;
	if (boundIdx >= boundIdxToTableCol.size())
		return false;
	if (boundIdxToTableCol[boundIdx] != 1)
		return false; // pos only

	auto extractEnd = [&](const Expression &endExpr, bool inclusive, bool isLower) {
		if (endExpr.expression_class != ExpressionClass::BOUND_CONSTANT)
			return;
		auto &c = endExpr.Cast<BoundConstantExpression>();
		if (c.value.IsNull())
			return;
		try {
			int64_t v = c.value.GetValue<int64_t>();
			if (isLower) {
				int64_t lo = inclusive ? v : v + 1;
				hint.posLo = std::max(hint.posLo, lo);
				hint.haveLo = true;
			} else {
				int64_t hi = inclusive ? v : v - 1;
				hint.posHi = std::min(hint.posHi, hi);
				hint.haveHi = true;
			}
		} catch (...) {
		}
	};
	extractEnd(*bet.lower, bet.lower_inclusive, /*isLower=*/true);
	extractEnd(*bet.upper, bet.upper_inclusive, /*isLower=*/false);
	return true;
}

void walkForSeekHint(const Expression &e, SeekHint &hint, const std::vector<idx_t> &boundIdxToTableCol) {
	if (e.expression_class == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = e.Cast<BoundConjunctionExpression>();
		// Only AND lets us combine — OR could admit non-matching rows.
		if (conj.type == ExpressionType::CONJUNCTION_AND) {
			for (auto &child : conj.children) {
				walkForSeekHint(*child, hint, boundIdxToTableCol);
			}
		}
		return;
	}
	if (tryExtractBetween(e, hint, boundIdxToTableCol))
		return;
	tryExtractChromOrPos(e, hint, boundIdxToTableCol);
}

// pushdown_complex_filter body shared by read_gorz and read_gord. PEEKS at
// the filter list and stashes the seek hint on bind_data; the `filters`
// vector itself stays untouched, so DuckDB keeps every predicate in a
// post-scan filter operator.
template <typename BindT>
void seekHintComplexPushdown(ClientContext &, LogicalGet &get, FunctionData *bind_data,
                             vector<unique_ptr<Expression>> &filters) {
	auto &bind = bind_data->Cast<BindT>();
	// Build the bound-index → original-table-column map once for this
	// callback. column_ids[boundIdx].GetPrimaryIndex() is the original
	// column index in the table function's bind schema.
	const auto &column_ids = get.GetColumnIds();
	std::vector<idx_t> boundIdxToTableCol;
	boundIdxToTableCol.reserve(column_ids.size());
	for (const auto &ci : column_ids) {
		boundIdxToTableCol.push_back(ci.GetPrimaryIndex());
	}
	for (auto &expr : filters) {
		if (expr)
			walkForSeekHint(*expr, bind.seek, boundIdxToTableCol);
	}
}

// Collect a named tag parameter (f / ff) into `out`. Accepts a LIST(VARCHAR)
// — a literal `['a','b']`, a subquery `(SELECT list(x) ...)`, or any expression
// producing a list — and, for convenience, a bare VARCHAR. GOR keeps both -f
// (list) and -ff (file/expression); here both are lists, unioned by the caller.
void collectTagParam(TableFunctionBindInput &input, const char *name, std::vector<std::string> &out) {
	auto it = input.named_parameters.find(name);
	if (it == input.named_parameters.end() || it->second.IsNull())
		return;
	const Value &v = it->second;
	if (v.type().id() == LogicalTypeId::LIST) {
		for (const auto &child : ListValue::GetChildren(v)) {
			if (!child.IsNull())
				out.push_back(child.ToString());
		}
	} else {
		out.push_back(v.ToString());
	}
}

// Parse a GOR -p style range spec into a SeekHint (positions 1-based inclusive,
// the GOR convention). Accepted forms:
//   'chrN'            whole chromosome
//   'chrN:start-end'  inclusive window
//   'chrN:start-'     start..end-of-chrom
//   'chrN:-end'       chrom-start..end
//   'chrN:pos'        the single position pos
// Throws BinderException on a malformed spec.
SeekHint parseRangeParam(const std::string &spec) {
	SeekHint h;
	auto parseInt = [&](const std::string &t) -> int64_t {
		try {
			size_t idx = 0;
			long long v = std::stoll(t, &idx);
			if (idx != t.size() || v < 0)
				throw std::invalid_argument("");
			return static_cast<int64_t>(v);
		} catch (...) {
			throw BinderException("read_gor: range: bad position '" + t + "' in '" + spec + "'");
		}
	};
	auto colon = spec.find(':');
	std::string chrom = (colon == std::string::npos) ? spec : spec.substr(0, colon);
	if (chrom.empty()) {
		throw BinderException("read_gor: range: empty chromosome in '" + spec + "'");
	}
	h.chrom = chrom;
	h.haveChrom = true;
	if (colon == std::string::npos)
		return h; // whole chromosome

	std::string rest = spec.substr(colon + 1);
	auto dash = rest.find('-');
	if (dash == std::string::npos) { // 'chrN:pos' → single position
		if (rest.empty())
			throw BinderException("read_gor: range: empty position in '" + spec + "'");
		h.posLo = h.posHi = parseInt(rest);
		h.haveLo = h.haveHi = true;
		return h;
	}
	std::string lo = rest.substr(0, dash);
	std::string hi = rest.substr(dash + 1);
	if (!lo.empty()) {
		h.posLo = parseInt(lo);
		h.haveLo = true;
	}
	if (!hi.empty()) {
		h.posHi = parseInt(hi);
		h.haveHi = true;
	}
	if (h.haveLo && h.haveHi && h.posHi < h.posLo) {
		throw BinderException("read_gor: range: end < start in '" + spec + "'");
	}
	return h;
}

// The effective seek window for a bound query: the pushdown hint (`seek`)
// intersected with the hard `range` parameter. When `range` names a different
// chromosome than the pushdown found, the pushdown's pos bounds are dropped
// (they belong to another chromosome; the result is empty and DuckDB's WHERE
// enforces that). `range`'s chromosome is authoritative.
SeekHint effectiveSeek(const GorBindData &bind) {
	SeekHint eff = bind.seek;
	if (!bind.haveRangeParam)
		return eff;
	const SeekHint &rp = bind.rangeParam;
	if (bind.seek.haveChrom && bind.seek.chrom != rp.chrom) {
		return rp; // pushdown is for another chrom → use the param window
	}
	eff.chrom = rp.chrom;
	eff.haveChrom = true;
	if (rp.haveLo) {
		eff.posLo = eff.haveLo ? std::max(eff.posLo, rp.posLo) : rp.posLo;
		eff.haveLo = true;
	}
	if (rp.haveHi) {
		eff.posHi = eff.haveHi ? std::min(eff.posHi, rp.posHi) : rp.posHi;
		eff.haveHi = true;
	}
	return eff;
}

// Shared bind for read_gor / read_gorz / read_gord. Resolves the path,
// determines the kind (forced by read_gorz/read_gord, else by extension for
// read_gor), and discovers columns + types for that kind — filling
// return_types / names. The kind is carried on the bind data into init/execute.
unique_ptr<FunctionData> ReadGorBindImpl(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names,
                                         std::optional<GorKind> forced) {
	if (input.inputs.empty() || input.inputs[0].type() != LogicalType::VARCHAR) {
		throw BinderException("read_gor expects a single VARCHAR file path");
	}
	auto path = resolvePathViaFileSearch(context, input.inputs[0].GetValue<std::string>());
	GorKind kind = forced.has_value()
	                   ? *forced
	                   : (StringUtil::EndsWith(StringUtil::Lower(path), ".gord") ? GorKind::GORD : GorKind::GORZ);
	auto fsOpener = makeFsOpener(context);
	auto data = make_uniq<GorBindData>();
	data->kind = kind;
	data->path = path;

	// GOR -f / -ff partition filter (union of both). Only valid for .gord.
	collectTagParam(input, "f", data->tagFilter);
	collectTagParam(input, "ff", data->tagFilter);

	// GOR -s: rename the exposed source column. Only meaningful for .gord.
	auto srcIt = input.named_parameters.find("source");
	if (srcIt != input.named_parameters.end() && !srcIt->second.IsNull()) {
		data->sourceName = srcIt->second.ToString();
	}

	// GOR -p style hard range: 'chrN' | 'chrN:start-end'. Applies to both kinds.
	auto rangeIt = input.named_parameters.find("range");
	if (rangeIt != input.named_parameters.end() && !rangeIt->second.IsNull()) {
		std::string spec = rangeIt->second.ToString();
		if (!spec.empty()) {
			data->rangeParam = parseRangeParam(spec);
			data->haveRangeParam = true;
		}
	}
	if (!data->tagFilter.empty() && kind != GorKind::GORD) {
		throw BinderException("read_gor: f / ff partition filtering only applies to .gord dictionaries");
	}

	// Both kinds discover column names, then resolve types via the .meta
	// sidecar (falling back to the gor convention: col 0 VARCHAR, col 1
	// INTEGER for pos, rest VARCHAR).
	auto finishColumns = [&](const gorz::GorMeta &meta) {
		for (std::size_t i = 0; i < data->columnNames.size(); ++i) {
			names.push_back(data->columnNames[i]);
			LogicalType t = resolveColumnType(i, data->columnNames, meta.schema);
			data->columnTypes.push_back(t);
			return_types.push_back(t);
		}
	};

	if (kind == GorKind::GORZ) {
		// Main .gorz stream via DuckDB FileSystem (local FS or object stores).
		auto probe = openViaFileSystem(context, path, "read_gor");
		try {
			gorz::Reader reader(*probe);
			data->columnNames = reader.header().columns;
			gorz::GorMeta meta = gorz::parseMetaFile(path + ".meta", fsOpener);
			finishColumns(meta);
			return std::move(data);
		} catch (const gorz::FormatError &e) {
			throw IOException(std::string("read_gor: format error in ") + path + ": " + e.what());
		} catch (const std::exception &e) {
			throw IOException(std::string("read_gor: ") + e.what());
		}
	}

	// GORD: schema from the dictionary's ## COLUMNS header, else the first
	// entry's .gorz header; types from the first entry's .meta.
	try {
		gorz::Gord gord = gorz::parseGordFile(path, fsOpener);
		if (gord.entries.empty()) {
			throw IOException("read_gor: empty dictionary " + path);
		}
		if (!data->tagFilter.empty() && !gord.hasTags) {
			throw BinderException("read_gor: f / ff given but dictionary '" + path + "' carries no tags");
		}
		data->columnNames = gord.columns;
		if (data->columnNames.empty()) {
			auto first = openViaFileSystem(context, gord.entries.front().path, "read_gor");
			gorz::Reader r(*first);
			data->columnNames = r.header().columns;
		}
		gorz::GorMeta meta = gorz::parseMetaFile(gord.entries.front().path + ".meta", fsOpener);
		finishColumns(meta);
		// Expose the trailing Source column. Every emitted row carries it: bucket
		// rows already do, and GordReader appends the tag for single-tag primaries
		// so both come out at this width. Name resolution mirrors GOR's -s chain:
		//   `source` param  >  <dict>.meta SOURCE_COLUMN  >  dict header  >  "Source".
		std::string sourceCol = data->sourceName;
		if (sourceCol.empty()) {
			gorz::GorMeta dictMeta = gorz::parseMetaFile(path + ".meta", fsOpener);
			auto sc = dictMeta.properties.find("SOURCE_COLUMN");
			if (sc != dictMeta.properties.end() && !sc->second.empty()) {
				sourceCol = sc->second;
			}
		}
		if (sourceCol.empty())
			sourceCol = gord.sourceColumnName;
		if (sourceCol.empty())
			sourceCol = "Source";
		data->columnNames.push_back(sourceCol);
		data->columnTypes.push_back(LogicalType::VARCHAR);
		names.push_back(sourceCol);
		return_types.push_back(LogicalType::VARCHAR);
		return std::move(data);
	} catch (const std::exception &e) {
		throw IOException(std::string("read_gor: ") + e.what());
	}
}

// read_gor auto-detects the kind by extension; read_gorz / read_gord force it.
unique_ptr<FunctionData> ReadGorBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<string> &names) {
	return ReadGorBindImpl(context, input, return_types, names, std::nullopt);
}
unique_ptr<FunctionData> ReadGorzBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<string> &names) {
	return ReadGorBindImpl(context, input, return_types, names, GorKind::GORZ);
}
unique_ptr<FunctionData> ReadGordBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<string> &names) {
	return ReadGorBindImpl(context, input, return_types, names, GorKind::GORD);
}

unique_ptr<GlobalTableFunctionState> ReadGorInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<GorBindData>();
	auto state = make_uniq<GorGlobalState>();
	state->kind = bind.kind;
	// Effective window = pushdown seek hint ∩ the hard `range` parameter.
	SeekHint eff = effectiveSeek(bind);
	// Projection pushdown: the columns DuckDB wants, in output order.
	state->projection = input.column_ids;
	auto fsOpener = makeFsOpener(context);

	if (bind.kind == GorKind::GORD) {
		gorz::Gord gord = gorz::parseGordFile(bind.path, fsOpener);
		// Filter context reused for every per-group reader (also drives the
		// intra-entry seek + bucket row filter). Chrom-specific range; pos
		// bounds default to half-int64 sentinels to avoid overflow.
		state->opener = fsOpener;
		state->gordColumns = gord.columns.empty() ? bind.columnNames : gord.columns;
		state->gordHasTags = gord.hasTags;
		state->gordHaveRange = eff.haveChrom;
		if (eff.haveChrom) {
			state->gordChrom = eff.chrom;
			state->gordPosLo = eff.haveLo ? eff.posLo : std::numeric_limits<int64_t>::min() / 2;
			state->gordPosHi = eff.haveHi ? eff.posHi : std::numeric_limits<int64_t>::max() / 2;
		}
		state->gordTags = bind.tagFilter;

		// Prune once (metadata-only) with a planning reader, then partition the
		// surviving entries — already sorted by declared (chromStart, posStart)
		// — into range-contiguous thread groups (Part 3). Each group is read by
		// a single-threaded GordReader; the groups run in parallel.
		gorz::GordReader planner(gord, fsOpener);
		if (state->gordHaveRange) {
			planner.setRangeFilter(state->gordChrom, state->gordPosLo, state->gordPosHi);
		}
		planner.setTagFilter(state->gordTags);
		const auto &survivors = planner.candidates();
		// Bucket selection may have replaced source files with synthetic bucket
		// entries and (for a full scan of a bucketized dict) widened the tag set
		// to validTags. Propagate the *effective* requested tags so each
		// per-group reader's bucket row filter matches what the planner chose.
		state->gordTags = planner.requestedTags();

		if (!survivors.empty()) {
			int32_t nThreads = TaskScheduler::GetScheduler(context).NumberOfThreads();
			idx_t g = std::min<idx_t>(survivors.size(), std::max<idx_t>(1, static_cast<idx_t>(nThreads)));
			idx_t chunk = (survivors.size() + g - 1) / g; // ceil
			state->groups.reserve(g);
			for (idx_t start = 0; start < survivors.size(); start += chunk) {
				idx_t end = std::min<idx_t>(start + chunk, survivors.size());
				state->groups.emplace_back(survivors.begin() + start, survivors.begin() + end);
			}
		}
		return std::move(state);
	}

	// GORZ. A range/seek query keeps the single-threaded seek path below
	// (already sub-second — parallelising it would only lose the seek). A full
	// scan (no seek hint) of a large-enough file is split into byte ranges and
	// read in parallel.
	state->gorzPath = bind.path;
	state->opener = fsOpener;
	const bool hasSeek = eff.haveChrom || eff.haveLo || eff.haveHi;
	if (!hasSeek) {
		// Probe body-start + file size to plan the split.
		auto probe = openViaFileSystem(context, bind.path, "read_gor");
		int64_t bodyStart = 0;
		try {
			gorz::Reader r(*probe, bind.path, fsOpener);
			bodyStart = r.bodyStartOffset();
		} catch (const std::exception &e) {
			throw IOException(std::string("read_gor: ") + e.what());
		}
		probe->clear();
		probe->seekg(0, std::ios::end);
		int64_t fileSize = static_cast<int64_t>(probe->tellg());
		const int64_t MIN_CHUNK = 16LL * 1024 * 1024; // don't split tiny files
		int64_t span = fileSize - bodyStart;
		int32_t nThreads = TaskScheduler::GetScheduler(context).NumberOfThreads();
		idx_t g = std::min<idx_t>(std::max<idx_t>(1, static_cast<idx_t>(nThreads)),
		                          std::max<idx_t>(1, static_cast<idx_t>(span / MIN_CHUNK)));
		if (g > 1) {
			state->gorzParallel = true;
			int64_t chunk = span / static_cast<int64_t>(g); // byte-balanced
			state->byteRanges.reserve(g);
			for (idx_t i = 0; i < g; ++i) {
				int64_t s = bodyStart + static_cast<int64_t>(i) * chunk;
				int64_t e = (i + 1 == g) ? fileSize : bodyStart + static_cast<int64_t>(i + 1) * chunk;
				state->byteRanges.emplace_back(s, e);
			}
			return std::move(state); // parallel; local states open per-range readers
		}
		// g == 1 → fall through to the single-threaded full read.
	}

	// Single-threaded path: seek/range query, or a full scan too small to split.
	// Reopen via FileSystem (seekable → .gori / binary-search seek works over
	// object stores too).
	state->in = openViaFileSystem(context, bind.path, "read_gor");
	try {
		state->reader = make_uniq<gorz::Reader>(*state->in, bind.path, fsOpener);
	} catch (const std::exception &e) {
		throw IOException(std::string("read_gor: reopen failed: ") + e.what());
	}
	// Seek hint from pushdown_complex_filter → drive the reader's high-level
	// seek and set the Execute-side short-circuit (stop once we walk past the
	// target chrom). Best-effort: seekTo is no-throw and the short-circuit
	// keeps results correct even when no .gori is present.
	state->chromEq = eff.chrom;
	state->haveChromEq = eff.haveChrom;
	state->posLo = eff.posLo;
	state->posHi = eff.posHi;
	state->rangeActive = state->haveChromEq || eff.haveLo || eff.haveHi;
	if (state->haveChromEq) {
		int64_t seekPos = eff.haveLo ? eff.posLo : 0;
		try {
			state->reader->seekTo(state->chromEq, seekPos);
		} catch (const std::exception &) {
			// fall through to sequential scan — still correct
		}
	}
	return std::move(state);
}

// Parse `field` into the appropriate typed value and store it in
// `output.data[col]` at row index `row`. Empty fields produce NULL for
// non-string types (DuckDB's idiomatic behavior for absent values).
void storeField(DataChunk &output, idx_t col, idx_t row, const LogicalType &type, std::string_view field) {
	switch (type.id()) {
	case LogicalTypeId::VARCHAR:
		FlatVector::GetData<string_t>(output.data[col])[row] =
		    StringVector::AddString(output.data[col], field.data(), field.size());
		break;
	case LogicalTypeId::INTEGER: {
		if (field.empty()) {
			FlatVector::SetNull(output.data[col], row, true);
			break;
		}
		int32_t v = 0;
		auto r = std::from_chars(field.data(), field.data() + field.size(), v);
		if (r.ec != std::errc()) {
			FlatVector::SetNull(output.data[col], row, true);
		} else {
			FlatVector::GetData<int32_t>(output.data[col])[row] = v;
		}
		break;
	}
	case LogicalTypeId::BIGINT: {
		if (field.empty()) {
			FlatVector::SetNull(output.data[col], row, true);
			break;
		}
		int64_t v = 0;
		auto r = std::from_chars(field.data(), field.data() + field.size(), v);
		if (r.ec != std::errc()) {
			FlatVector::SetNull(output.data[col], row, true);
		} else {
			FlatVector::GetData<int64_t>(output.data[col])[row] = v;
		}
		break;
	}
	case LogicalTypeId::DOUBLE: {
		if (field.empty()) {
			FlatVector::SetNull(output.data[col], row, true);
			break;
		}
		// std::from_chars for double is C++17 but historically lagged in
		// libstdc++/libc++; std::stod is the safe fallback. Construct a
		// null-terminated string from the view for stod's API.
		std::string tmp(field);
		try {
			FlatVector::GetData<double>(output.data[col])[row] = std::stod(tmp);
		} catch (...) {
			FlatVector::SetNull(output.data[col], row, true);
		}
		break;
	}
	default:
		// Shouldn't happen — bind() only emits the four types above.
		FlatVector::SetNull(output.data[col], row, true);
		break;
	}
}

// Claim the next unprocessed GORD group for a local state and build a reader
// over it with the plan's range + tag filters (which also re-enable the
// intra-entry seek and bucket row filter within the group). Returns false when
// no groups remain. Thread-safe via the atomic cursor, so extra threads and
// early finishers keep pulling work until the groups are drained.
bool claimNextGroup(GorGlobalState &g, GorLocalState &local) {
	idx_t idx = g.nextGroup.fetch_add(1);
	if (idx >= g.groups.size()) {
		local.reader.reset();
		return false;
	}
	gorz::Gord sub;
	sub.columns = g.gordColumns;
	sub.entries = g.groups[idx];
	sub.hasTags = g.gordHasTags;
	auto reader = make_uniq<gorz::GordReader>(sub, g.opener);
	if (g.gordHaveRange) {
		reader->setRangeFilter(g.gordChrom, g.gordPosLo, g.gordPosHi);
	}
	reader->setTagFilter(g.gordTags);
	local.reader = std::move(reader);
	return true;
}

// Claim the next GORZ byte range for a local state and build a bounded reader
// over it (a fresh istream per range so threads don't share a stream position).
// Returns false when no ranges remain. Thread-safe via the atomic cursor.
bool claimNextByteRange(GorGlobalState &g, GorLocalState &local) {
	idx_t idx = g.nextGroup.fetch_add(1);
	if (idx >= g.byteRanges.size()) {
		local.gorzReader.reset();
		local.gorzIn.reset();
		return false;
	}
	local.gorzIn = g.opener(g.gorzPath);
	if (!local.gorzIn) {
		throw IOException("read_gor: cannot open " + g.gorzPath);
	}
	local.gorzReader = make_uniq<gorz::Reader>(*local.gorzIn, g.gorzPath, g.opener);
	local.gorzReader->setByteRange(g.byteRanges[idx].first, g.byteRanges[idx].second);
	return true;
}

unique_ptr<LocalTableFunctionState> ReadGorInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                     GlobalTableFunctionState *gstate_p) {
	auto local = make_uniq<GorLocalState>();
	auto &g = gstate_p->Cast<GorGlobalState>();
	if (g.kind == GorKind::GORD) {
		claimNextGroup(g, *local); // GORD: first dictionary group
	} else if (g.gorzParallel) {
		claimNextByteRange(g, *local); // GORZ full scan: first byte range
	}
	return std::move(local);
}

void ReadGorExecute(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &g = data.global_state->Cast<GorGlobalState>();
	const auto &bind = data.bind_data->Cast<GorBindData>();
	const idx_t nCols = bind.columnNames.size();
	const idx_t capacity = STANDARD_VECTOR_SIZE;
	idx_t row = 0;

	std::vector<std::string_view> fields;
	fields.reserve(nCols);

	// Store the projected columns of the row currently split into `fields`.
	auto storeProjected = [&]() {
		for (idx_t i = 0; i < g.projection.size(); ++i) {
			const column_t srcCol = g.projection[i];
			if (srcCol == COLUMN_IDENTIFIER_ROW_ID || srcCol >= nCols) {
				FlatVector::SetNull(output.data[i], row, true);
				continue;
			}
			storeField(output, i, row, bind.columnTypes[srcCol], fields[srcCol]);
		}
		++row;
	};

	std::string_view line;

	// GORD: parallel read. Each local state drives its claimed group's reader
	// (which does its own range/tag/row filtering + intra-entry seek), then
	// claims the next group when exhausted, until the groups are drained.
	if (bind.kind == GorKind::GORD) {
		auto &local = data.local_state->Cast<GorLocalState>();
		while (row < capacity) {
			if (!local.reader)
				break; // no group / all groups drained
			if (!local.reader->nextRow(line)) {
				if (!claimNextGroup(g, local))
					break;
				continue;
			}
			splitTabs(line, fields);
			if (fields.size() < nCols)
				fields.resize(nCols, std::string_view());
			storeProjected();
		}
		output.SetCardinality(row);
		return;
	}

	// GORZ full-scan parallel: each local state reads its claimed byte range
	// with a bounded reader, then claims the next range. No range short-circuit
	// (this path only runs when there's no seek hint).
	if (g.gorzParallel) {
		auto &local = data.local_state->Cast<GorLocalState>();
		while (row < capacity) {
			if (!local.gorzReader)
				break;
			if (!local.gorzReader->nextRow(line)) {
				if (!claimNextByteRange(g, local))
					break;
				continue;
			}
			splitTabs(line, fields);
			if (fields.size() < nCols)
				fields.resize(nCols, std::string_view());
			storeProjected();
		}
		output.SetCardinality(row);
		return;
	}

	// GORZ: single-threaded (global reader) with the range short-circuit.
	if (g.exhausted) {
		output.SetCardinality(0);
		return;
	}
	while (row < capacity) {
		if (!g.reader->nextRow(line)) {
			g.exhausted = true;
			break;
		}
		splitTabs(line, fields);
		if (fields.size() < nCols)
			fields.resize(nCols, std::string_view());

		// Skip rows outside the window and stop once past the upper bound on
		// the target chrom — rows are (chrom,pos)-sorted, so chrom > chromEq
		// means we'll never see chromEq again.
		if (g.rangeActive) {
			std::string_view rowChrom = fields[0];
			int64_t rowPos = 0;
			if (fields.size() > 1 && !fields[1].empty()) {
				auto r = std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), rowPos);
				if (r.ec != std::errc())
					rowPos = 0;
			}
			if (g.haveChromEq) {
				if (rowChrom != g.chromEq) {
					if (rowChrom > g.chromEq) {
						g.exhausted = true;
						break;
					}
					continue; // before our chrom; keep walking
				}
				if (rowPos > g.posHi) {
					g.exhausted = true;
					break;
				}
			} else {
				if (rowPos > g.posHi || rowPos < g.posLo)
					continue;
			}
			if (g.haveChromEq && rowPos < g.posLo)
				continue;
		}
		storeProjected();
	}
	output.SetCardinality(row);
}

// SELECT * FROM 'foo.gorz' → SELECT * FROM read_gorz('foo.gorz').
unique_ptr<TableRef> GorzReplacementScan(ClientContext &, ReplacementScanInput &input,
                                         optional_ptr<ReplacementScanData>) {
	auto full = ReplacementScan::GetFullPath(input);
	auto lower = StringUtil::Lower(full);
	// SELECT * FROM 'foo.gorz' / 'foo.gord' → read_gor('...'), which dispatches
	// on the extension internally.
	if (!StringUtil::EndsWith(lower, ".gorz") && !StringUtil::EndsWith(lower, ".gord")) {
		return nullptr;
	}
	const char *fn = "read_gor";
	auto table_ref = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(full)));
	table_ref->function = make_uniq<FunctionExpression>(fn, std::move(children));
	return std::move(table_ref);
}

// ===========================================================================
// COPY (SELECT ... ORDER BY chrom, pos) TO 'x.gorz' (FORMAT gorz)
//   — and the plain-text sibling FORMAT gor.
//
// Single-threaded, single-file writer. Column 0 must be VARCHAR (chrom) and
// column 1 an integer (pos); the remaining columns are written as tab-joined
// text (NULL -> empty field). Rows must already be in GOR order — the sink
// VALIDATES (lexicographic chrom, non-decreasing pos within a chrom) and errors
// out otherwise, mirroring GorZipLexOutputStream. Nothing re-sorts; ORDER BY in
// the query is the caller's job. No .meta / .gori sidecars, no columnar/zstd,
// no parallelism (execution_mode is pinned to REGULAR so insertion order — and
// therefore the validation — holds).
// ===========================================================================

enum class GorWriteFormat { GORZ, GOR };

struct GorCopyBindData : public FunctionData {
	GorWriteFormat format = GorWriteFormat::GORZ;
	vector<string> names; // SELECT output column names -> header
	vector<LogicalType> sqlTypes;

	unique_ptr<FunctionData> Copy() const override {
		auto r = make_uniq<GorCopyBindData>();
		r->format = format;
		r->names = names;
		r->sqlTypes = sqlTypes;
		return std::move(r);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &o = other_p.Cast<GorCopyBindData>();
		return format == o.format && names == o.names && sqlTypes == o.sqlTypes;
	}
};

// Per-file writer + running order key. One instance (REGULAR mode = single
// global state); the mutex is belt-and-suspenders in case DuckDB ever hands the
// sink to more than one thread.
struct GorCopyGlobalState : public GlobalFunctionData {
	unique_ptr<std::ostream> out;        // owns the FileHandle stream
	unique_ptr<gorz::Writer> gorzWriter; // GORZ only; null for plain GOR
	std::string lastChrom;
	int64_t lastPos = 0;
	bool haveLast = false;
	idx_t rowsWritten = 0;
	std::mutex mtx;
};

struct GorCopyLocalState : public LocalFunctionData {};

unique_ptr<FunctionData> GorCopyBind(ClientContext &, CopyFunctionBindInput &, const vector<string> &names,
                                     const vector<LogicalType> &sqlTypes, GorWriteFormat fmt) {
	const char *fmtName = (fmt == GorWriteFormat::GORZ) ? "gorz" : "gor";
	if (names.size() < 2) {
		throw BinderException("COPY ... TO (FORMAT %s): need at least 2 columns (chrom, pos); got %llu", fmtName,
		                      (unsigned long long)names.size());
	}
	if (sqlTypes[0].id() != LogicalTypeId::VARCHAR) {
		throw BinderException("COPY ... TO (FORMAT %s): first column '%s' (chrom) must be VARCHAR, got %s", fmtName,
		                      names[0].c_str(), sqlTypes[0].ToString().c_str());
	}
	if (!sqlTypes[1].IsIntegral()) {
		throw BinderException("COPY ... TO (FORMAT %s): second column '%s' (pos) must be an integer type, got %s",
		                      fmtName, names[1].c_str(), sqlTypes[1].ToString().c_str());
	}
	auto bd = make_uniq<GorCopyBindData>();
	bd->format = fmt;
	bd->names = names;
	bd->sqlTypes = sqlTypes;
	return std::move(bd);
}

unique_ptr<FunctionData> GorzCopyBind(ClientContext &ctx, CopyFunctionBindInput &in, const vector<string> &names,
                                      const vector<LogicalType> &types) {
	return GorCopyBind(ctx, in, names, types, GorWriteFormat::GORZ);
}
unique_ptr<FunctionData> GorPlainCopyBind(ClientContext &ctx, CopyFunctionBindInput &in, const vector<string> &names,
                                          const vector<LogicalType> &types) {
	return GorCopyBind(ctx, in, names, types, GorWriteFormat::GOR);
}

unique_ptr<GlobalFunctionData> GorCopyInitGlobal(ClientContext &context, FunctionData &bind_data,
                                                 const string &file_path) {
	auto &bd = bind_data.Cast<GorCopyBindData>();
	auto &fs = FileSystem::GetFileSystem(context);
	unique_ptr<FileHandle> handle;
	try {
		handle = fs.OpenFile(file_path, FileOpenFlags::FILE_FLAGS_WRITE | FileOpenFlags::FILE_FLAGS_FILE_CREATE);
	} catch (const std::exception &e) {
		throw IOException("COPY to gor: cannot open " + file_path + " for writing: " + e.what());
	}
	if (!handle) {
		throw IOException("COPY to gor: cannot open " + file_path + " for writing");
	}
	handle->Truncate(0); // overwrite semantics — no trailing bytes from a prior file

	auto gs = make_uniq<GorCopyGlobalState>();
	gs->out = make_uniq<FileHandleOutputStream>(std::move(handle));
	if (bd.format == GorWriteFormat::GORZ) {
		// Writer emits the header (verbatim column names) in its constructor.
		gs->gorzWriter = make_uniq<gorz::Writer>(*gs->out, bd.names);
	} else {
		// Plain .gor: header line = tab-joined column names.
		for (idx_t i = 0; i < bd.names.size(); ++i) {
			if (i)
				gs->out->put('\t');
			*gs->out << bd.names[i];
		}
		gs->out->put('\n');
	}
	return std::move(gs);
}

unique_ptr<LocalFunctionData> GorCopyInitLocal(ExecutionContext &, FunctionData &) {
	return make_uniq<GorCopyLocalState>();
}

void GorCopySink(ExecutionContext &, FunctionData &bind_data, GlobalFunctionData &gstate, LocalFunctionData &,
                 DataChunk &input) {
	auto &bd = bind_data.Cast<GorCopyBindData>();
	auto &gs = gstate.Cast<GorCopyGlobalState>();
	const idx_t ncol = input.ColumnCount();
	const idx_t nrow = input.size();
	input.Flatten(); // simplify per-cell GetValue

	std::lock_guard<std::mutex> lock(gs.mtx);
	std::string rest;
	for (idx_t r = 0; r < nrow; ++r) {
		Value cval = input.GetValue(0, r);
		if (cval.IsNull()) {
			throw InvalidInputException("gor COPY: chrom (column 1) must not be NULL");
		}
		std::string chrom = cval.ToString();
		Value pval = input.GetValue(1, r);
		if (pval.IsNull()) {
			throw InvalidInputException("gor COPY: pos (column 2) must not be NULL");
		}
		int64_t pos = pval.GetValue<int64_t>();

		// GOR order rule (matches GorZipLexOutputStream): chromosomes ascend
		// lexicographically (chr1 < chr10 < ... < chr2), positions are
		// non-decreasing within a chromosome. std::string '<' == Java
		// String.compareTo for the ASCII chrom names in practice.
		if (gs.haveLast) {
			if (chrom == gs.lastChrom) {
				if (pos < gs.lastPos) {
					throw InvalidInputException("Wrong order in gor stream " + std::to_string(pos) + " " +
					                            std::to_string(gs.lastPos) + " (need ORDER BY chrom, pos)");
				}
			} else if (chrom < gs.lastChrom) {
				throw InvalidInputException("Wrong chromosome order in gor stream " + chrom + " " + gs.lastChrom +
				                            " (need ORDER BY chrom, pos)");
			}
		}

		rest.clear();
		for (idx_t c = 2; c < ncol; ++c) {
			if (c > 2)
				rest.push_back('\t');
			Value v = input.GetValue(c, r);
			if (!v.IsNull())
				rest += v.ToString();
		}

		if (bd.format == GorWriteFormat::GORZ) {
			gs.gorzWriter->writeRow(chrom, pos, rest);
		} else {
			*gs.out << chrom << '\t' << pos;
			if (ncol > 2) {
				gs.out->put('\t');
				*gs.out << rest;
			}
			gs.out->put('\n');
		}

		gs.lastChrom = chrom;
		gs.lastPos = pos;
		gs.haveLast = true;
		gs.rowsWritten++;
	}
}

void GorCopyCombine(ExecutionContext &, FunctionData &, GlobalFunctionData &, LocalFunctionData &) {
	// Nothing to merge — the single global state does all writing.
}

void GorCopyFinalize(ClientContext &, FunctionData &, GlobalFunctionData &gstate) {
	auto &gs = gstate.Cast<GorCopyGlobalState>();
	if (gs.gorzWriter) {
		gs.gorzWriter->close(); // flush the final block
	}
	if (gs.out) {
		gs.out->flush(); // FileHandle closes when the stream (owning it) is destroyed
	}
}

CopyFunctionExecutionMode GorCopyExecMode(bool /*preserve_insertion_order*/, bool /*supports_batch_index*/) {
	// Force the non-parallel, insertion-order path: writing a single sorted
	// .gorz/.gor is inherently sequential, and out-of-order chunks would trip
	// the order validation.
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

} // namespace

// Registration shared by the loadable-extension C entry (below) and the legacy
// static Extension::Load path (GorzExtension::Load).
void LoadInternal(ExtensionLoader &loader) {
	// One implementation (ReadGorInitGlobal + ReadGorExecute) behind three
	// bind entry points: read_gor auto-detects .gorz vs .gord by extension;
	// read_gorz / read_gord force the kind. All three share:
	//
	//   * pushdown_complex_filter — a PEEK-only callback that stashes a
	//     (chrom, posLo, posHi) seek hint onto bind_data and leaves the filter
	//     list untouched, so DuckDB keeps every predicate in a post-scan filter
	//     operator and the seek is a pure optimisation. (Enabling
	//     filter_pushdown instead would make us responsible for applying every
	//     predicate, and anything we don't handle — e.g. `WHERE ref='G'` —
	//     would be silently dropped → wrong results.)
	//   * projection_pushdown — init_global receives the pruned column_ids and
	//     Execute only materialises those columns.
	auto registerGor = [&](const char *name, duckdb::table_function_bind_t bindFn) {
		duckdb::TableFunction tf(name, {duckdb::LogicalType::VARCHAR}, duckdb::ReadGorExecute, bindFn,
		                         duckdb::ReadGorInitGlobal, duckdb::ReadGorInitLocal);
		tf.pushdown_complex_filter = duckdb::seekHintComplexPushdown<duckdb::GorBindData>;
		tf.projection_pushdown = true;
		// GOR -f / -ff partition filtering (GORD only; validated at bind). Both
		// are LIST(VARCHAR) so a literal list, a subquery, or an expression all
		// work; the two are unioned.
		auto tagList = duckdb::LogicalType::LIST(duckdb::LogicalType::VARCHAR);
		tf.named_parameters["f"] = tagList;
		tf.named_parameters["ff"] = tagList;
		// GOR -p style hard range: range := 'chrN' | 'chrN:start-end'.
		tf.named_parameters["range"] = duckdb::LogicalType::VARCHAR;
		// GOR -s: rename the exposed source column (GORD only).
		tf.named_parameters["source"] = duckdb::LogicalType::VARCHAR;
		loader.RegisterFunction(tf);
	};
	registerGor("read_gor", duckdb::ReadGorBind);
	registerGor("read_gorz", duckdb::ReadGorzBind);
	registerGor("read_gord", duckdb::ReadGordBind);

	// COPY (SELECT ... ORDER BY chrom, pos) TO 'x.gorz' (FORMAT gorz)
	//   and the plain-text FORMAT gor. Registering `extension` also makes the
	//   bare `COPY ... TO 'x.gorz'` (format inferred from the suffix) work.
	//   Single-threaded, single file; validates GOR order, no sidecars.
	auto registerCopy = [&](const char *name, const char *ext, duckdb::copy_to_bind_t bindFn) {
		duckdb::CopyFunction cf(name);
		cf.extension = ext;
		cf.copy_to_bind = bindFn;
		cf.copy_to_initialize_global = duckdb::GorCopyInitGlobal;
		cf.copy_to_initialize_local = duckdb::GorCopyInitLocal;
		cf.copy_to_sink = duckdb::GorCopySink;
		cf.copy_to_combine = duckdb::GorCopyCombine;
		cf.copy_to_finalize = duckdb::GorCopyFinalize;
		cf.execution_mode = duckdb::GorCopyExecMode;
		loader.RegisterFunction(cf);
	};
	registerCopy("gorz", "gorz", duckdb::GorzCopyBind);
	registerCopy("gor", "gor", duckdb::GorPlainCopyBind);

	// Replacement scan goes through the DatabaseInstance config (no
	// ExtensionLoader method exists yet — see PR 17772). One scan rewrites
	// both .gorz and .gord literals to read_gor(...).
	auto &db = loader.GetDatabaseInstance();
	auto &config = duckdb::DBConfig::GetConfig(db);
	config.replacement_scans.emplace_back(duckdb::GorzReplacementScan);

	loader.SetDescription("Read/write GORpipe .gorz files and .gord dictionaries as native DuckDB "
	                      "tables (read_gor / read_gorz / read_gord + COPY TO). "
	                      "https://github.com/gorfather/duckdb-gorz");
}

void GorzExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string GorzExtension::Name() {
	return "gorz";
}
std::string GorzExtension::Version() const {
#ifdef EXT_VERSION_GORZ
	return EXT_VERSION_GORZ;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(gorz, loader) {
	duckdb::LoadInternal(loader);
}
} // extern "C"
