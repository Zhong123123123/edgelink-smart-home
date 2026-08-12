from __future__ import annotations

import sqlite3
from pathlib import Path
from typing import Any


def inspect_sqlite(db_path: Path, sample_rows: int = 3) -> dict[str, Any]:
    if not db_path.exists():
        return {"database": str(db_path), "error": "database not found", "tables": []}

    conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    conn.row_factory = sqlite3.Row
    try:
        tables = []
        table_rows = conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
        ).fetchall()
        for table_row in table_rows:
            name = table_row["name"]
            columns = [
                {"name": row["name"], "type": row["type"]}
                for row in conn.execute(f"PRAGMA table_info({name})").fetchall()
            ]
            samples = [
                dict(row)
                for row in conn.execute(f"SELECT * FROM {name} ORDER BY rowid DESC LIMIT ?", (sample_rows,)).fetchall()
            ]
            tables.append({"name": name, "columns": columns, "samples": samples})
        return {"database": str(db_path), "tables": tables}
    finally:
        conn.close()
