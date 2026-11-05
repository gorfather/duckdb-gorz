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

-- write a .gord dictionary folder: one sorted .gorz per genomic range (any input order)
COPY (SELECT chrom, pos, ref, alt FROM my_variants)
  TO 'out.gord' (FORMAT gord, SPLIT 100);
```

## Features

- **`read_gor(path)`** — auto-detects `.gorz` vs `.gord` by extension.
- **`read_gorz(path)` / `read_gord(path)`** — force the kind.
- **Replacement scan** — `FROM 'x.gorz'` / `FROM 'x.gord'` works with no function call.
- **`pgor … | write x.gord` folders** — pass the folder itself (`FROM 'x.gord'`); it reads the `x.gord/thedict.gord` inside, so the explicit `'x.gord/thedict.gord'` is optional.
- **Projection & parallel scan** — only selected columns are read; large files scan across threads.
- **`WHERE chrom/pos` → block seek** — range predicates seek into the right block instead of scanning the whole file.
- **Partition filters** — GOR's `-f` / `-ff` semantics prune a `.gord`'s file list and expose a `Source` column: `f := ['A','B']` is an inline tag list, `ff := 'tags.txt'` is a *tag file* (one tag per line / first tab-delimited column, `#` lines skipped). `source := 'PN'` renames the exposed column.
- **`COPY … TO 'x.gorz' (FORMAT gorz)`** and `(FORMAT gor)` — single-threaded, block-zip; validates GOR order (chromosomes ascend lexicographically, positions non-decreasing). `META` also writes the `.gorz.meta` sidecar (columns, types, line count, range, MD5), which `read_gor` uses to hand the column types back.
- **`COPY … TO 'x.gord' (FORMAT gord)`** — writes a dictionary *folder*, as `pgor … | write x.gord` does: one `.gorz` + `.gorz.meta` per genomic range and `x.gord/thedict.gord`. See [Writing a `.gord` folder](#writing-a-gord-folder).
- **Object stores** — reads and writes resolve through DuckDB's FileSystem, so `s3://…` (S3, MinIO, R2, GCS) works for `.gorz` files and `.gord` folders alike once `httpfs` is loaded and a secret is set up.

Files written by this extension are read/seeked natively by `gorpipe` itself, and vice-versa.

## Demo notebook

[`examples/gorz_community_extension.ipynb`](examples/gorz_community_extension.ipynb)
walks through the published extension end to end — `.gorz` round-trip, range
pushdown (GOR's `-p` semantics), `.gord` dictionaries and the `Source` column,
and partition filtering with `f` / `ff` tag files.

It installs the extension from the community repository, so it needs no local
build and writes only to `/tmp`:

```shell
pip install duckdb jupyterlab pandas
jupyter lab examples/gorz_community_extension.ipynb
```

## Writing a `.gord` folder

```sql
COPY (SELECT * FROM variants) TO 'out.gord' (FORMAT gord);                  -- one file per chromosome
COPY (SELECT * FROM variants) TO 'out.gord' (FORMAT gord, SPLIT 100);       -- 100 genome partitions
COPY (SELECT * FROM variants) TO 'out.gord' (FORMAT gord, SPLIT 10000000);  -- 10 Mb per file
SELECT count(*) FROM 'out.gord' WHERE chrom = 'chr17';                      -- read it back by folder path
```

The first two columns must be the chromosome (`VARCHAR`) and the position (an integer). The query
needs no `ORDER BY`: rows may arrive in any order and each file is sorted as it is written.

| Option | Meaning |
|---|---|
| `SPLIT 'chrom'` | One file per chromosome (the default). |
| `SPLIT n`, `n` ≤ 1000 | `n` genome partitions: a step of 3,000,000,000 / `n` bases within each chromosome (as `pgor -split=n`). |
| `SPLIT n`, `n` > 1000 | `n` bases per file. |
| `OVERWRITE` | Replace an existing `.gord` folder. Without it a non-empty target is an error; a directory holding anything that is not part of a `.gord` folder is never deleted. |
| `STAGING 'dir'` | Where to put the temporary files (default: next to the output). |

A file never spans chromosomes, ranges with no rows produce no file, and at most 5000 files are
written. The dictionary lists each file with the alias `1..N` and the range of the rows it holds.

How it works: the query runs once, in your connection (temp tables and views work), and is written
as partitioned Parquet into a staging directory, one partition per range. Each partition is then
sorted and written as `.gorz` on its own, so no sort ever covers more than one partition and memory
follows the partition size (set by `SPLIT`) rather than the size of the input. Several partitions
run at once — as many as fit within `memory_limit`, so larger partitions run fewer at a time. The
staging directory needs room for a Parquet copy of the data and is removed at the end;
`thedict.gord` is written last, so an interrupted write never leaves a readable dictionary.

### Writing to an object store

```sql
LOAD httpfs;
CREATE SECRET (TYPE s3, KEY_ID '…', SECRET '…', REGION 'eu-west-1');
COPY (SELECT * FROM variants) TO 's3://bucket/out.gord' (FORMAT gord, SPLIT 100);
SELECT count(*) FROM 's3://bucket/out.gord' WHERE chrom = 'chr17';
```

The staging files stay on local disk — in DuckDB's `temp_directory` unless `STAGING` names another
local directory — and only the finished `.gorz`, `.meta` and `thedict.gord` objects are uploaded,
the dictionary last. `OVERWRITE` deletes the objects of the folder being replaced.

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
