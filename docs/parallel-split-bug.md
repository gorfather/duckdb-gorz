# Parallel `.gorz` scans could silently drop a block

## Symptom

A full scan of a large `.gorz` returned fewer rows with many threads than with
`SET threads = 1`. It gave the same wrong answer every time, and nothing was
reported. In the file that exposed it (5,938,660 rows, 205 MB), 12 or more
threads lost 340 rows in `chr5`, while 1–10 threads gave the right count.

## Cause

A `.gorz` body is a sequence of `\n`-terminated blocks. Each block is a few KB
of compressed rows. For a full scan, the reader cuts the file into equal byte
ranges, one per thread, each at least 16 MiB. That gives
`min(threads, size / 16 MiB)` ranges. Each range reads the blocks whose
**first byte** falls inside it:

- Every range except the first starts reading at its start offset and discards
  bytes up to and including the next `\n`. The assumption is that it landed in
  the middle of a block owned by the previous range.
- A range stops once the next block would start at or after its end offset.

If a split offset happened to be **exactly the first byte of a block**, the
new range treated that whole block as the previous range's leftover and
discarded it. The previous range had already stopped, because the block starts
at its end offset. So nobody read the block, and all its rows were lost.

The fix is to start the discard **one byte earlier** (`start - 1`), the usual
trick for splitting CSV files across threads. If the split lands on a block
start, that byte is the previous block's closing `\n`, so only that `\n` is
consumed and the block is kept.

## Reproducing it

The bug needs a split offset to be an exact block start. That depends on the
file size, the thread count and the compressed size of every block before the
split, so you can't set it up by choosing data. To reproduce it:

- **With a real file:** with `debug.gorz` (not in the repo, 205 MB), 12 ranges
  put split 9 exactly on a `chr5` block start. Run
  `duckdb -c "load gorz; set threads=12; select count(*) from 'debug.gorz'"`
  on a build before the fix and compare against `threads=1`.
- **Deterministically:** `SET gorz_scan_chunk_bytes = 1` makes every byte a
  range start, so every block start is a split point. Before the fix, this
  returns only the first block. This is what
  `test/sql/gorz_parallel_split.test` does.

## How rare it is

Each split point lands on one byte of the file. It hits a block start with
probability of about `1 / average block size`. In `debug.gorz` there are
16,874 blocks averaging about 12 KB, so the chance is about **1 in 12,000 per
split point**. A scan with `g` ranges has `g − 1` split points:

- 12 ranges (11 split points): about 0.09% of files are affected.
- All thread counts from 2 to 12 together: about 0.5%.

It is rare for any one file, but a certainty across enough files. For
example, if a thousand large files are scanned at 16 threads, about one of
them loses a block. The result depends only on the file and the thread count,
so it isn't random. Rewriting the file with even one changed value changes the
compressed block sizes, which moves every block start. That's why edited
copies of the file (`debug2.gorz`) stopped showing the bug.

What was affected:

- Only DuckDB **full scans** of a `.gorz` of at least 32 MiB. Anything
  smaller is read by one thread.
- Not queries with a chromosome or position filter, which use the
  single-threaded seek path.
- Not `.gord` dictionaries, which are split per file, not per byte range.
- Not the files themselves, which were never damaged.

Each hit loses exactly one block, typically a few hundred rows.
