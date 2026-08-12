from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .gateway_db import GatewayDB


@dataclass(frozen=True)
class QueryResult:
    sql: str
    columns: list[str]
    rows: list[list[Any]]
    note: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "sql": self.sql,
            "columns": self.columns,
            "rows": self.rows,
            "note": self.note,
        }


def _missing_db_result(db: GatewayDB) -> QueryResult:
    return QueryResult(sql="", columns=[], rows=[], note=f"database not found: {db.db_path}")


def _missing_columns_result(sql: str, missing: set[str], table_name: str) -> QueryResult:
    return QueryResult(sql=sql, columns=[], rows=[], note=f"table {table_name} missing columns: {', '.join(sorted(missing))}")


def _validate_columns(db: GatewayDB, table_name: str, required: set[str], sql: str) -> QueryResult | None:
    if not db.exists():
        return _missing_db_result(db)
    if not db.table_exists(table_name):
        return QueryResult(sql=sql, columns=[], rows=[], note=f"table not found: {table_name}")
    existing = db.table_columns(table_name)
    missing = required - existing
    if missing:
        return _missing_columns_result(sql, missing, table_name)
    return None


def query_recent_system_events(db: GatewayDB, limit: int = 20) -> QueryResult:
    sql = """
        SELECT id, ts_unix_ms, event_type, device_id, detail
        FROM system_events
        ORDER BY ts_unix_ms DESC
        LIMIT ?
    """
    invalid = _validate_columns(db, "system_events", {"id", "ts_unix_ms", "event_type", "device_id", "detail"}, sql)
    if invalid:
        return invalid
    columns, rows = db.fetch_all(sql, (max(1, min(limit, 100)),))
    return QueryResult(sql=sql.strip(), columns=columns, rows=rows)


def query_system_event_type_count(db: GatewayDB, hours: int = 24) -> QueryResult:
    sql = """
        SELECT event_type, COUNT(*) AS event_count
        FROM system_events
        WHERE ts_unix_ms >= (
            CAST(strftime('%s', 'now') AS INTEGER) * 1000 - ? * 3600 * 1000
        )
        GROUP BY event_type
        ORDER BY event_count DESC, event_type ASC
    """
    invalid = _validate_columns(db, "system_events", {"event_type", "ts_unix_ms"}, sql)
    if invalid:
        return invalid
    columns, rows = db.fetch_all(sql, (max(1, min(hours, 24 * 30)),))
    return QueryResult(sql=sql.strip(), columns=columns, rows=rows)


def query_recent_sensor_events(db: GatewayDB, limit: int = 20) -> QueryResult:
    sql = """
        SELECT id, ts_unix_ms, device_id, device_name, frame_type, temperature, humidity, light, last_error
        FROM sensor_events
        ORDER BY ts_unix_ms DESC
        LIMIT ?
    """
    required = {"id", "ts_unix_ms", "device_id", "device_name", "frame_type", "temperature", "humidity", "light", "last_error"}
    invalid = _validate_columns(db, "sensor_events", required, sql)
    if invalid:
        return invalid
    columns, rows = db.fetch_all(sql, (max(1, min(limit, 100)),))
    return QueryResult(sql=sql.strip(), columns=columns, rows=rows)


def query_latest_sensor_event(db: GatewayDB, device_id: int | None = None) -> QueryResult:
    sql = """
        SELECT id, ts_unix_ms, device_id, device_name, frame_type, temperature, humidity, light, last_error
        FROM sensor_events
        {where_clause}
        ORDER BY ts_unix_ms DESC
        LIMIT 1
    """
    required = {"id", "ts_unix_ms", "device_id", "device_name", "frame_type", "temperature", "humidity", "light", "last_error"}
    rendered_sql = sql.format(where_clause="WHERE device_id = ?" if device_id is not None else "")
    invalid = _validate_columns(db, "sensor_events", required, rendered_sql)
    if invalid:
        return invalid
    params: tuple[Any, ...] = (device_id,) if device_id is not None else ()
    columns, rows = db.fetch_all(rendered_sql, params)
    return QueryResult(sql=rendered_sql.strip(), columns=columns, rows=rows)


def query_latest_sensor_event_by_name(db: GatewayDB, device_name: str) -> QueryResult:
    sql = """
        SELECT id, ts_unix_ms, device_id, device_name, frame_type, temperature, humidity, light, last_error
        FROM sensor_events
        WHERE device_name = ?
        ORDER BY ts_unix_ms DESC
        LIMIT 1
    """
    required = {"id", "ts_unix_ms", "device_id", "device_name", "frame_type", "temperature", "humidity", "light", "last_error"}
    invalid = _validate_columns(db, "sensor_events", required, sql)
    if invalid:
        return invalid
    columns, rows = db.fetch_all(sql, (device_name,))
    return QueryResult(sql=sql.strip(), columns=columns, rows=rows)


def query_sensor_event_count(db: GatewayDB, hours: int = 24) -> QueryResult:
    sql = """
        SELECT COUNT(*) AS sensor_event_count
        FROM sensor_events
        WHERE ts_unix_ms >= (
            CAST(strftime('%s', 'now') AS INTEGER) * 1000 - ? * 3600 * 1000
        )
    """
    invalid = _validate_columns(db, "sensor_events", {"ts_unix_ms"}, sql)
    if invalid:
        return invalid
    columns, rows = db.fetch_all(sql, (max(1, min(hours, 24 * 30)),))
    return QueryResult(sql=sql.strip(), columns=columns, rows=rows)


def query_ota_status(db: GatewayDB, limit: int = 20) -> QueryResult:
    sql = """
        SELECT task_uuid, device_id, device_type, firmware_id, state, last_error
        FROM ota_tasks
        ORDER BY rowid DESC
        LIMIT ?
    """
    invalid = _validate_columns(db, "ota_tasks", {"task_uuid", "device_id", "device_type", "firmware_id", "state", "last_error"}, sql)
    if invalid:
        return invalid
    columns, rows = db.fetch_all(sql, (max(1, min(limit, 100)),))
    return QueryResult(sql=sql.strip(), columns=columns, rows=rows)


def query_ota_failed_tasks(db: GatewayDB, limit: int = 20) -> QueryResult:
    sql = """
        SELECT task_uuid, device_id, device_type, firmware_id, state, last_error
        FROM ota_tasks
        WHERE state = 'FAILED'
        ORDER BY rowid DESC
        LIMIT ?
    """
    invalid = _validate_columns(db, "ota_tasks", {"task_uuid", "device_id", "device_type", "firmware_id", "state", "last_error"}, sql)
    if invalid:
        return invalid
    columns, rows = db.fetch_all(sql, (max(1, min(limit, 100)),))
    return QueryResult(sql=sql.strip(), columns=columns, rows=rows)


def query_ota_state_count(db: GatewayDB) -> QueryResult:
    sql = """
        SELECT state, COUNT(*) AS task_count
        FROM ota_tasks
        GROUP BY state
        ORDER BY task_count DESC, state ASC
    """
    invalid = _validate_columns(db, "ota_tasks", {"state"}, sql)
    if invalid:
        return invalid
    columns, rows = db.fetch_all(sql)
    return QueryResult(sql=sql.strip(), columns=columns, rows=rows)


def query_recent_ota_events(db: GatewayDB, limit: int = 20) -> QueryResult:
    sql = """
        SELECT id, task_uuid, ts_unix_ms, state, detail
        FROM ota_events
        ORDER BY id DESC
        LIMIT ?
    """
    invalid = _validate_columns(db, "ota_events", {"id", "task_uuid", "ts_unix_ms", "state", "detail"}, sql)
    if invalid:
        return invalid
    columns, rows = db.fetch_all(sql, (max(1, min(limit, 100)),))
    return QueryResult(sql=sql.strip(), columns=columns, rows=rows)


def query_sensor_event_activity_summary(db: GatewayDB, window_hours: int = 24) -> QueryResult:
    sql = """
        WITH bounds AS (
            SELECT MAX(ts_unix_ms) AS max_ts FROM sensor_events
        )
        SELECT
            SUM(CASE
                WHEN ts_unix_ms >= (SELECT max_ts FROM bounds) - ? * 3600 * 1000
                THEN 1 ELSE 0 END
            ) AS recent_count,
            SUM(CASE
                WHEN ts_unix_ms < (SELECT max_ts FROM bounds) - ? * 3600 * 1000
                 AND ts_unix_ms >= (SELECT max_ts FROM bounds) - ? * 2 * 3600 * 1000
                THEN 1 ELSE 0 END
            ) AS previous_count,
            COUNT(DISTINCT CASE
                WHEN ts_unix_ms >= (SELECT max_ts FROM bounds) - ? * 3600 * 1000
                THEN device_id END
            ) AS recent_device_count,
            MAX(ts_unix_ms) AS latest_ts
        FROM sensor_events
    """
    invalid = _validate_columns(db, "sensor_events", {"ts_unix_ms", "device_id"}, sql)
    if invalid:
        return invalid
    hours = max(1, min(window_hours, 24 * 30))
    columns, rows = db.fetch_all(sql, (hours, hours, hours, hours))
    return QueryResult(sql=sql.strip(), columns=columns, rows=rows)


def query_ota_failure_summary(db: GatewayDB) -> QueryResult:
    sql = """
        WITH latest AS (
            SELECT MAX(rowid) AS max_rowid FROM ota_tasks
        )
        SELECT
            SUM(CASE WHEN state = 'FAILED' THEN 1 ELSE 0 END) AS failed_total,
            SUM(CASE
                WHEN rowid > (SELECT max_rowid FROM latest) - 10 AND state = 'FAILED'
                THEN 1 ELSE 0 END
            ) AS failed_last_10_tasks,
            MAX(CASE WHEN state = 'FAILED' THEN last_error ELSE '' END) AS sample_last_error
        FROM ota_tasks
    """
    invalid = _validate_columns(db, "ota_tasks", {"state", "last_error"}, sql)
    if invalid:
        return invalid
    columns, rows = db.fetch_all(sql)
    return QueryResult(sql=sql.strip(), columns=columns, rows=rows)


def query_ota_failure_reason_count(db: GatewayDB) -> QueryResult:
    sql = """
        SELECT last_error, COUNT(*) AS failed_count
        FROM ota_tasks
        WHERE state = 'FAILED'
        GROUP BY last_error
        ORDER BY failed_count DESC, last_error ASC
    """
    invalid = _validate_columns(db, "ota_tasks", {"state", "last_error"}, sql)
    if invalid:
        return invalid
    columns, rows = db.fetch_all(sql)
    return QueryResult(sql=sql.strip(), columns=columns, rows=rows)
