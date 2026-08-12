from __future__ import annotations

from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import sqlite3
import time
from typing import Any
import urllib.error
import urllib.parse
import urllib.request

from ..config import Settings


DEFAULT_GATEWAY_BASE_URL = "http://127.0.0.1:9010"
DEFAULT_DEVICE_OFFLINE_TIMEOUT_SEC = 120
MAX_LIMIT = 100
SQLITE_HEADER = b"SQLite format 3\x00"


class RuntimeGateway:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings

    def get_device_status(self, device_ref: str) -> dict[str, Any]:
        """Query device status from the gateway monitor API, with SQLite fallback and explicit inference markers."""
        try:
            resolved_id, resolution = self._resolve_device_id(device_ref)
            if resolved_id is None:
                return self._error("Failed to resolve device identifier.", device_ref=device_ref, resolution=resolution)

            ok, data, http_error = self._http_get_json("/api/devices")
            if ok and isinstance(data, list):
                for item in data:
                    if int(item.get("device_id", -1)) == resolved_id:
                        annotated_item = self._annotate_device_row(item)
                        latest_ota_task = self._latest_ota_task_for_device(resolved_id)
                        last_report_ms = annotated_item.get("last_report_ms")
                        return self._ok(
                            device_ref=device_ref,
                            device_id=resolved_id,
                            resolution=resolution,
                            source="http",
                            base_url=self.gateway_base_url,
                            device=annotated_item,
                            online=annotated_item.get("online"),
                            status="online" if annotated_item.get("online") else "offline",
                            status_source="http_api:/api/devices",
                            last_seen_readable_time=self._readable_time(last_report_ms),
                            last_seen_age_text=self._age_text_from_ms(last_report_ms),
                            latest_ota_task=latest_ota_task,
                            ota_status=latest_ota_task["state"] if latest_ota_task else None,
                            message="Device status was retrieved from gateway monitor HTTP API.",
                        )

            fallback = self._infer_status_from_history(resolved_id, device_ref)
            fallback["resolution"] = resolution
            if http_error:
                fallback["http_error"] = http_error
            return fallback
        except Exception as exc:
            return self._error(
                "Failed to query device status.",
                device_ref=device_ref,
                error=f"{type(exc).__name__}: {exc}",
            )

    def get_sensor_history(self, device_ref: str, limit: int = 10) -> dict[str, Any]:
        """Query recent sensor history from gateway HTTP API, with SQLite fallback when the API is unavailable."""
        try:
            resolved_id, resolution = self._resolve_device_id(device_ref)
            if resolved_id is None:
                return self._error("Failed to resolve device identifier.", device_ref=device_ref, resolution=resolution)

            normalized_limit = self._normalize_limit(limit)
            ok, data, http_error = self._http_get_json(
                "/api/history",
                params={"device_id": resolved_id, "limit": normalized_limit},
            )
            if ok and isinstance(data, list):
                records = self._annotate_sensor_rows(data)
                return self._ok(
                    device_ref=device_ref,
                    device_id=resolved_id,
                    resolution=resolution,
                    limit=normalized_limit,
                    source="http",
                    base_url=self.gateway_base_url,
                    count=len(records),
                    records=records,
                    message="Sensor history was retrieved from gateway HTTP API.",
                )

            db_path = self.gateway_db_path
            if not self._table_exists(db_path, "sensor_events"):
                return self._error(
                    "sensor_events table is unavailable in SQLite fallback.",
                    device_ref=device_ref,
                    device_id=resolved_id,
                    source="sqlite",
                    db_path=str(db_path),
                    http_error=http_error,
                )

            rows = self._query_rows(
                db_path,
                """
                SELECT id, ts_unix_ms, device_id, device_name, frame_type, link_type, seq,
                       temperature, humidity, voltage, light, status, command_id,
                       command_result, payload_summary, last_error, wifi_rssi,
                       wifi_connected, wifi_last_seen_ms, mq2_alarm, ld2402_presence,
                       led_on, alarm_on, sensor_valid, auto_mode
                FROM sensor_events
                WHERE device_id = ?
                ORDER BY ts_unix_ms DESC, id DESC
                LIMIT ?
                """,
                (resolved_id, normalized_limit),
            )
            records = self._annotate_sensor_rows(rows)
            return self._ok(
                device_ref=device_ref,
                device_id=resolved_id,
                resolution=resolution,
                limit=normalized_limit,
                source="sqlite",
                db_path=str(db_path),
                count=len(records),
                records=records,
                message="Sensor history was retrieved from SQLite fallback."
                if records
                else "No sensor history records were found for this device.",
                http_error=http_error,
            )
        except Exception as exc:
            return self._error(
                "Failed to query sensor history.",
                device_ref=device_ref,
                error=f"{type(exc).__name__}: {exc}",
            )

    def get_system_events(self, limit: int = 10) -> dict[str, Any]:
        """Query recent system events from gateway HTTP API, with SQLite fallback."""
        try:
            normalized_limit = self._normalize_limit(limit)
            ok, data, http_error = self._http_get_json(
                "/api/history/system",
                params={"limit": normalized_limit},
            )
            if ok and isinstance(data, list):
                records = self._annotate_system_rows(data)
                return self._ok(
                    source="http",
                    base_url=self.gateway_base_url,
                    limit=normalized_limit,
                    count=len(records),
                    records=records,
                    message="System events were retrieved from gateway HTTP API.",
                )

            db_path = self.gateway_db_path
            if not self._table_exists(db_path, "system_events"):
                return self._error(
                    "system_events table is unavailable in SQLite fallback.",
                    source="sqlite",
                    db_path=str(db_path),
                    http_error=http_error,
                )

            rows = self._query_rows(
                db_path,
                """
                SELECT id, ts_unix_ms, event_type, device_id, detail
                FROM system_events
                ORDER BY ts_unix_ms DESC, id DESC
                LIMIT ?
                """,
                (normalized_limit,),
            )
            records = self._annotate_system_rows(rows)
            return self._ok(
                source="sqlite",
                db_path=str(db_path),
                limit=normalized_limit,
                count=len(records),
                records=records,
                message="System events were retrieved from SQLite fallback."
                if records
                else "No system events were found.",
                http_error=http_error,
            )
        except Exception as exc:
            return self._error("Failed to query system events.", error=f"{type(exc).__name__}: {exc}")

    def get_ota_task_status(self, task_id: str) -> dict[str, Any]:
        """Query OTA task status from gateway HTTP API, with SQLite fallback for task and event history."""
        try:
            if not task_id:
                return self._error("task_id is required.", task_id=task_id)

            ok, task_data, http_error = self._http_get_json(f"/api/ota/tasks/{task_id}")
            if ok and isinstance(task_data, dict):
                events_ok, events_data, events_error = self._http_get_json(f"/api/ota/tasks/{task_id}/events")
                events = self._annotate_ota_rows(events_data) if events_ok and isinstance(events_data, list) else []
                trimmed_events, truncated = self._trim_events(events)
                return self._ok(
                    task_id=task_id,
                    source="http",
                    base_url=self.gateway_base_url,
                    ota_store=self._get_ota_store_info(),
                    task=task_data,
                    events=trimmed_events,
                    total_events=len(events),
                    events_truncated=truncated,
                    message="OTA task status was retrieved from gateway HTTP API.",
                    events_http_error=events_error,
                )

            ota_info = self._get_ota_store_info()
            ota_db_path = self.gateway_ota_db_path
            if ota_info["format"] != "sqlite":
                return self._error(
                    "OTA store is not a SQLite database. This implementation does not mix JSON and SQLite parsing.",
                    task_id=task_id,
                    source="sqlite",
                    ota_store=ota_info,
                    http_error=http_error,
                )
            if not self._table_exists(ota_db_path, "ota_tasks"):
                return self._error(
                    "OTA task store is unavailable in SQLite fallback.",
                    task_id=task_id,
                    source="sqlite",
                    db_path=str(ota_db_path),
                    ota_store=ota_info,
                    http_error=http_error,
                )

            task_rows = self._query_rows(
                ota_db_path,
                """
                SELECT task_uuid, device_id, device_type, firmware_id, state, last_error
                FROM ota_tasks
                WHERE task_uuid = ?
                LIMIT 1
                """,
                (task_id,),
            )
            if not task_rows:
                return self._error(
                    "OTA task was not found in HTTP API or SQLite fallback.",
                    task_id=task_id,
                    source="sqlite",
                    db_path=str(ota_db_path),
                    ota_store=ota_info,
                    http_error=http_error,
                )

            event_rows: list[dict[str, Any]] = []
            if self._table_exists(ota_db_path, "ota_events"):
                event_rows = self._query_rows(
                    ota_db_path,
                    """
                    SELECT id, task_uuid, ts_unix_ms, state, detail
                    FROM ota_events
                    WHERE task_uuid = ?
                    ORDER BY ts_unix_ms ASC, id ASC
                    """,
                    (task_id,),
                )
            event_rows = self._annotate_ota_rows(event_rows)
            trimmed_events, truncated = self._trim_events(event_rows)

            return self._ok(
                task_id=task_id,
                source="sqlite",
                db_path=str(ota_db_path),
                ota_store=ota_info,
                task=task_rows[0],
                events=trimmed_events,
                count=len(trimmed_events),
                total_events=len(event_rows),
                events_truncated=truncated,
                message="OTA task status was retrieved from SQLite fallback.",
                http_error=http_error,
            )
        except Exception as exc:
            return self._error(
                "Failed to query OTA task status.",
                task_id=task_id,
                error=f"{type(exc).__name__}: {exc}",
            )

    def get_database_summary(self) -> dict[str, Any]:
        """Inspect gateway SQLite databases, summarize schema, row counts, and OTA store format."""
        try:
            history_db = self.gateway_db_path
            ota_info = self._get_ota_store_info()
            summary: dict[str, Any] = {
                "history_db_path": str(history_db),
                "history_db_exists": history_db.exists(),
                "history_db_is_sqlite": self._is_sqlite_file(history_db) if history_db.exists() else False,
                "ota_store": ota_info,
                "device_offline_timeout_sec": self.device_offline_timeout_sec,
            }

            if history_db.exists() and self._is_sqlite_file(history_db):
                tables = self._query_rows(
                    history_db,
                    "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name",
                )
                summary["history_tables"] = tables
                if self._table_exists(history_db, "sensor_events"):
                    sensor_stats = self._query_rows(
                        history_db,
                        """
                        SELECT COUNT(*) AS count,
                               MIN(ts_unix_ms) AS min_ts_unix_ms,
                               MAX(ts_unix_ms) AS max_ts_unix_ms,
                               COUNT(DISTINCT device_id) AS device_count
                        FROM sensor_events
                        """,
                    )[0]
                    summary["sensor_events"] = self._add_time_fields(sensor_stats, "min_ts_unix_ms", "max_ts_unix_ms")
                if self._table_exists(history_db, "system_events"):
                    system_stats = self._query_rows(
                        history_db,
                        """
                        SELECT COUNT(*) AS count,
                               MIN(ts_unix_ms) AS min_ts_unix_ms,
                               MAX(ts_unix_ms) AS max_ts_unix_ms
                        FROM system_events
                        """,
                    )[0]
                    summary["system_events"] = self._add_time_fields(system_stats, "min_ts_unix_ms", "max_ts_unix_ms")

            ota_path = self.gateway_ota_db_path
            if ota_info["format"] == "sqlite" and self._table_exists(ota_path, "ota_tasks"):
                ota_task_stats = self._query_rows(
                    ota_path,
                    """
                    SELECT COUNT(*) AS count
                    FROM ota_tasks
                    """,
                )[0]
                summary["ota_tasks"] = ota_task_stats
                if self._table_exists(ota_path, "ota_events"):
                    ota_event_stats = self._query_rows(
                        ota_path,
                        """
                        SELECT COUNT(*) AS count,
                               MIN(ts_unix_ms) AS min_ts_unix_ms,
                               MAX(ts_unix_ms) AS max_ts_unix_ms
                        FROM ota_events
                        """,
                    )[0]
                    summary["ota_events"] = self._add_time_fields(ota_event_stats, "min_ts_unix_ms", "max_ts_unix_ms")

            return self._ok(
                source="sqlite",
                summary=summary,
                message="Database summary was collected successfully.",
            )
        except Exception as exc:
            return self._error("Failed to collect database summary.", error=f"{type(exc).__name__}: {exc}")

    def get_gateway_status(self) -> dict[str, Any]:
        """Query gateway runtime status from the monitor API."""
        try:
            ok, data, http_error = self._http_get_json("/api/status")
            if ok and isinstance(data, dict):
                return self._ok(
                    source="http",
                    base_url=self.gateway_base_url,
                    status=data,
                    serial_available=data.get("serial_available"),
                    message="Gateway status was retrieved from gateway monitor HTTP API.",
                )
            return self._error(
                "Failed to query gateway status from HTTP API.",
                source="http",
                base_url=self.gateway_base_url,
                error=http_error,
            )
        except Exception as exc:
            return self._error(
                "Failed to query gateway status.",
                source="http",
                base_url=self.gateway_base_url,
                error=f"{type(exc).__name__}: {exc}",
            )

    @property
    def gateway_base_url(self) -> str:
        return os.getenv("GATEWAY_BASE_URL", DEFAULT_GATEWAY_BASE_URL).rstrip("/")

    @property
    def gateway_db_path(self) -> Path:
        return Path(os.getenv("GATEWAY_DB_PATH", str(self.settings.gateway_history_db)))

    @property
    def gateway_ota_db_path(self) -> Path:
        return Path(os.getenv("GATEWAY_OTA_DB_PATH", str(self.settings.gateway_ota_db)))

    @property
    def device_offline_timeout_sec(self) -> int:
        raw = os.getenv("DEVICE_OFFLINE_TIMEOUT_SEC")
        if raw is None:
            return DEFAULT_DEVICE_OFFLINE_TIMEOUT_SEC
        try:
            value = int(raw)
        except ValueError:
            return DEFAULT_DEVICE_OFFLINE_TIMEOUT_SEC
        return value if value > 0 else DEFAULT_DEVICE_OFFLINE_TIMEOUT_SEC

    def _ok(self, **payload: Any) -> dict[str, Any]:
        return {"ok": True, **payload}

    def _error(self, message: str, **payload: Any) -> dict[str, Any]:
        return {"ok": False, "message": message, **payload}

    def _normalize_limit(self, limit: int, default: int = 10, maximum: int = MAX_LIMIT) -> int:
        try:
            value = int(limit)
        except (TypeError, ValueError):
            return default
        if value <= 0:
            return default
        return min(value, maximum)

    def _now_ms(self) -> int:
        return int(time.time() * 1000)

    def _readable_time(self, ts_ms: int | None) -> str | None:
        if not ts_ms:
            return None
        return datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc).isoformat()

    def _age_text_from_ms(self, ts_ms: int | None) -> str | None:
        if not ts_ms:
            return None
        delta_sec = max(0, int((self._now_ms() - ts_ms) / 1000))
        if delta_sec < 60:
            return f"{delta_sec}s ago"
        if delta_sec < 3600:
            return f"{delta_sec // 60}m ago"
        if delta_sec < 86400:
            return f"{delta_sec // 3600}h ago"
        return f"{delta_sec // 86400}d ago"

    def _add_time_fields(self, row: dict[str, Any], *timestamp_keys: str) -> dict[str, Any]:
        enriched = dict(row)
        for key in timestamp_keys:
            value = enriched.get(key)
            if isinstance(value, (int, float)) and value > 0:
                enriched[f"{key}_readable_time"] = self._readable_time(int(value))
                enriched[f"{key}_age_text"] = self._age_text_from_ms(int(value))
        return enriched

    def _http_get_json(self, path: str, params: dict[str, Any] | None = None) -> tuple[bool, Any, str | None]:
        query = urllib.parse.urlencode(params or {})
        url = f"{self.gateway_base_url}{path}"
        if query:
            url = f"{url}?{query}"
        req = urllib.request.Request(url=url, method="GET")
        try:
            with urllib.request.urlopen(req, timeout=5) as resp:
                body = json.loads(resp.read().decode("utf-8"))
            return True, body, None
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, json.JSONDecodeError) as exc:
            return False, None, f"{type(exc).__name__}: {exc}"

    @contextmanager
    def _sqlite_connect(self, path: Path) -> sqlite3.Connection:
        conn = sqlite3.connect(path)
        conn.row_factory = sqlite3.Row
        try:
            yield conn
        finally:
            conn.close()

    def _is_sqlite_file(self, path: Path) -> bool:
        if not path.exists() or path.stat().st_size < len(SQLITE_HEADER):
            return False
        with path.open("rb") as handle:
            return handle.read(len(SQLITE_HEADER)) == SQLITE_HEADER

    def _table_exists(self, db_path: Path, table_name: str) -> bool:
        if not db_path.exists() or not self._is_sqlite_file(db_path):
            return False
        with self._sqlite_connect(db_path) as conn:
            row = conn.execute(
                "SELECT name FROM sqlite_master WHERE type='table' AND name=?",
                (table_name,),
            ).fetchone()
        return row is not None

    def _query_rows(self, db_path: Path, sql: str, params: tuple[Any, ...] = ()) -> list[dict[str, Any]]:
        with self._sqlite_connect(db_path) as conn:
            rows = conn.execute(sql, params).fetchall()
        return [dict(row) for row in rows]

    def _normalize_device_alias(self, text: str) -> str:
        return re.sub(r"[^a-z0-9]+", "", text.lower())

    def _build_device_aliases(self, device_id: int, device_name: str | None = None) -> set[str]:
        aliases = {
            str(device_id),
            f"{device_id:03d}",
            f"{device_id:02d}",
            f"device_{device_id:03d}",
            f"device-{device_id:03d}",
            f"device{device_id:03d}",
            f"device_{device_id}",
            f"device-{device_id}",
            f"device{device_id}",
            f"sensor-{device_id:02d}",
            f"sensor_{device_id:02d}",
            f"sensor{device_id:02d}",
            f"sensor-{device_id}",
            f"sensor_{device_id}",
            f"sensor{device_id}",
            f"node-{device_id:02d}",
            f"node_{device_id:02d}",
            f"node{device_id:02d}",
            f"node-{device_id}",
            f"node_{device_id}",
            f"node{device_id}",
            f"wifi-node-{device_id:02d}",
            f"wifi_node_{device_id:02d}",
            f"wifinode{device_id:02d}",
            f"wifi-node-{device_id}",
            f"wifi_node_{device_id}",
            f"wifinode{device_id}",
            f"wifi-{device_id:02d}",
            f"wifi_{device_id:02d}",
            f"wifi{device_id:02d}",
            f"wifi-{device_id}",
            f"wifi_{device_id}",
            f"wifi{device_id}",
        }
        if device_name:
            aliases.add(device_name)
        return {self._normalize_device_alias(item) for item in aliases if item}

    def _known_devices_from_sqlite(self) -> list[dict[str, Any]]:
        db_path = self.gateway_db_path
        if not self._table_exists(db_path, "sensor_events"):
            return []
        return self._query_rows(
            db_path,
            """
            SELECT device_id, MAX(device_name) AS device_name
            FROM sensor_events
            GROUP BY device_id
            ORDER BY device_id
            """,
        )

    def _resolve_device_id(self, device_ref: str | int) -> tuple[int | None, dict[str, Any]]:
        raw = str(device_ref).strip()
        if not raw:
            return None, {"input": device_ref, "error": "empty device identifier"}

        if raw.isdigit():
            value = int(raw)
            return value, {"input": device_ref, "matched_by": "numeric"}

        normalized = self._normalize_device_alias(raw)
        patterns = [
            r"^device0*([0-9]+)$",
            r"^sensor0*([0-9]+)$",
            r"^node0*([0-9]+)$",
            r"^wifinode0*([0-9]+)$",
            r"^wifi0*([0-9]+)$",
        ]
        for pattern in patterns:
            match = re.match(pattern, normalized)
            if match:
                value = int(match.group(1))
                return value, {"input": device_ref, "matched_by": "pattern"}

        for item in self._known_devices_from_sqlite():
            device_id = int(item["device_id"])
            aliases = self._build_device_aliases(device_id, item.get("device_name"))
            if normalized in aliases:
                return device_id, {
                    "input": device_ref,
                    "matched_by": "sqlite_alias",
                    "device_name": item.get("device_name"),
                    "aliases": sorted(aliases),
                }

        return None, {
            "input": device_ref,
            "error": "unable to map device identifier",
            "examples": ["1", "sensor-01", "DEVICE_001"],
        }

    def _get_ota_store_info(self) -> dict[str, Any]:
        path = self.gateway_ota_db_path
        exists = path.exists()
        is_sqlite = self._is_sqlite_file(path) if exists else False
        return {
            "path": str(path),
            "exists": exists,
            "format": "sqlite" if is_sqlite else "unknown",
            "filename": path.name,
            "note": "This file is treated strictly as SQLite even though its filename ends with .json."
            if is_sqlite
            else "OTA store file is missing or not a SQLite database.",
        }

    def _latest_ota_task_for_device(self, device_id: int) -> dict[str, Any] | None:
        ota_info = self._get_ota_store_info()
        ota_db_path = self.gateway_ota_db_path
        if ota_info["format"] != "sqlite" or not self._table_exists(ota_db_path, "ota_tasks"):
            return None
        rows = self._query_rows(
            ota_db_path,
            """
            SELECT task_uuid, device_id, device_type, firmware_id, state, last_error
            FROM ota_tasks
            WHERE device_id = ?
            ORDER BY task_uuid DESC
            LIMIT 1
            """,
            (device_id,),
        )
        return rows[0] if rows else None

    def _parse_online_from_payload(self, payload_summary: str) -> bool | None:
        if not payload_summary:
            return None
        match = re.search(r"online=(\d+)", payload_summary)
        if not match:
            return None
        return match.group(1) == "1"

    def _trim_events(self, events: list[dict[str, Any]], keep_last: int = 50) -> tuple[list[dict[str, Any]], bool]:
        if len(events) <= keep_last:
            return events, False
        return events[-keep_last:], True

    def _annotate_sensor_rows(self, rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
        return [self._add_time_fields(row, "ts_unix_ms", "wifi_last_seen_ms") for row in rows]

    def _annotate_system_rows(self, rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
        return [self._add_time_fields(row, "ts_unix_ms") for row in rows]

    def _annotate_device_row(self, row: dict[str, Any]) -> dict[str, Any]:
        return self._add_time_fields(row, "last_report_ms", "wifi_last_seen_ms")

    def _annotate_ota_rows(self, rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
        return [self._add_time_fields(row, "ts_unix_ms") for row in rows]

    def _infer_status_from_history(self, device_id: int, original_ref: str | int) -> dict[str, Any]:
        db_path = self.gateway_db_path
        if not db_path.exists():
            return self._error(
                "Gateway history SQLite database does not exist.",
                device_ref=original_ref,
                device_id=device_id,
                db_path=str(db_path),
                source="sqlite",
            )
        if not self._is_sqlite_file(db_path):
            return self._error(
                "Gateway history database path is not a SQLite file.",
                device_ref=original_ref,
                device_id=device_id,
                db_path=str(db_path),
                source="sqlite",
            )
        if not self._table_exists(db_path, "sensor_events") or not self._table_exists(db_path, "system_events"):
            return self._error(
                "Required history tables are missing in SQLite database.",
                device_ref=original_ref,
                device_id=device_id,
                db_path=str(db_path),
                source="sqlite",
                available_tables=self._query_rows(
                    db_path,
                    "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name",
                ),
            )

        sensor_rows = self._query_rows(
            db_path,
            """
            SELECT id, ts_unix_ms, device_id, device_name, frame_type, link_type,
                   temperature, humidity, voltage, status, payload_summary,
                   last_error, wifi_connected, wifi_rssi, wifi_last_seen_ms
            FROM sensor_events
            WHERE device_id = ?
            ORDER BY ts_unix_ms DESC, id DESC
            LIMIT 5
            """,
            (device_id,),
        )
        system_rows = self._query_rows(
            db_path,
            """
            SELECT id, ts_unix_ms, event_type, device_id, detail
            FROM system_events
            WHERE device_id = ?
            ORDER BY ts_unix_ms DESC, id DESC
            LIMIT 5
            """,
            (device_id,),
        )
        if not sensor_rows and not system_rows:
            return self._error(
                "No device history found in SQLite database.",
                device_ref=original_ref,
                device_id=device_id,
                db_path=str(db_path),
                source="sqlite",
            )

        sensor_rows = self._annotate_sensor_rows(sensor_rows)
        system_rows = self._annotate_system_rows(system_rows)
        latest_sensor = sensor_rows[0] if sensor_rows else None
        latest_system = system_rows[0] if system_rows else None

        explicit_online: bool | None = None
        explicit_source = ""
        if latest_system and latest_system["event_type"] in {"device_online", "device_offline"}:
            explicit_online = latest_system["event_type"] == "device_online"
            explicit_source = "system_events"
        elif latest_sensor and latest_sensor["frame_type"] == "device_status":
            parsed_online = self._parse_online_from_payload(str(latest_sensor["payload_summary"]))
            if parsed_online is not None:
                explicit_online = parsed_online
                explicit_source = "sensor_events.device_status"

        latest_event_ts = 0
        if latest_sensor:
            latest_event_ts = max(latest_event_ts, int(latest_sensor["ts_unix_ms"]))
        if latest_system:
            latest_event_ts = max(latest_event_ts, int(latest_system["ts_unix_ms"]))
        last_seen_ago_sec = ((self._now_ms() - latest_event_ts) / 1000.0) if latest_event_ts else None

        online: bool | None = None
        status_source = "inferred_from_recent_events"
        status_message = "Device status is inferred from the latest history events, not from a dedicated status API."
        is_recent = last_seen_ago_sec is not None and last_seen_ago_sec <= self.device_offline_timeout_sec
        if explicit_online is not None and is_recent:
            online = explicit_online
            status_source = explicit_source
            status_message = (
                "Device status is derived from a recent history event because no dedicated live status API was available."
            )
        elif latest_event_ts:
            online = bool(is_recent)
            if is_recent:
                status_message = (
                    "Device status is inferred from the recency of the latest device event, not from a dedicated status table."
                )
            else:
                status_message = "Latest device history is stale, so current status is inferred as offline from event age."

        latest_ota_task = self._latest_ota_task_for_device(device_id)
        return self._ok(
            device_ref=original_ref,
            device_id=device_id,
            source="sqlite",
            db_path=str(db_path),
            online=bool(online) if online is not None else None,
            status="online" if online else "offline" if online is not None else "unknown",
            status_source=status_source,
            message=status_message,
            last_seen_ago_sec=last_seen_ago_sec,
            last_seen_age_text=self._age_text_from_ms(latest_event_ts),
            last_seen_readable_time=self._readable_time(latest_event_ts),
            latest_sensor_event=latest_sensor,
            recent_system_events=system_rows,
            latest_temperature=latest_sensor["temperature"] if latest_sensor else None,
            latest_humidity=latest_sensor["humidity"] if latest_sensor else None,
            latest_voltage=latest_sensor["voltage"] if latest_sensor else None,
            latest_payload_summary=latest_sensor["payload_summary"] if latest_sensor else None,
            latest_error=latest_sensor["last_error"] if latest_sensor else None,
            wifi_connected=bool(latest_sensor["wifi_connected"]) if latest_sensor else None,
            wifi_rssi=latest_sensor["wifi_rssi"] if latest_sensor else None,
            ota_status=latest_ota_task["state"] if latest_ota_task else None,
            latest_ota_task=latest_ota_task,
        )
