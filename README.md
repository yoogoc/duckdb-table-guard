# DuckDB Table Guard Extension

A DuckDB extension that enforces table-level access control via an allowlist mechanism. Only tables explicitly added to the allowlist can be queried; all other table accesses are blocked with a `PermissionException`.

## Features

- **Table-level allowlist**: restrict queries to only the tables you explicitly permit
- **Catalog.schema.table granularity**: support `catalog.schema.table`, `schema.table`, or bare `table` patterns, with `*` as wildcard
- **One-time configuration**: the allowlist can only be set once per connection, preventing runtime tampering
- **Metadata filtering**: automatically filters `duckdb_tables()`, `duckdb_views()`, and `duckdb_columns()` so that only allowed tables are visible
- **Enable/disable toggle**: temporarily disable the guard without losing the allowlist configuration

## Usage

```sql
-- Load the extension
LOAD table_guard;

-- Set the allowlist (once per connection, comma-separated)
PRAGMA table_guard_allow('mydb.public.patients, mydb.public.visits');

-- Allowed queries work normally
SELECT * FROM mydb.public.patients;
SELECT * FROM mydb.public.visits;

-- Blocked tables raise a PermissionException
SELECT * FROM mydb.public.audit_log;
-- Error: TableGuard: "mydb.public.audit_log" is not in the allowlist.

-- Metadata queries are also filtered
SELECT table_name FROM duckdb_tables();
-- Only shows: patients, visits

-- Temporarily disable the guard
PRAGMA table_guard_disable;

-- Re-enable the guard
PRAGMA table_guard_enable;

-- Check current status
PRAGMA table_guard_status;
```

## Allowlist Format

Entries are comma-separated and support three levels of specificity:

| Format | Example | Meaning |
|--------|---------|---------|
| `catalog.schema.table` | `mydb.public.patients` | Exact match |
| `schema.table` | `public.patients` | Any catalog |
| `table` | `patients` | Any catalog and schema |
| Wildcard `*` | `mydb.*.patients` | Match any value for that component |

## PRAGMA Commands

| Command | Description |
|---------|-------------|
| `PRAGMA table_guard_allow('entries')` | Set the allowlist (one-time, comma-separated) |
| `PRAGMA table_guard_enable` | Enable the guard (requires allowlist to be set first) |
| `PRAGMA table_guard_disable` | Disable the guard (allowlist is preserved) |
| `PRAGMA table_guard_status` | Print current guard state |

## Building

### Managing dependencies
DuckDB extensions uses VCPKG for dependency management. Enabling VCPKG is very simple: follow the [installation instructions](https://vcpkg.io/en/getting-started) or just run the following:
```shell
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=`pwd`/vcpkg/scripts/buildsystems/vcpkg.cmake
```

### Build steps
Now to build the extension, run:
```sh
make
```
The main binaries that will be built are:
```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/table_guard/table_guard.duckdb_extension
```
- `duckdb` is the binary for the duckdb shell with the extension code automatically loaded.
- `unittest` is the test runner of duckdb. Again, the extension is already linked into the binary.
- `table_guard.duckdb_extension` is the loadable binary as it would be distributed.

## Running the tests
```sh
make test
```

## Installing the deployed binaries
To install your extension binaries from S3, you will need to do two things. Firstly, DuckDB should be launched with the
`allow_unsigned_extensions` option set to true. Some examples:

CLI:
```shell
duckdb -unsigned
```

Python:
```python
con = duckdb.connect(':memory:', config={'allow_unsigned_extensions' : 'true'})
```

NodeJS:
```js
db = new duckdb.Database(':memory:', {"allow_unsigned_extensions": "true"});
```

Then set the repository endpoint and install:
```sql
SET custom_extension_repository='bucket.s3.eu-west-1.amazonaws.com/table_guard/latest';
INSTALL table_guard;
LOAD table_guard;
```