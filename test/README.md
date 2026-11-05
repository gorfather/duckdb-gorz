# Testing this extension
This directory contains all the tests for this extension. The `sql` directory holds tests that are written as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html). DuckDB aims to have most its tests in this format as SQL statements, so for the quack extension, this should probably be the goal too.

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or 
```bash
make test_debug
```

## Object-store tests

`test/sql/gord_write_s3.test` writes to and reads from an S3-compatible store. It is skipped unless
the environment names one. With Docker, a local MinIO is enough (the test needs DuckDB's `httpfs`
extension installed: `INSTALL httpfs;` once):

```bash
docker run -d --rm --name gorz-minio-test -p 127.0.0.1:9000:9000 \
  -e MINIO_ROOT_USER=minioadmin -e MINIO_ROOT_PASSWORD=minioadmin minio/minio server /data
curl -X PUT --aws-sigv4 "aws:amz:us-east-1:s3" --user minioadmin:minioadmin http://127.0.0.1:9000/gorz-test

GORZ_S3_TEST_URL=s3://gorz-test/sqllogic GORZ_S3_ENDPOINT=127.0.0.1:9000 \
AWS_ACCESS_KEY_ID=minioadmin AWS_SECRET_ACCESS_KEY=minioadmin \
  ./build/release/test/unittest "test/sql/gord_write_s3.test"

docker stop gorz-minio-test
```
