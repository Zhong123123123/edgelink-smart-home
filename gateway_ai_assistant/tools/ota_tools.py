from __future__ import annotations

import json
import os
from pathlib import Path
import sqlite3
from typing import Any
import urllib.error
import urllib.request

from ..config import Settings


SQLITE_HEADER = b"SQLite format 3\x00"


class OtaTools:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings

    @property
    def firmware_root(self) -> Path:
        return self.settings.gateway_root / "data" / "firmware"

    @property
    def ota_db_path(self) -> Path:
        return self.settings.gateway_ota_db

    def get_firmware_manifest(self, firmware_id: str) -> dict[str, Any]:
        """Load a firmware manifest from the gateway firmware store."""
        if not firmware_id:
            return {"ok": False, "message": "firmware_id is required."}
        manifest_path = self.firmware_root / firmware_id / "manifest.json"
        if not manifest_path.exists():
            return {
                "ok": False,
                "message": "Firmware manifest was not found.",
                "firmware_id": firmware_id,
                "manifest_path": str(manifest_path),
            }
        try:
            with manifest_path.open("r", encoding="utf-8") as handle:
                manifest = json.load(handle)
        except Exception as exc:
            return {
                "ok": False,
                "message": "Failed to read firmware manifest.",
                "firmware_id": firmware_id,
                "manifest_path": str(manifest_path),
                "error": f"{type(exc).__name__}: {exc}",
            }
        return {
            "ok": True,
            "source": "filesystem",
            "firmware_id": firmware_id,
            "manifest_path": str(manifest_path),
            "manifest": manifest,
            "message": "Firmware manifest was loaded from the gateway firmware store.",
        }

    def list_recent_ota_tasks_for_device(self, device_id: int, limit: int = 10) -> dict[str, Any]:
        """List recent OTA tasks for a device from the gateway OTA SQLite store."""
        if device_id < 0:
            return {"ok": False, "message": "device_id must be >= 0.", "device_id": device_id}
        if not self.ota_db_path.exists():
            return {
                "ok": False,
                "message": "OTA task store does not exist.",
                "device_id": device_id,
                "db_path": str(self.ota_db_path),
            }
        if not self._is_sqlite_file(self.ota_db_path):
            return {
                "ok": False,
                "message": "OTA task store is not a SQLite database.",
                "device_id": device_id,
                "db_path": str(self.ota_db_path),
            }
        try:
            rows = self._query_rows(
                """
                SELECT task_uuid, device_id, device_type, firmware_id, state, last_error
                FROM ota_tasks
                WHERE device_id = ?
                ORDER BY task_uuid DESC
                LIMIT ?
                """,
                (device_id, max(1, min(limit, 50))),
            )
        except Exception as exc:
            return {
                "ok": False,
                "message": "Failed to query OTA tasks.",
                "device_id": device_id,
                "db_path": str(self.ota_db_path),
                "error": f"{type(exc).__name__}: {exc}",
            }
        return {
            "ok": True,
            "source": "sqlite",
            "device_id": device_id,
            "db_path": str(self.ota_db_path),
            "count": len(rows),
            "tasks": rows,
            "message": "Recent OTA tasks were loaded from the gateway OTA store.",
        }

    def create_ota_task(
        self,
        device_id: int,
        device_type: str,
        firmware_id: str,
        transport: str = "serial",
        target: str = "127.0.0.1:19090",
    ) -> dict[str, Any]:
        """Create an OTA task through the existing gateway HTTP API."""
        payload = {
            "device_id": device_id,
            "device_type": device_type,
            "firmware_id": firmware_id,
            "transport": transport,
            "target": target,
        }
        base_url = self._gateway_base_url()
        req = urllib.request.Request(
            url=f"{base_url}/api/ota/tasks",
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json; charset=utf-8"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=8) as resp:
                body = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            try:
                body = json.loads(exc.read().decode("utf-8"))
            except Exception:
                body = {}
            return {
                "ok": False,
                "source": "http",
                "base_url": base_url,
                "request_payload": payload,
                "message": "Gateway OTA API returned an HTTP error.",
                "error": f"HTTPError: {exc.code}",
                "response": body,
            }
        except Exception as exc:
            return {
                "ok": False,
                "source": "http",
                "base_url": base_url,
                "request_payload": payload,
                "message": "Failed to call gateway OTA creation API.",
                "error": f"{type(exc).__name__}: {exc}",
            }
        if isinstance(body, dict):
            return {
                "ok": True,
                "source": "http",
                "base_url": base_url,
                "request_payload": payload,
                "task": body,
                "message": "OTA task was created through the gateway HTTP API.",
            }
        return {
            "ok": False,
            "source": "http",
            "base_url": base_url,
            "request_payload": payload,
            "message": "Gateway OTA API returned a non-object JSON response.",
            "response": body,
        }

    def _is_sqlite_file(self, path: Path) -> bool:
        if not path.exists() or path.stat().st_size < len(SQLITE_HEADER):
            return False
        with path.open("rb") as handle:
            return handle.read(len(SQLITE_HEADER)) == SQLITE_HEADER

    def _query_rows(self, sql: str, params: tuple[Any, ...]) -> list[dict[str, Any]]:
        conn = sqlite3.connect(self.ota_db_path)
        conn.row_factory = sqlite3.Row
        try:
            rows = conn.execute(sql, params).fetchall()
            return [dict(row) for row in rows]
        finally:
            conn.close()

    def _gateway_base_url(self) -> str:
        return os.getenv("GATEWAY_BASE_URL", "http://127.0.0.1:9010").rstrip("/")
