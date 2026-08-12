from __future__ import annotations

import sqlite3
from pathlib import Path
from typing import Any


class GatewayDB:
    def __init__(self, db_path: Path) -> None:
        self.db_path = db_path

    def exists(self) -> bool:
        return self.db_path.exists()

    def fetch_all(self, sql: str, params: tuple[Any, ...] = ()) -> tuple[list[str], list[list[Any]]]:
        if not self.exists():
            raise FileNotFoundError(f"database not found: {self.db_path}")
        conn = sqlite3.connect(f"file:{self.db_path}?mode=ro", uri=True)
        conn.row_factory = sqlite3.Row
        try:
            cursor = conn.execute(sql, params)
            rows = cursor.fetchall()
            columns = list(rows[0].keys()) if rows else [col[0] for col in cursor.description or []]
            return columns, [list(row) for row in rows]
        finally:
            conn.close()

    def table_columns(self, table_name: str) -> set[str]:
        if not self.exists():
            return set()
        conn = sqlite3.connect(f"file:{self.db_path}?mode=ro", uri=True)
        try:
            rows = conn.execute(f"PRAGMA table_info({table_name})").fetchall()
            return {str(row[1]) for row in rows}
        finally:
            conn.close()

    def table_exists(self, table_name: str) -> bool:
        if not self.exists():
            return False
        conn = sqlite3.connect(f"file:{self.db_path}?mode=ro", uri=True)
        try:
            row = conn.execute(
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
                (table_name,),
            ).fetchone()
            return row is not None
        finally:
            conn.close()
