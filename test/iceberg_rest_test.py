"""Exercise table_guard against DuckDB's Iceberg REST catalog scan."""

import argparse
import json
import subprocess
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from tempfile import TemporaryDirectory
from threading import Thread
from urllib.parse import urlsplit


def sql_literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def empty_table_metadata(location):
    return {
        "format-version": 2,
        "table-uuid": "12345678-1234-5678-1234-567812345678",
        "location": str(location),
        "last-sequence-number": 0,
        "last-updated-ms": 1,
        "last-column-id": 1,
        "current-schema-id": 0,
        "schemas": [
            {
                "type": "struct",
                "schema-id": 0,
                "fields": [{"id": 1, "name": "id", "required": True, "type": "int"}],
            }
        ],
        "default-spec-id": 0,
        "partition-specs": [{"spec-id": 0, "fields": []}],
        "last-partition-id": 1000,
        "default-sort-order-id": 0,
        "sort-orders": [{"order-id": 0, "fields": []}],
        "current-snapshot-id": -1,
        "snapshots": [],
    }


def rest_catalog(directory):
    responses = {
        "/v1/config": {"defaults": {}, "overrides": {}},
        "/v1/namespaces": {"namespaces": [["clinical"]]},
        "/v1/namespaces/clinical": {"namespace": ["clinical"], "properties": {}},
        "/v1/namespaces/clinical/tables": {"identifiers": []},
    }
    for table in ("patients", "audit_log"):
        location = directory / table
        metadata_path = location / "metadata" / "v1.metadata.json"
        metadata_path.parent.mkdir(parents=True)
        metadata = empty_table_metadata(location)
        metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
        responses["/v1/namespaces/clinical/tables"]["identifiers"].append({"namespace": ["clinical"], "name": table})
        responses[f"/v1/namespaces/clinical/tables/{table}"] = {
            "metadata-location": str(metadata_path),
            "metadata": metadata,
        }

    class CatalogHandler(BaseHTTPRequestHandler):
        def log_request(self, code="-", size="-"):
            if code != 200:
                super().log_request(code, size)

        def do_GET(self):
            response = responses.get(urlsplit(self.path).path)
            if response is None:
                self.send_error(404)
                return
            body = json.dumps(response).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    return ThreadingHTTPServer(("127.0.0.1", 0), CatalogHandler)


def check_queries(duckdb, extension_path, directory, port):
    endpoint = f"http://127.0.0.1:{port}"
    setup = f"""
        LOAD {sql_literal(extension_path)};
        LOAD iceberg;
        ATTACH '' AS lake (TYPE ICEBERG, READ_ONLY, ENDPOINT '{endpoint}', AUTHORIZATION_TYPE 'none');
        ATTACH '' AS other_lake (TYPE ICEBERG, READ_ONLY, ENDPOINT '{endpoint}', AUTHORIZATION_TYPE 'none');
        CREATE VIEW audit_view AS SELECT * FROM lake.clinical.audit_log;
        SET search_path = 'memory.main,lake.clinical';
        PRAGMA table_guard_allow('lake.clinical.patients');
    """
    audit_denied = 'TableGuard: "lake.clinical.audit_log" is not in the allowlist.'
    metadata_path = directory / "patients" / "metadata" / "v1.metadata.json"
    queries = [
        ("SELECT count(*) FROM lake.clinical.patients", "0", None),
        ("SELECT count(*) FROM clinical.patients", "0", None),
        ("SELECT table_name FROM duckdb_tables() WHERE database_name = 'lake'", "patients", None),
        ("SELECT * FROM lake.clinical.audit_log", "", audit_denied),
        ("SELECT * FROM lake.clinical.audit_log LIMIT 0", "", audit_denied),
        ("WITH audit AS (SELECT * FROM audit_log) SELECT * FROM audit", "", audit_denied),
        ("SELECT * FROM audit_view", "", audit_denied),
        (
            "SELECT * FROM patients JOIN audit_log USING (id)",
            "",
            audit_denied,
        ),
        (
            "SELECT * FROM other_lake.clinical.patients",
            "",
            'TableGuard: "other_lake.clinical.patients" is not in the allowlist.',
        ),
        (
            f"SELECT * FROM iceberg_scan({sql_literal(metadata_path)})",
            "",
            'TableGuard: Iceberg function "iceberg_scan" requires a bound catalog table.',
        ),
    ]
    for sql, output, error in queries:
        result = subprocess.run(
            [duckdb, "-unsigned", "-bail", "-batch", "-csv", "-noheader"],
            input=f"{setup}\n{sql};\n",
            text=True,
            capture_output=True,
            timeout=30,
        )
        if error is None:
            passed = result.returncode == 0 and result.stdout.strip() == output
        else:
            passed = result.returncode != 0 and error in result.stderr
        if not passed:
            raise AssertionError(f"Unexpected result for {sql}: {result}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("extension", type=Path, help="path to table_guard.duckdb_extension")
    parser.add_argument("--duckdb", default="duckdb", help="path to the matching DuckDB CLI")
    args = parser.parse_args()

    with TemporaryDirectory() as temporary_path:
        directory = Path(temporary_path)
        with rest_catalog(directory) as server:
            thread = Thread(target=server.serve_forever)
            thread.start()
            try:
                check_queries(args.duckdb, args.extension.resolve(strict=True), directory, server.server_port)
            finally:
                server.shutdown()
                thread.join()

    print("Iceberg REST table guard checks passed")


if __name__ == "__main__":
    main()
