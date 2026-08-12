from __future__ import annotations

import sqlite3
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from gateway_ai_assistant.config import get_settings
from gateway_ai_assistant.tools.runtime_gateway import RuntimeGateway


class RuntimeGatewayTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmpdir = tempfile.TemporaryDirectory()
        self.history_db = Path(self.tmpdir.name) / "gateway_history.db"
        self.ota_db = Path(self.tmpdir.name) / "ota_tasks.json"
        self._create_history_db(self.history_db)
        self._create_ota_db(self.ota_db)
        self.settings = get_settings()
        self.gateway = RuntimeGateway(self.settings)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def _create_history_db(self, path: Path) -> None:
        conn = sqlite3.connect(path)
        conn.execute(
            """
            CREATE TABLE sensor_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts_unix_ms INTEGER NOT NULL,
                device_id INTEGER NOT NULL,
                device_name TEXT NOT NULL DEFAULT '',
                frame_type TEXT NOT NULL DEFAULT '',
                link_type TEXT NOT NULL DEFAULT '',
                seq INTEGER NOT NULL DEFAULT 0,
                temperature REAL NOT NULL DEFAULT 0.0,
                humidity REAL NOT NULL DEFAULT 0.0,
                voltage REAL NOT NULL DEFAULT 0.0,
                light REAL NOT NULL DEFAULT 0.0,
                status INTEGER NOT NULL DEFAULT 0,
                command_id INTEGER NOT NULL DEFAULT 0,
                command_result INTEGER NOT NULL DEFAULT 0,
                payload_summary TEXT NOT NULL DEFAULT '',
                last_error TEXT NOT NULL DEFAULT '',
                wifi_rssi INTEGER NOT NULL DEFAULT 0,
                wifi_connected INTEGER NOT NULL DEFAULT 0,
                wifi_last_seen_ms INTEGER NOT NULL DEFAULT 0,
                mq2_alarm INTEGER NOT NULL DEFAULT -1,
                ld2402_presence INTEGER NOT NULL DEFAULT -1,
                led_on INTEGER NOT NULL DEFAULT -1,
                alarm_on INTEGER NOT NULL DEFAULT -1,
                sensor_valid INTEGER NOT NULL DEFAULT -1,
                auto_mode INTEGER NOT NULL DEFAULT -1
            )
            """
        )
        conn.execute(
            """
            CREATE TABLE system_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts_unix_ms INTEGER NOT NULL,
                event_type TEXT NOT NULL,
                device_id INTEGER NOT NULL DEFAULT 0,
                detail TEXT NOT NULL DEFAULT ''
            )
            """
        )
        conn.execute(
            """
            INSERT INTO sensor_events (
                ts_unix_ms, device_id, device_name, frame_type, link_type, seq,
                temperature, humidity, voltage, light, status, command_id, command_result,
                payload_summary, last_error, wifi_rssi, wifi_connected, wifi_last_seen_ms,
                mq2_alarm, ld2402_presence, led_on, alarm_on, sensor_valid, auto_mode
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                1893456000000,
                1,
                "sensor-01",
                "sensor_data",
                "wifi",
                7,
                25.5,
                54.0,
                3.3,
                100.0,
                1,
                0,
                0,
                "online=1,temp=25.5",
                "",
                -50,
                1,
                1893456000000,
                0,
                1,
                1,
                0,
                1,
                1,
            ),
        )
        conn.execute(
            "INSERT INTO system_events (ts_unix_ms, event_type, device_id, detail) VALUES (?, ?, ?, ?)",
            (1893456000000, "device_online", 1, "device came online"),
        )
        conn.commit()
        conn.close()

    def _create_ota_db(self, path: Path) -> None:
        conn = sqlite3.connect(path)
        conn.execute(
            """
            CREATE TABLE ota_tasks (
                task_uuid TEXT PRIMARY KEY,
                device_id INTEGER NOT NULL,
                device_type TEXT NOT NULL,
                firmware_id TEXT NOT NULL,
                state TEXT NOT NULL,
                last_error TEXT NOT NULL
            )
            """
        )
        conn.execute(
            """
            CREATE TABLE ota_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                task_uuid TEXT NOT NULL,
                ts_unix_ms INTEGER NOT NULL,
                state TEXT NOT NULL,
                detail TEXT NOT NULL
            )
            """
        )
        conn.execute(
            "INSERT INTO ota_tasks (task_uuid, device_id, device_type, firmware_id, state, last_error) VALUES (?, ?, ?, ?, ?, ?)",
            ("ota-001", 1, "stm32", "fw-1", "confirmed", ""),
        )
        conn.execute(
            "INSERT INTO ota_events (task_uuid, ts_unix_ms, state, detail) VALUES (?, ?, ?, ?)",
            ("ota-001", 1893456000000, "confirmed", "done"),
        )
        conn.commit()
        conn.close()

    @patch.object(RuntimeGateway, "_http_get_json", return_value=(False, None, "offline"))
    def test_get_device_status_sqlite_fallback(self, _mock_http) -> None:
        with patch.dict(
            "os.environ",
            {
                "GATEWAY_DB_PATH": str(self.history_db),
                "GATEWAY_OTA_DB_PATH": str(self.ota_db),
                "DEVICE_OFFLINE_TIMEOUT_SEC": "999999999",
            },
            clear=False,
        ):
            result = self.gateway.get_device_status("sensor-01")
        self.assertTrue(result["ok"])
        self.assertEqual(result["status"], "online")
        self.assertEqual(result["source"], "sqlite")

    @patch.object(RuntimeGateway, "_http_get_json", return_value=(False, None, "offline"))
    def test_get_ota_task_status_sqlite_fallback(self, _mock_http) -> None:
        with patch.dict(
            "os.environ",
            {
                "GATEWAY_OTA_DB_PATH": str(self.ota_db),
            },
            clear=False,
        ):
            result = self.gateway.get_ota_task_status("ota-001")
        self.assertTrue(result["ok"])
        self.assertEqual(result["task"]["state"], "confirmed")

    @patch.object(RuntimeGateway, "_http_get_json", return_value=(True, {"instance": "primary", "serial_available": False}, None))
    def test_get_gateway_status_http(self, _mock_http) -> None:
        result = self.gateway.get_gateway_status()
        self.assertTrue(result["ok"])
        self.assertEqual(result["source"], "http")
        self.assertFalse(result["serial_available"])

    @patch.object(RuntimeGateway, "_http_get_json", return_value=(False, None, "offline"))
    def test_get_gateway_status_http_error(self, _mock_http) -> None:
        result = self.gateway.get_gateway_status()
        self.assertFalse(result["ok"])
        self.assertEqual(result["source"], "http")
        self.assertEqual(result["error"], "offline")


if __name__ == "__main__":
    unittest.main()
