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

To run the Iceberg REST integration test, use a DuckDB 1.5.6 CLI with `httpfs`, `avro`, and `iceberg` installed. The test starts a local catalog and loads the extension binary built for your platform.

```bash
python3 test/iceberg_rest_test.py build/release/extension/table_guard/table_guard.duckdb_extension
```
