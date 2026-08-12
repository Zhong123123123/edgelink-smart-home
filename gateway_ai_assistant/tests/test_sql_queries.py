from __future__ import annotations

import sqlite3
import tempfile
import unittest
from pathlib import Path

from gateway_ai_assistant.sql.gateway_db import GatewayDB
from gateway_ai_assistant.sql.query_templates import query_latest_sensor_event_by_name, query_recent_system_events


class SQLQueryTests(unittest.TestCase):
    def test_missing_database_returns_clear_note(self) -> None:
        db = GatewayDB(Path("/tmp/nonexistent-gateway-history.db"))
        result = query_recent_system_events(db)
        self.assertIn("database not found", result.note)

    def test_missing_columns_returns_clear_note(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            db_path = Path(tmpdir) / "history.db"
            conn = sqlite3.connect(db_path)
            conn.execute("CREATE TABLE system_events (id INTEGER PRIMARY KEY)")
            conn.commit()
            conn.close()
            db = GatewayDB(db_path)
            result = query_recent_system_events(db)
            self.assertIn("missing columns", result.note)

    def test_template_query_does_not_execute_user_sql(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            db_path = Path(tmpdir) / "history.db"
            conn = sqlite3.connect(db_path)
            conn.execute(
                "CREATE TABLE system_events (id INTEGER PRIMARY KEY, ts_unix_ms INTEGER, event_type TEXT, device_id INTEGER, detail TEXT)"
            )
            conn.execute(
                "INSERT INTO system_events(ts_unix_ms, event_type, device_id, detail) VALUES(1, 'device_online', 1, 'sensor-01')"
            )
            conn.commit()
            conn.close()
            db = GatewayDB(db_path)
            result = query_recent_system_events(db, limit=5)
            self.assertEqual(len(result.rows), 1)
            self.assertNotIn("DROP TABLE", result.sql.upper())

    def test_query_latest_sensor_event_by_name(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            db_path = Path(tmpdir) / "history.db"
            conn = sqlite3.connect(db_path)
            conn.execute(
                "CREATE TABLE sensor_events (id INTEGER PRIMARY KEY, ts_unix_ms INTEGER, device_id INTEGER, device_name TEXT, frame_type TEXT, temperature REAL, humidity REAL, light REAL, last_error TEXT)"
            )
            conn.execute(
                "INSERT INTO sensor_events VALUES (1, 2, 1, 'sensor-01', 'sensor_data', 25.1, 52.0, 12.0, '')"
            )
            conn.commit()
            conn.close()
            db = GatewayDB(db_path)
            result = query_latest_sensor_event_by_name(db, "sensor-01")
            self.assertEqual(result.rows[0][3], "sensor-01")


if __name__ == "__main__":
    unittest.main()
