# gorz — GORpipe `.gorz` / `.gord` for DuckDB

A DuckDB extension that reads and writes [GORpipe](https://github.com/gorpipe/gor)
genomic files — block-compressed `.gorz` and `.gord` dictionaries — as native
DuckDB tables.

```sql
INSTALL gorz FROM community;
LOAD gorz;

-- read a .gorz (or .gord dictionary) directly
SELECT chrom, pos, ref, alt FROM read_gor('gnomAD.gorz')
WHERE chrom = 'chr1' AND pos BETWEEN 100000 AND 200000;

-- bare-literal form (replacement scan)
SELECT count(*) FROM 'variants.gord';

-- write a query result back out as .gorz (GOR order required)
COPY (SELECT chrom, pos, ref, alt FROM my_variants ORDER BY chrom, pos)
  TO 'out.gorz' (FORMAT gorz);
```

## Features

- **`read_gor(path)`** — auto-detects `.gorz` vs `.gord` by extension.
- **`read_gorz(path)` / `read_gord(path)`** — force the kind.
- **Replacement scan** — `FROM 'x.gorz'` / `FROM 'x.gord'` works with no function call.
- **Projection & parallel scan** — only selected columns are read; large files scan across threads.
- **`WHERE chrom/pos` → block seek** — range predicates seek into the right block instead of scanning the whole file.
- **Partition filters** — `read_gor(dict, f := [...], ff := [...])`, the `-f` / `-ff` GOR semantics, prune a `.gord`'s file list.
- **`COPY … TO 'x.gorz' (FORMAT gorz)`** and `(FORMAT gor)` — single-threaded, block-zip; validates GOR order (chromosomes ascend lexicographically, positions non-decreasing).
- **Object stores** — paths resolve through DuckDB's FileSystem, so `s3://…` works when `httpfs` is loaded.

Files written by this extension are read/seeked natively by `gorpipe` itself, and vice-versa.

## Building locally

Uses the standard DuckDB extension toolchain (vcpkg for the `zlib` + `zstd` deps):

```shell
git clone --recurse-submodules https://github.com/gorfather/duckdb-gorz
cd duckdb-gorz
make            # builds DuckDB + the extension
make test       # runs test/sql/*.test
```

The built loadable extension lands at `build/release/extension/gorz/gorz.duckdb_extension`.
To load a locally-built (unsigned) extension, start DuckDB with `-unsigned` and
`LOAD './build/release/extension/gorz/gorz.duckdb_extension';`.

## License

MIT — see [LICENSE](LICENSE). Based on the
[DuckDB extension template](https://github.com/duckdb/extension-template). The
GORZ/GORD codec is vendored from the
[gormore](https://github.com/gorfather/gormore) project.
