from __future__ import annotations

from contextlib import contextmanager
import sqlite3
from dataclasses import dataclass
from datetime import datetime, timezone
import json
from pathlib import Path


@dataclass(frozen=True)
class QueryLogRecord:
    question: str
    route_type: str
    sql_used: str
    answer_summary: str


@dataclass(frozen=True)
class ToolCallLogRecord:
    session_id: str
    question: str
    tool_name: str
    tool_input: dict[str, object]
    tool_output: dict[str, object]
    status: str


@dataclass(frozen=True)
class ApprovalRequestRecord:
    approval_id: str
    request_type: str
    request_payload: dict[str, object]
    risk_summary: str
    status: str
    approved_by: str = ""
    approved_at: str = ""
    decision_note: str = ""
    result_payload: dict[str, object] | None = None


@dataclass(frozen=True)
class ActionAuditLogRecord:
    session_id: str
    action_type: str
    operator: str
    request_payload: dict[str, object]
    result_payload: dict[str, object]
    status: str


@dataclass(frozen=True)
class ReportRecord:
    report_id: str
    report_type: str
    title: str
    device_id: int | None
    task_id: str
    summary: str
    markdown_path: str
    json_path: str
    created_by: str = "agent"


@dataclass(frozen=True)
class TicketRecord:
    ticket_id: str
    title: str
    severity: str
    status: str
    device_id: int | None
    report_id: str
    description: str
    assignee: str = ""


@dataclass(frozen=True)
class OtaBatchRunRecord:
    batch_run_id: str
    approval_id: str
    firmware_id: str
    transport: str
    target: str
    batch_size: int
    batch_count: int
    total_devices: int
    status: str
    created_by: str = "operator"
    summary_payload: dict[str, object] | None = None


@dataclass(frozen=True)
class OtaBatchRunItemRecord:
    batch_run_id: str
    batch_index: int
    device_id: int
    status: str
    task_uuid: str = ""
    last_error: str = ""
    retry_count: int = 0
    last_checked_at: str = ""


@dataclass(frozen=True)
class EvalRunRecord:
    run_id: str
    eval_type: str
    status: str
    case_count: int
    summary_payload: dict[str, object]
    baseline_run_id: str = ""


@dataclass(frozen=True)
class EvalCaseResultRecord:
    run_id: str
    case_index: int
    session_id: str
    question: str
    description: str
    ok: bool
    agent_ok: bool
    interrupted: bool
    expected_tools: list[str]
    actual_tools: list[str]
    missing_tools: list[str]
    forbidden_called: list[str]
    tool_order_ok: bool
    answer_present: bool
    failure_reasons: list[str]
    result_payload: dict[str, object]
    replay_payload: dict[str, object]


@dataclass(frozen=True)
class SessionMemoryRecord:
    session_id: str
    summary: str
    key_facts: dict[str, object]
    last_question: str = ""
    last_answer: str = ""


@dataclass(frozen=True)
class LongTermMemoryRecord:
    scope: str
    memory_key: str
    content: str
    confidence: float = 0.5
    source_session_id: str = ""
    memory_type: str = "preference"
    provenance: str = "manual_curated"
    status: str = "active"
    is_pinned: bool = False
    expires_at: str = ""


@dataclass(frozen=True)
class MemoryCandidateRecord:
    candidate_id: str
    session_id: str
    scope: str
    memory_key: str
    content: str
    rationale: str
    confidence: float = 0.6
    memory_type: str = "fact"
    provenance: str = "user_stated"
    risk_score: float = 0.0
    risk_flags: list[str] | None = None
    status: str = "pending"


@dataclass(frozen=True)
class MemoryAuditLogRecord:
    memory_scope: str
    memory_key: str
    action_type: str
    actor: str
    actor_role: str
    before_state: dict[str, object]
    after_state: dict[str, object]
    reason: str = ""
    memory_id: int | None = None
    candidate_id: str = ""


@dataclass(frozen=True)
class RealtimeEventRecord:
    event_id: str
    event_type: str
    source: str
    payload: dict[str, object]
    severity: str = "info"
    device_id: int | None = None
    happened_at: str = ""
    ingest_status: str = "received"


@dataclass(frozen=True)
class AlertRecord:
    alert_id: str
    rule_id: str
    title: str
    severity: str
    status: str
    summary: str
    dedupe_key: str
    payload: dict[str, object]
    event_id: str = ""
    device_id: int | None = None
    occurrence_count: int = 1
    acknowledged_by: str = ""
    resolved_at: str = ""
    suppressed_until: str = ""
    suppression_reason: str = ""
    escalation_level: int = 0
    escalated_at: str = ""


@dataclass(frozen=True)
class DecisionRecord:
    decision_id: str
    policy_id: str
    policy_version: str
    action_type: str
    reason: str
    risk_level: str
    status: str
    payload: dict[str, object]
    alert_id: str = ""
    event_id: str = ""
    target_type: str = ""
    target_id: str = ""
    requires_approval: bool = False


@dataclass(frozen=True)
class ExecutionRecord:
    execution_id: str
    decision_id: str
    action_type: str
    operator: str
    status: str
    guardrail_status: str
    input_payload: dict[str, object]
    result_payload: dict[str, object]


@dataclass(frozen=True)
class RollbackRecord:
    rollback_id: str
    execution_id: str
    decision_id: str
    operator: str
    status: str
    reason: str
    result_payload: dict[str, object]


@dataclass(frozen=True)
class DecisionMetricSnapshotRecord:
    snapshot_id: str
    metric_name: str
    metric_value: float
    dimensions: dict[str, object] | None = None


@dataclass(frozen=True)
class NotificationRecord:
    notification_id: str
    source_type: str
    source_id: str
    target: str
    channel: str
    severity: str
    status: str
    title: str
    body: str


class AssistantDB:
    def __init__(self, db_path: Path) -> None:
        self.db_path = db_path
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        self._init_schema()

    @contextmanager
    def _connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        try:
            yield conn
            conn.commit()
        except Exception:
            conn.rollback()
            raise
        finally:
            conn.close()

    def _init_schema(self) -> None:
        with self._connect() as conn:
            conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS query_logs (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    question TEXT NOT NULL,
                    route_type TEXT NOT NULL,
                    sql_used TEXT NOT NULL DEFAULT '',
                    answer_summary TEXT NOT NULL DEFAULT '',
                    created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS rag_document_metadata (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    source_path TEXT NOT NULL,
                    source_type TEXT NOT NULL,
                    modified_at TEXT NOT NULL,
                    chunk_count INTEGER NOT NULL DEFAULT 0,
                    indexed_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS metric_definitions (
                    metric_name TEXT PRIMARY KEY,
                    description TEXT NOT NULL,
                    better_direction TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS tool_call_logs (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    session_id TEXT NOT NULL,
                    question TEXT NOT NULL,
                    tool_name TEXT NOT NULL,
                    tool_input TEXT NOT NULL DEFAULT '',
                    tool_output TEXT NOT NULL DEFAULT '',
                    status TEXT NOT NULL,
                    created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS approval_requests (
                    approval_id TEXT PRIMARY KEY,
                    request_type TEXT NOT NULL,
                    request_payload TEXT NOT NULL,
                    risk_summary TEXT NOT NULL,
                    status TEXT NOT NULL,
                    approved_by TEXT NOT NULL DEFAULT '',
                    approved_at TEXT NOT NULL DEFAULT '',
                    decision_note TEXT NOT NULL DEFAULT '',
                    result_payload TEXT NOT NULL DEFAULT '{}',
                    created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS action_audit_logs (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    session_id TEXT NOT NULL,
                    action_type TEXT NOT NULL,
                    operator TEXT NOT NULL DEFAULT '',
                    request_payload TEXT NOT NULL,
                    result_payload TEXT NOT NULL,
                    status TEXT NOT NULL,
                    created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS report_records (
                    report_id TEXT PRIMARY KEY,
                    report_type TEXT NOT NULL,
                    title TEXT NOT NULL,
                    device_id INTEGER,
                    task_id TEXT NOT NULL DEFAULT '',
                    summary TEXT NOT NULL,
                    markdown_path TEXT NOT NULL,
                    json_path TEXT NOT NULL,
                    created_by TEXT NOT NULL DEFAULT 'agent',
                    created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS ticket_records (
                    ticket_id TEXT PRIMARY KEY,
                    title TEXT NOT NULL,
                    severity TEXT NOT NULL,
                    status TEXT NOT NULL,
                    device_id INTEGER,
                    report_id TEXT NOT NULL DEFAULT '',
                    description TEXT NOT NULL,
                    assignee TEXT NOT NULL DEFAULT '',
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS ota_batch_runs (
                    batch_run_id TEXT PRIMARY KEY,
                    approval_id TEXT NOT NULL,
                    firmware_id TEXT NOT NULL,
                    transport TEXT NOT NULL,
                    target TEXT NOT NULL,
                    batch_size INTEGER NOT NULL,
                    batch_count INTEGER NOT NULL,
                    total_devices INTEGER NOT NULL,
                    status TEXT NOT NULL,
                    created_by TEXT NOT NULL DEFAULT 'operator',
                    summary_payload TEXT NOT NULL DEFAULT '{}',
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS ota_batch_run_items (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    batch_run_id TEXT NOT NULL,
                    batch_index INTEGER NOT NULL,
                    device_id INTEGER NOT NULL,
                    status TEXT NOT NULL,
                    task_uuid TEXT NOT NULL DEFAULT '',
                    last_error TEXT NOT NULL DEFAULT '',
                    retry_count INTEGER NOT NULL DEFAULT 0,
                    last_checked_at TEXT NOT NULL DEFAULT '',
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS eval_runs (
                    run_id TEXT PRIMARY KEY,
                    eval_type TEXT NOT NULL,
                    status TEXT NOT NULL,
                    case_count INTEGER NOT NULL DEFAULT 0,
                    summary_payload TEXT NOT NULL DEFAULT '{}',
                    baseline_run_id TEXT NOT NULL DEFAULT '',
                    created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS eval_case_results (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    run_id TEXT NOT NULL,
                    case_index INTEGER NOT NULL,
                    session_id TEXT NOT NULL,
                    question TEXT NOT NULL,
                    description TEXT NOT NULL DEFAULT '',
                    ok INTEGER NOT NULL DEFAULT 0,
                    agent_ok INTEGER NOT NULL DEFAULT 0,
                    interrupted INTEGER NOT NULL DEFAULT 0,
                    expected_tools TEXT NOT NULL DEFAULT '[]',
                    actual_tools TEXT NOT NULL DEFAULT '[]',
                    missing_tools TEXT NOT NULL DEFAULT '[]',
                    forbidden_called TEXT NOT NULL DEFAULT '[]',
                    tool_order_ok INTEGER NOT NULL DEFAULT 0,
                    answer_present INTEGER NOT NULL DEFAULT 0,
                    failure_reasons TEXT NOT NULL DEFAULT '[]',
                    result_payload TEXT NOT NULL DEFAULT '{}',
                    replay_payload TEXT NOT NULL DEFAULT '{}',
                    created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS session_memories (
                    session_id TEXT PRIMARY KEY,
                    summary TEXT NOT NULL DEFAULT '',
                    key_facts TEXT NOT NULL DEFAULT '{}',
                    last_question TEXT NOT NULL DEFAULT '',
                    last_answer TEXT NOT NULL DEFAULT '',
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS long_term_memories (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    scope TEXT NOT NULL,
                    memory_key TEXT NOT NULL,
                    content TEXT NOT NULL,
                    confidence REAL NOT NULL DEFAULT 0.5,
                    source_session_id TEXT NOT NULL DEFAULT '',
                    memory_type TEXT NOT NULL DEFAULT 'preference',
                    provenance TEXT NOT NULL DEFAULT 'manual_curated',
                    status TEXT NOT NULL DEFAULT 'active',
                    is_pinned INTEGER NOT NULL DEFAULT 0,
                    expires_at TEXT NOT NULL DEFAULT '',
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL,
                    last_used_at TEXT NOT NULL DEFAULT '',
                    UNIQUE(scope, memory_key)
                );
                CREATE TABLE IF NOT EXISTS memory_candidates (
                    candidate_id TEXT PRIMARY KEY,
                    session_id TEXT NOT NULL,
                    scope TEXT NOT NULL,
                    memory_key TEXT NOT NULL,
                    content TEXT NOT NULL,
                    rationale TEXT NOT NULL DEFAULT '',
                    confidence REAL NOT NULL DEFAULT 0.6,
                    memory_type TEXT NOT NULL DEFAULT 'fact',
                    provenance TEXT NOT NULL DEFAULT 'user_stated',
                    risk_score REAL NOT NULL DEFAULT 0.0,
                    risk_flags TEXT NOT NULL DEFAULT '[]',
                    status TEXT NOT NULL DEFAULT 'pending',
                    reviewed_by TEXT NOT NULL DEFAULT '',
                    reviewed_at TEXT NOT NULL DEFAULT '',
                    first_reviewed_by TEXT NOT NULL DEFAULT '',
                    first_reviewed_at TEXT NOT NULL DEFAULT '',
                    final_reviewed_by TEXT NOT NULL DEFAULT '',
                    final_reviewed_at TEXT NOT NULL DEFAULT '',
                    decision_note TEXT NOT NULL DEFAULT '',
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS memory_audit_logs (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    memory_scope TEXT NOT NULL DEFAULT 'global',
                    memory_key TEXT NOT NULL DEFAULT '',
                    memory_id INTEGER,
                    candidate_id TEXT NOT NULL DEFAULT '',
                    action_type TEXT NOT NULL,
                    actor TEXT NOT NULL DEFAULT '',
                    actor_role TEXT NOT NULL DEFAULT '',
                    before_state TEXT NOT NULL DEFAULT '{}',
                    after_state TEXT NOT NULL DEFAULT '{}',
                    reason TEXT NOT NULL DEFAULT '',
                    created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS realtime_events (
                    event_id TEXT PRIMARY KEY,
                    event_type TEXT NOT NULL,
                    source TEXT NOT NULL DEFAULT 'api',
                    severity TEXT NOT NULL DEFAULT 'info',
                    device_id INTEGER,
                    payload TEXT NOT NULL DEFAULT '{}',
                    happened_at TEXT NOT NULL,
                    ingest_status TEXT NOT NULL DEFAULT 'received',
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS alert_records (
                    alert_id TEXT PRIMARY KEY,
                    rule_id TEXT NOT NULL,
                    title TEXT NOT NULL,
                    severity TEXT NOT NULL,
                    status TEXT NOT NULL,
                    summary TEXT NOT NULL,
                    dedupe_key TEXT NOT NULL,
                    payload TEXT NOT NULL DEFAULT '{}',
                    event_id TEXT NOT NULL DEFAULT '',
                    device_id INTEGER,
                    occurrence_count INTEGER NOT NULL DEFAULT 1,
                    acknowledged_by TEXT NOT NULL DEFAULT '',
                    resolved_at TEXT NOT NULL DEFAULT '',
                    suppressed_until TEXT NOT NULL DEFAULT '',
                    suppression_reason TEXT NOT NULL DEFAULT '',
                    escalation_level INTEGER NOT NULL DEFAULT 0,
                    escalated_at TEXT NOT NULL DEFAULT '',
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS decision_records (
                    decision_id TEXT PRIMARY KEY,
                    policy_id TEXT NOT NULL,
                    policy_version TEXT NOT NULL DEFAULT 'v1',
                    action_type TEXT NOT NULL,
                    reason TEXT NOT NULL,
                    risk_level TEXT NOT NULL,
                    status TEXT NOT NULL,
                    payload TEXT NOT NULL DEFAULT '{}',
                    alert_id TEXT NOT NULL DEFAULT '',
                    event_id TEXT NOT NULL DEFAULT '',
                    target_type TEXT NOT NULL DEFAULT '',
                    target_id TEXT NOT NULL DEFAULT '',
                    requires_approval INTEGER NOT NULL DEFAULT 0,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS execution_records (
                    execution_id TEXT PRIMARY KEY,
                    decision_id TEXT NOT NULL,
                    action_type TEXT NOT NULL,
                    operator TEXT NOT NULL DEFAULT '',
                    status TEXT NOT NULL,
                    guardrail_status TEXT NOT NULL DEFAULT '',
                    input_payload TEXT NOT NULL DEFAULT '{}',
                    result_payload TEXT NOT NULL DEFAULT '{}',
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS rollback_records (
                    rollback_id TEXT PRIMARY KEY,
                    execution_id TEXT NOT NULL,
                    decision_id TEXT NOT NULL,
                    operator TEXT NOT NULL DEFAULT '',
                    status TEXT NOT NULL,
                    reason TEXT NOT NULL DEFAULT '',
                    result_payload TEXT NOT NULL DEFAULT '{}',
                    created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS decision_metric_snapshots (
                    snapshot_id TEXT PRIMARY KEY,
                    metric_name TEXT NOT NULL,
                    metric_value REAL NOT NULL DEFAULT 0,
                    dimensions TEXT NOT NULL DEFAULT '{}',
                    created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS notification_records (
                    notification_id TEXT PRIMARY KEY,
                    source_type TEXT NOT NULL,
                    source_id TEXT NOT NULL,
                    target TEXT NOT NULL,
                    channel TEXT NOT NULL DEFAULT 'console',
                    severity TEXT NOT NULL,
                    status TEXT NOT NULL,
                    title TEXT NOT NULL,
                    body TEXT NOT NULL,
                    created_at TEXT NOT NULL
                );
                """
            )
            conn.executemany(
                """
                INSERT INTO metric_definitions(metric_name, description, better_direction)
                VALUES(?, ?, ?)
                ON CONFLICT(metric_name) DO UPDATE SET
                    description=excluded.description,
                    better_direction=excluded.better_direction
                """,
                [
                    ("system_event_count", "系统事件数量，需结合事件类型判断好坏", "depends"),
                    ("sensor_event_count", "传感器上报数量，过低可能代表设备离线或链路异常", "higher"),
                    ("ota_failed_count", "OTA 失败次数，越低越好", "lower"),
                    ("crc_error_count", "CRC 错误次数，越低越好，仅在系统存在对应事件时有效", "lower"),
                ],
            )
            self._ensure_column(conn, "approval_requests", "decision_note", "TEXT NOT NULL DEFAULT ''")
            self._ensure_column(conn, "approval_requests", "result_payload", "TEXT NOT NULL DEFAULT '{}'")
            self._ensure_column(conn, "long_term_memories", "memory_type", "TEXT NOT NULL DEFAULT 'preference'")
            self._ensure_column(conn, "long_term_memories", "provenance", "TEXT NOT NULL DEFAULT 'manual_curated'")
            self._ensure_column(conn, "long_term_memories", "status", "TEXT NOT NULL DEFAULT 'active'")
            self._ensure_column(conn, "long_term_memories", "is_pinned", "INTEGER NOT NULL DEFAULT 0")
            self._ensure_column(conn, "long_term_memories", "expires_at", "TEXT NOT NULL DEFAULT ''")
            self._ensure_column(conn, "memory_candidates", "provenance", "TEXT NOT NULL DEFAULT 'user_stated'")
            self._ensure_column(conn, "memory_candidates", "risk_score", "REAL NOT NULL DEFAULT 0.0")
            self._ensure_column(conn, "memory_candidates", "risk_flags", "TEXT NOT NULL DEFAULT '[]'")
            self._ensure_column(conn, "memory_candidates", "first_reviewed_by", "TEXT NOT NULL DEFAULT ''")
            self._ensure_column(conn, "memory_candidates", "first_reviewed_at", "TEXT NOT NULL DEFAULT ''")
            self._ensure_column(conn, "memory_candidates", "final_reviewed_by", "TEXT NOT NULL DEFAULT ''")
            self._ensure_column(conn, "memory_candidates", "final_reviewed_at", "TEXT NOT NULL DEFAULT ''")
            self._ensure_column(conn, "alert_records", "acknowledged_by", "TEXT NOT NULL DEFAULT ''")
            self._ensure_column(conn, "alert_records", "resolved_at", "TEXT NOT NULL DEFAULT ''")
            self._ensure_column(conn, "alert_records", "suppressed_until", "TEXT NOT NULL DEFAULT ''")
            self._ensure_column(conn, "alert_records", "suppression_reason", "TEXT NOT NULL DEFAULT ''")
            self._ensure_column(conn, "alert_records", "escalation_level", "INTEGER NOT NULL DEFAULT 0")
            self._ensure_column(conn, "alert_records", "escalated_at", "TEXT NOT NULL DEFAULT ''")
            self._ensure_column(conn, "decision_records", "policy_version", "TEXT NOT NULL DEFAULT 'v1'")

    def log_query(self, record: QueryLogRecord) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO query_logs(question, route_type, sql_used, answer_summary, created_at)
                VALUES(?, ?, ?, ?, ?)
                """,
                (
                    record.question,
                    record.route_type,
                    record.sql_used,
                    record.answer_summary,
                    datetime.now(timezone.utc).isoformat(),
                ),
            )

    def log_tool_call(self, record: ToolCallLogRecord) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO tool_call_logs(
                    session_id, question, tool_name, tool_input, tool_output, status, created_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.session_id,
                    record.question,
                    record.tool_name,
                    json.dumps(record.tool_input, ensure_ascii=False, sort_keys=True),
                    json.dumps(record.tool_output, ensure_ascii=False, sort_keys=True),
                    record.status,
                    datetime.now(timezone.utc).isoformat(),
                ),
            )

    def replace_rag_metadata(self, rows: list[tuple[str, str, str, int, str]]) -> None:
        with self._connect() as conn:
            conn.execute("DELETE FROM rag_document_metadata")
            conn.executemany(
                """
                INSERT INTO rag_document_metadata(source_path, source_type, modified_at, chunk_count, indexed_at)
                VALUES(?, ?, ?, ?, ?)
                """,
                rows,
            )

    def create_approval_request(self, record: ApprovalRequestRecord) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO approval_requests(
                    approval_id, request_type, request_payload, risk_summary,
                    status, approved_by, approved_at, decision_note, result_payload, created_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.approval_id,
                    record.request_type,
                    json.dumps(record.request_payload, ensure_ascii=False, sort_keys=True),
                    record.risk_summary,
                    record.status,
                    record.approved_by,
                    record.approved_at,
                    record.decision_note,
                    json.dumps(record.result_payload or {}, ensure_ascii=False, sort_keys=True),
                    datetime.now(timezone.utc).isoformat(),
                ),
            )

    def get_approval_request(self, approval_id: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT approval_id, request_type, request_payload, risk_summary,
                       status, approved_by, approved_at, decision_note, result_payload, created_at
                FROM approval_requests
                WHERE approval_id = ?
                LIMIT 1
                """,
                (approval_id,),
            ).fetchone()
        if row is None:
            return None
        return self._decode_approval_row(dict(row))

    def update_approval_request_status(
        self,
        approval_id: str,
        status: str,
        approved_by: str = "",
        approved_at: str = "",
        decision_note: str = "",
        result_payload: dict[str, object] | None = None,
    ) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                UPDATE approval_requests
                SET status = ?, approved_by = ?, approved_at = ?, decision_note = ?, result_payload = ?
                WHERE approval_id = ?
                """,
                (
                    status,
                    approved_by,
                    approved_at,
                    decision_note,
                    json.dumps(result_payload or {}, ensure_ascii=False, sort_keys=True),
                    approval_id,
                ),
            )

    def list_approval_requests(self, limit: int = 20, status: str = "") -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if status.strip():
            clauses.append("status = ?")
            params.append(status.strip())
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 100)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT approval_id, request_type, request_payload, risk_summary,
                       status, approved_by, approved_at, decision_note, result_payload, created_at
                FROM approval_requests
                {where_sql}
                ORDER BY created_at DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [self._decode_approval_row(dict(row)) for row in rows]

    def log_action_audit(self, record: ActionAuditLogRecord) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO action_audit_logs(
                    session_id, action_type, operator, request_payload, result_payload, status, created_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.session_id,
                    record.action_type,
                    record.operator,
                    json.dumps(record.request_payload, ensure_ascii=False, sort_keys=True),
                    json.dumps(record.result_payload, ensure_ascii=False, sort_keys=True),
                    record.status,
                    datetime.now(timezone.utc).isoformat(),
                ),
            )

    def list_action_audits(self, limit: int = 50, session_id: str = "") -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if session_id.strip():
            clauses.append("session_id = ?")
            params.append(session_id.strip())
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 200)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT id, session_id, action_type, operator, request_payload,
                       result_payload, status, created_at
                FROM action_audit_logs
                {where_sql}
                ORDER BY id DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [self._decode_action_audit_row(dict(row)) for row in rows]

    def log_memory_audit(self, record: MemoryAuditLogRecord) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO memory_audit_logs(
                    memory_scope, memory_key, memory_id, candidate_id, action_type,
                    actor, actor_role, before_state, after_state, reason, created_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.memory_scope,
                    record.memory_key,
                    record.memory_id,
                    record.candidate_id,
                    record.action_type,
                    record.actor,
                    record.actor_role,
                    json.dumps(record.before_state, ensure_ascii=False, sort_keys=True),
                    json.dumps(record.after_state, ensure_ascii=False, sort_keys=True),
                    record.reason,
                    datetime.now(timezone.utc).isoformat(),
                ),
            )

    def list_memory_audits(
        self,
        limit: int = 50,
        memory_key: str = "",
        actor: str = "",
        action_type: str = "",
    ) -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if memory_key.strip():
            clauses.append("memory_key = ?")
            params.append(memory_key.strip())
        if actor.strip():
            clauses.append("actor = ?")
            params.append(actor.strip())
        if action_type.strip():
            clauses.append("action_type = ?")
            params.append(action_type.strip())
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 200)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT id, memory_scope, memory_key, memory_id, candidate_id, action_type,
                       actor, actor_role, before_state, after_state, reason, created_at
                FROM memory_audit_logs
                {where_sql}
                ORDER BY id DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [self._decode_memory_audit_row(dict(row)) for row in rows]

    def create_realtime_event(self, record: RealtimeEventRecord) -> None:
        now = datetime.now(timezone.utc).isoformat()
        happened_at = record.happened_at or now
        with self._connect() as conn:
            conn.execute(
                """
                INSERT OR REPLACE INTO realtime_events(
                    event_id, event_type, source, severity, device_id, payload,
                    happened_at, ingest_status, created_at, updated_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.event_id,
                    record.event_type,
                    record.source,
                    record.severity,
                    record.device_id,
                    json.dumps(record.payload, ensure_ascii=False, sort_keys=True),
                    happened_at,
                    record.ingest_status,
                    now,
                    now,
                ),
            )

    def list_realtime_events(
        self,
        limit: int = 50,
        event_type: str = "",
        severity: str = "",
        device_id: int | None = None,
    ) -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if event_type.strip():
            clauses.append("event_type = ?")
            params.append(event_type.strip())
        if severity.strip():
            clauses.append("severity = ?")
            params.append(severity.strip())
        if device_id is not None:
            clauses.append("device_id = ?")
            params.append(device_id)
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 200)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT event_id, event_type, source, severity, device_id, payload,
                       happened_at, ingest_status, created_at, updated_at
                FROM realtime_events
                {where_sql}
                ORDER BY happened_at DESC, created_at DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [self._decode_realtime_event_row(dict(row)) for row in rows]

    def get_realtime_event(self, event_id: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT event_id, event_type, source, severity, device_id, payload,
                       happened_at, ingest_status, created_at, updated_at
                FROM realtime_events
                WHERE event_id = ?
                LIMIT 1
                """,
                (event_id,),
            ).fetchone()
        return self._decode_realtime_event_row(dict(row)) if row is not None else None

    def get_open_alert_by_dedupe_key(self, dedupe_key: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT alert_id, rule_id, title, severity, status, summary, dedupe_key,
                       payload, event_id, device_id, occurrence_count, acknowledged_by,
                       resolved_at, suppressed_until, suppression_reason, escalation_level,
                       escalated_at, created_at, updated_at
                FROM alert_records
                WHERE dedupe_key = ? AND status IN ('open', 'acknowledged', 'suppressed')
                ORDER BY updated_at DESC
                LIMIT 1
                """,
                (dedupe_key,),
            ).fetchone()
        return self._decode_alert_row(dict(row)) if row is not None else None

    def create_alert_record(self, record: AlertRecord) -> None:
        now = datetime.now(timezone.utc).isoformat()
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO alert_records(
                    alert_id, rule_id, title, severity, status, summary, dedupe_key,
                    payload, event_id, device_id, occurrence_count, acknowledged_by,
                    resolved_at, suppressed_until, suppression_reason, escalation_level,
                    escalated_at, created_at, updated_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.alert_id,
                    record.rule_id,
                    record.title,
                    record.severity,
                    record.status,
                    record.summary,
                    record.dedupe_key,
                    json.dumps(record.payload, ensure_ascii=False, sort_keys=True),
                    record.event_id,
                    record.device_id,
                    record.occurrence_count,
                    record.acknowledged_by,
                    record.resolved_at,
                    record.suppressed_until,
                    record.suppression_reason,
                    record.escalation_level,
                    record.escalated_at,
                    now,
                    now,
                ),
            )

    def update_alert_record(
        self,
        alert_id: str,
        *,
        status: str | None = None,
        summary: str | None = None,
        payload: dict[str, object] | None = None,
        event_id: str | None = None,
        severity: str | None = None,
        acknowledged_by: str | None = None,
        resolved_at: str | None = None,
        suppressed_until: str | None = None,
        suppression_reason: str | None = None,
        escalation_level: int | None = None,
        escalated_at: str | None = None,
        occurrence_count: int | None = None,
    ) -> dict[str, object] | None:
        current = self.get_alert_record(alert_id)
        if current is None:
            return None
        updated = {
            "status": current.get("status", "open"),
            "summary": current.get("summary", ""),
            "payload": current.get("payload", {}),
            "event_id": current.get("event_id", ""),
            "severity": current.get("severity", "medium"),
            "acknowledged_by": current.get("acknowledged_by", ""),
            "resolved_at": current.get("resolved_at", ""),
            "suppressed_until": current.get("suppressed_until", ""),
            "suppression_reason": current.get("suppression_reason", ""),
            "escalation_level": current.get("escalation_level", 0),
            "escalated_at": current.get("escalated_at", ""),
            "occurrence_count": current.get("occurrence_count", 1),
        }
        if status is not None:
            updated["status"] = status
        if summary is not None:
            updated["summary"] = summary
        if payload is not None:
            updated["payload"] = payload
        if event_id is not None:
            updated["event_id"] = event_id
        if severity is not None:
            updated["severity"] = severity
        if acknowledged_by is not None:
            updated["acknowledged_by"] = acknowledged_by
        if resolved_at is not None:
            updated["resolved_at"] = resolved_at
        if suppressed_until is not None:
            updated["suppressed_until"] = suppressed_until
        if suppression_reason is not None:
            updated["suppression_reason"] = suppression_reason
        if escalation_level is not None:
            updated["escalation_level"] = escalation_level
        if escalated_at is not None:
            updated["escalated_at"] = escalated_at
        if occurrence_count is not None:
            updated["occurrence_count"] = occurrence_count
        with self._connect() as conn:
            conn.execute(
                """
                UPDATE alert_records
                SET severity = ?, status = ?, summary = ?, payload = ?, event_id = ?,
                    acknowledged_by = ?, resolved_at = ?, suppressed_until = ?, suppression_reason = ?,
                    escalation_level = ?, escalated_at = ?, occurrence_count = ?, updated_at = ?
                WHERE alert_id = ?
                """,
                (
                    updated["severity"],
                    updated["status"],
                    updated["summary"],
                    json.dumps(updated["payload"], ensure_ascii=False, sort_keys=True),
                    updated["event_id"],
                    updated["acknowledged_by"],
                    updated["resolved_at"],
                    updated["suppressed_until"],
                    updated["suppression_reason"],
                    int(updated["escalation_level"]),
                    updated["escalated_at"],
                    int(updated["occurrence_count"]),
                    datetime.now(timezone.utc).isoformat(),
                    alert_id,
                ),
            )
        return self.get_alert_record(alert_id)

    def get_alert_record(self, alert_id: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT alert_id, rule_id, title, severity, status, summary, dedupe_key,
                       payload, event_id, device_id, occurrence_count, acknowledged_by,
                       resolved_at, suppressed_until, suppression_reason, escalation_level,
                       escalated_at, created_at, updated_at
                FROM alert_records
                WHERE alert_id = ?
                LIMIT 1
                """,
                (alert_id,),
            ).fetchone()
        return self._decode_alert_row(dict(row)) if row is not None else None

    def list_alert_records(self, limit: int = 50, status: str = "", severity: str = "") -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if status.strip():
            clauses.append("status = ?")
            params.append(status.strip())
        if severity.strip():
            clauses.append("severity = ?")
            params.append(severity.strip())
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 200)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT alert_id, rule_id, title, severity, status, summary, dedupe_key,
                       payload, event_id, device_id, occurrence_count, acknowledged_by,
                       resolved_at, suppressed_until, suppression_reason, escalation_level,
                       escalated_at, created_at, updated_at
                FROM alert_records
                {where_sql}
                ORDER BY updated_at DESC, created_at DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [self._decode_alert_row(dict(row)) for row in rows]

    def get_active_decision_by_policy(self, alert_id: str, policy_id: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT decision_id, policy_id, action_type, reason, risk_level, status,
                       policy_version, payload, alert_id, event_id, target_type, target_id, requires_approval,
                       created_at, updated_at
                FROM decision_records
                WHERE alert_id = ? AND policy_id = ? AND status IN ('proposed', 'approved', 'executing')
                ORDER BY updated_at DESC
                LIMIT 1
                """,
                (alert_id, policy_id),
            ).fetchone()
        return self._decode_decision_row(dict(row)) if row is not None else None

    def create_decision_record(self, record: DecisionRecord) -> None:
        now = datetime.now(timezone.utc).isoformat()
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO decision_records(
                    decision_id, policy_id, policy_version, action_type, reason, risk_level, status,
                    payload, alert_id, event_id, target_type, target_id, requires_approval,
                    created_at, updated_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.decision_id,
                    record.policy_id,
                    record.policy_version,
                    record.action_type,
                    record.reason,
                    record.risk_level,
                    record.status,
                    json.dumps(record.payload, ensure_ascii=False, sort_keys=True),
                    record.alert_id,
                    record.event_id,
                    record.target_type,
                    record.target_id,
                    int(record.requires_approval),
                    now,
                    now,
                ),
            )

    def update_decision_record(
        self,
        decision_id: str,
        *,
        status: str | None = None,
        payload: dict[str, object] | None = None,
        reason: str | None = None,
    ) -> dict[str, object] | None:
        current = self.get_decision_record(decision_id)
        if current is None:
            return None
        updated = {
            "status": current.get("status", "proposed"),
            "payload": current.get("payload", {}),
            "reason": current.get("reason", ""),
        }
        if status is not None:
            updated["status"] = status
        if payload is not None:
            updated["payload"] = payload
        if reason is not None:
            updated["reason"] = reason
        with self._connect() as conn:
            conn.execute(
                """
                UPDATE decision_records
                SET status = ?, payload = ?, reason = ?, updated_at = ?
                WHERE decision_id = ?
                """,
                (
                    updated["status"],
                    json.dumps(updated["payload"], ensure_ascii=False, sort_keys=True),
                    updated["reason"],
                    datetime.now(timezone.utc).isoformat(),
                    decision_id,
                ),
            )
        return self.get_decision_record(decision_id)

    def get_decision_record(self, decision_id: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT decision_id, policy_id, action_type, reason, risk_level, status,
                       policy_version, payload, alert_id, event_id, target_type, target_id, requires_approval,
                       created_at, updated_at
                FROM decision_records
                WHERE decision_id = ?
                LIMIT 1
                """,
                (decision_id,),
            ).fetchone()
        return self._decode_decision_row(dict(row)) if row is not None else None

    def list_decision_records(self, limit: int = 50, status: str = "", risk_level: str = "") -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if status.strip():
            clauses.append("status = ?")
            params.append(status.strip())
        if risk_level.strip():
            clauses.append("risk_level = ?")
            params.append(risk_level.strip())
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 200)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT decision_id, policy_id, action_type, reason, risk_level, status,
                       policy_version, payload, alert_id, event_id, target_type, target_id, requires_approval,
                       created_at, updated_at
                FROM decision_records
                {where_sql}
                ORDER BY updated_at DESC, created_at DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [self._decode_decision_row(dict(row)) for row in rows]

    def create_execution_record(self, record: ExecutionRecord) -> None:
        now = datetime.now(timezone.utc).isoformat()
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO execution_records(
                    execution_id, decision_id, action_type, operator, status, guardrail_status,
                    input_payload, result_payload, created_at, updated_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.execution_id,
                    record.decision_id,
                    record.action_type,
                    record.operator,
                    record.status,
                    record.guardrail_status,
                    json.dumps(record.input_payload, ensure_ascii=False, sort_keys=True),
                    json.dumps(record.result_payload, ensure_ascii=False, sort_keys=True),
                    now,
                    now,
                ),
            )

    def update_execution_record(
        self,
        execution_id: str,
        *,
        status: str | None = None,
        guardrail_status: str | None = None,
        result_payload: dict[str, object] | None = None,
    ) -> dict[str, object] | None:
        current = self.get_execution_record(execution_id)
        if current is None:
            return None
        updated = {
            "status": current.get("status", ""),
            "guardrail_status": current.get("guardrail_status", ""),
            "result_payload": current.get("result_payload", {}),
        }
        if status is not None:
            updated["status"] = status
        if guardrail_status is not None:
            updated["guardrail_status"] = guardrail_status
        if result_payload is not None:
            updated["result_payload"] = result_payload
        with self._connect() as conn:
            conn.execute(
                """
                UPDATE execution_records
                SET status = ?, guardrail_status = ?, result_payload = ?, updated_at = ?
                WHERE execution_id = ?
                """,
                (
                    updated["status"],
                    updated["guardrail_status"],
                    json.dumps(updated["result_payload"], ensure_ascii=False, sort_keys=True),
                    datetime.now(timezone.utc).isoformat(),
                    execution_id,
                ),
            )
        return self.get_execution_record(execution_id)

    def get_execution_record(self, execution_id: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT execution_id, decision_id, action_type, operator, status, guardrail_status,
                       input_payload, result_payload, created_at, updated_at
                FROM execution_records
                WHERE execution_id = ?
                LIMIT 1
                """,
                (execution_id,),
            ).fetchone()
        return self._decode_execution_row(dict(row)) if row is not None else None

    def list_execution_records(self, limit: int = 50, status: str = "") -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if status.strip():
            clauses.append("status = ?")
            params.append(status.strip())
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 200)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT execution_id, decision_id, action_type, operator, status, guardrail_status,
                       input_payload, result_payload, created_at, updated_at
                FROM execution_records
                {where_sql}
                ORDER BY updated_at DESC, created_at DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [self._decode_execution_row(dict(row)) for row in rows]

    def get_latest_execution_for_decision(self, decision_id: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT execution_id, decision_id, action_type, operator, status, guardrail_status,
                       input_payload, result_payload, created_at, updated_at
                FROM execution_records
                WHERE decision_id = ?
                ORDER BY updated_at DESC, created_at DESC
                LIMIT 1
                """,
                (decision_id,),
            ).fetchone()
        return self._decode_execution_row(dict(row)) if row is not None else None

    def create_rollback_record(self, record: RollbackRecord) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO rollback_records(
                    rollback_id, execution_id, decision_id, operator, status, reason, result_payload, created_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.rollback_id,
                    record.execution_id,
                    record.decision_id,
                    record.operator,
                    record.status,
                    record.reason,
                    json.dumps(record.result_payload, ensure_ascii=False, sort_keys=True),
                    datetime.now(timezone.utc).isoformat(),
                ),
            )

    def list_rollback_records(self, limit: int = 50, status: str = "") -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if status.strip():
            clauses.append("status = ?")
            params.append(status.strip())
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 200)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT rollback_id, execution_id, decision_id, operator, status, reason, result_payload, created_at
                FROM rollback_records
                {where_sql}
                ORDER BY created_at DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [self._decode_rollback_row(dict(row)) for row in rows]

    def create_decision_metric_snapshot(self, record: DecisionMetricSnapshotRecord) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO decision_metric_snapshots(
                    snapshot_id, metric_name, metric_value, dimensions, created_at
                )
                VALUES(?, ?, ?, ?, ?)
                """,
                (
                    record.snapshot_id,
                    record.metric_name,
                    float(record.metric_value),
                    json.dumps(record.dimensions or {}, ensure_ascii=False, sort_keys=True),
                    datetime.now(timezone.utc).isoformat(),
                ),
            )

    def list_decision_metric_snapshots(self, limit: int = 100, metric_name: str = "") -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if metric_name.strip():
            clauses.append("metric_name = ?")
            params.append(metric_name.strip())
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 500)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT snapshot_id, metric_name, metric_value, dimensions, created_at
                FROM decision_metric_snapshots
                {where_sql}
                ORDER BY created_at DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [self._decode_decision_metric_snapshot_row(dict(row)) for row in rows]

    def create_notification_record(self, record: NotificationRecord) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO notification_records(
                    notification_id, source_type, source_id, target, channel, severity, status, title, body, created_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.notification_id,
                    record.source_type,
                    record.source_id,
                    record.target,
                    record.channel,
                    record.severity,
                    record.status,
                    record.title,
                    record.body,
                    datetime.now(timezone.utc).isoformat(),
                ),
            )

    def list_notification_records(self, limit: int = 50, status: str = "", severity: str = "") -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if status.strip():
            clauses.append("status = ?")
            params.append(status.strip())
        if severity.strip():
            clauses.append("severity = ?")
            params.append(severity.strip())
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 200)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT notification_id, source_type, source_id, target, channel, severity, status, title, body, created_at
                FROM notification_records
                {where_sql}
                ORDER BY created_at DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [dict(row) for row in rows]

    def create_report_record(self, record: ReportRecord) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO report_records(
                    report_id, report_type, title, device_id, task_id, summary,
                    markdown_path, json_path, created_by, created_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.report_id,
                    record.report_type,
                    record.title,
                    record.device_id,
                    record.task_id,
                    record.summary,
                    record.markdown_path,
                    record.json_path,
                    record.created_by,
                    datetime.now(timezone.utc).isoformat(),
                ),
            )

    def list_report_records(self, limit: int = 20) -> list[dict[str, object]]:
        with self._connect() as conn:
            rows = conn.execute(
                """
                SELECT report_id, report_type, title, device_id, task_id, summary,
                       markdown_path, json_path, created_by, created_at
                FROM report_records
                ORDER BY created_at DESC
                LIMIT ?
                """,
                (max(1, min(limit, 100)),),
            ).fetchall()
        return [dict(row) for row in rows]

    def get_report_record(self, report_id: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT report_id, report_type, title, device_id, task_id, summary,
                       markdown_path, json_path, created_by, created_at
                FROM report_records
                WHERE report_id = ?
                LIMIT 1
                """,
                (report_id,),
            ).fetchone()
        return self._decode_memory_candidate_row(dict(row)) if row is not None else None

    def create_ticket_record(self, record: TicketRecord) -> None:
        now = datetime.now(timezone.utc).isoformat()
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO ticket_records(
                    ticket_id, title, severity, status, device_id, report_id,
                    description, assignee, created_at, updated_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.ticket_id,
                    record.title,
                    record.severity,
                    record.status,
                    record.device_id,
                    record.report_id,
                    record.description,
                    record.assignee,
                    now,
                    now,
                ),
            )

    def list_ticket_records(
        self,
        limit: int = 20,
        status: str = "",
        severity: str = "",
        assignee: str = "",
    ) -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if status.strip():
            clauses.append("status = ?")
            params.append(status.strip())
        if severity.strip():
            clauses.append("severity = ?")
            params.append(severity.strip())
        if assignee.strip():
            clauses.append("assignee = ?")
            params.append(assignee.strip())
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 100)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT ticket_id, title, severity, status, device_id, report_id,
                       description, assignee, created_at, updated_at
                FROM ticket_records
                {where_sql}
                ORDER BY updated_at DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [self._decode_memory_candidate_row(dict(row)) for row in rows]

    def get_ticket_record(self, ticket_id: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT ticket_id, title, severity, status, device_id, report_id,
                       description, assignee, created_at, updated_at
                FROM ticket_records
                WHERE ticket_id = ?
                LIMIT 1
                """,
                (ticket_id,),
            ).fetchone()
        return self._decode_memory_candidate_row(dict(row)) if row is not None else None

    def update_ticket_assignment(self, ticket_id: str, assignee: str) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                UPDATE ticket_records
                SET assignee = ?, updated_at = ?
                WHERE ticket_id = ?
                """,
                (assignee, datetime.now(timezone.utc).isoformat(), ticket_id),
            )

    def update_ticket_status(self, ticket_id: str, status: str) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                UPDATE ticket_records
                SET status = ?, updated_at = ?
                WHERE ticket_id = ?
                """,
                (status, datetime.now(timezone.utc).isoformat(), ticket_id),
            )

    def list_tool_call_logs(self, limit: int = 50, session_id: str = "") -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if session_id.strip():
            clauses.append("session_id = ?")
            params.append(session_id.strip())
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 200)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT id, session_id, question, tool_name, tool_input, tool_output, status, created_at
                FROM tool_call_logs
                {where_sql}
                ORDER BY id DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [self._decode_tool_call_row(dict(row)) for row in rows]

    def create_ota_batch_run(self, record: OtaBatchRunRecord) -> None:
        now = datetime.now(timezone.utc).isoformat()
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO ota_batch_runs(
                    batch_run_id, approval_id, firmware_id, transport, target,
                    batch_size, batch_count, total_devices, status, created_by,
                    summary_payload, created_at, updated_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.batch_run_id,
                    record.approval_id,
                    record.firmware_id,
                    record.transport,
                    record.target,
                    record.batch_size,
                    record.batch_count,
                    record.total_devices,
                    record.status,
                    record.created_by,
                    json.dumps(record.summary_payload or {}, ensure_ascii=False, sort_keys=True),
                    now,
                    now,
                ),
            )

    def update_ota_batch_run_status(
        self,
        batch_run_id: str,
        status: str,
        summary_payload: dict[str, object] | None = None,
    ) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                UPDATE ota_batch_runs
                SET status = ?, summary_payload = ?, updated_at = ?
                WHERE batch_run_id = ?
                """,
                (
                    status,
                    json.dumps(summary_payload or {}, ensure_ascii=False, sort_keys=True),
                    datetime.now(timezone.utc).isoformat(),
                    batch_run_id,
                ),
            )

    def list_ota_batch_runs(self, limit: int = 20) -> list[dict[str, object]]:
        with self._connect() as conn:
            rows = conn.execute(
                """
                SELECT batch_run_id, approval_id, firmware_id, transport, target,
                       batch_size, batch_count, total_devices, status, created_by,
                       summary_payload, created_at, updated_at
                FROM ota_batch_runs
                ORDER BY updated_at DESC
                LIMIT ?
                """,
                (max(1, min(limit, 100)),),
            ).fetchall()
        return [self._decode_batch_run_row(dict(row)) for row in rows]

    def get_ota_batch_run(self, batch_run_id: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT batch_run_id, approval_id, firmware_id, transport, target,
                       batch_size, batch_count, total_devices, status, created_by,
                       summary_payload, created_at, updated_at
                FROM ota_batch_runs
                WHERE batch_run_id = ?
                LIMIT 1
                """,
                (batch_run_id,),
            ).fetchone()
        if row is None:
            return None
        return self._decode_batch_run_row(dict(row))

    def create_ota_batch_run_items(self, records: list[OtaBatchRunItemRecord]) -> None:
        if not records:
            return
        now = datetime.now(timezone.utc).isoformat()
        with self._connect() as conn:
            conn.executemany(
                """
                INSERT INTO ota_batch_run_items(
                    batch_run_id, batch_index, device_id, status, task_uuid,
                    last_error, retry_count, last_checked_at, created_at, updated_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                [
                    (
                        record.batch_run_id,
                        record.batch_index,
                        record.device_id,
                        record.status,
                        record.task_uuid,
                        record.last_error,
                        record.retry_count,
                        record.last_checked_at,
                        now,
                        now,
                    )
                    for record in records
                ],
            )

    def list_ota_batch_run_items(self, batch_run_id: str) -> list[dict[str, object]]:
        with self._connect() as conn:
            rows = conn.execute(
                """
                SELECT id, batch_run_id, batch_index, device_id, status, task_uuid,
                       last_error, retry_count, last_checked_at, created_at, updated_at
                FROM ota_batch_run_items
                WHERE batch_run_id = ?
                ORDER BY batch_index ASC, id ASC
                """,
                (batch_run_id,),
            ).fetchall()
        return [dict(row) for row in rows]

    def create_eval_run(self, record: EvalRunRecord) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO eval_runs(
                    run_id, eval_type, status, case_count, summary_payload, baseline_run_id, created_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    record.run_id,
                    record.eval_type,
                    record.status,
                    record.case_count,
                    json.dumps(record.summary_payload, ensure_ascii=False, sort_keys=True),
                    record.baseline_run_id,
                    datetime.now(timezone.utc).isoformat(),
                ),
            )

    def create_eval_case_results(self, records: list[EvalCaseResultRecord]) -> None:
        if not records:
            return
        now = datetime.now(timezone.utc).isoformat()
        with self._connect() as conn:
            conn.executemany(
                """
                INSERT INTO eval_case_results(
                    run_id, case_index, session_id, question, description,
                    ok, agent_ok, interrupted, expected_tools, actual_tools, missing_tools,
                    forbidden_called, tool_order_ok, answer_present, failure_reasons,
                    result_payload, replay_payload, created_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                [
                    (
                        record.run_id,
                        record.case_index,
                        record.session_id,
                        record.question,
                        record.description,
                        int(record.ok),
                        int(record.agent_ok),
                        int(record.interrupted),
                        json.dumps(record.expected_tools, ensure_ascii=False, sort_keys=True),
                        json.dumps(record.actual_tools, ensure_ascii=False, sort_keys=True),
                        json.dumps(record.missing_tools, ensure_ascii=False, sort_keys=True),
                        json.dumps(record.forbidden_called, ensure_ascii=False, sort_keys=True),
                        int(record.tool_order_ok),
                        int(record.answer_present),
                        json.dumps(record.failure_reasons, ensure_ascii=False, sort_keys=True),
                        json.dumps(record.result_payload, ensure_ascii=False, sort_keys=True),
                        json.dumps(record.replay_payload, ensure_ascii=False, sort_keys=True),
                        now,
                    )
                    for record in records
                ],
            )

    def list_eval_runs(self, limit: int = 20, eval_type: str = "") -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if eval_type.strip():
            clauses.append("eval_type = ?")
            params.append(eval_type.strip())
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 100)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT run_id, eval_type, status, case_count, summary_payload, baseline_run_id, created_at
                FROM eval_runs
                {where_sql}
                ORDER BY created_at DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [self._decode_eval_run_row(dict(row)) for row in rows]

    def get_eval_run(self, run_id: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT run_id, eval_type, status, case_count, summary_payload, baseline_run_id, created_at
                FROM eval_runs
                WHERE run_id = ?
                LIMIT 1
                """,
                (run_id,),
            ).fetchone()
        if row is None:
            return None
        return self._decode_eval_run_row(dict(row))

    def get_latest_eval_run(self, eval_type: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT run_id, eval_type, status, case_count, summary_payload, baseline_run_id, created_at
                FROM eval_runs
                WHERE eval_type = ?
                ORDER BY created_at DESC
                LIMIT 1
                """,
                (eval_type,),
            ).fetchone()
        if row is None:
            return None
        return self._decode_eval_run_row(dict(row))

    def list_eval_case_results(self, run_id: str) -> list[dict[str, object]]:
        with self._connect() as conn:
            rows = conn.execute(
                """
                SELECT id, run_id, case_index, session_id, question, description,
                       ok, agent_ok, interrupted, expected_tools, actual_tools,
                       missing_tools, forbidden_called, tool_order_ok, answer_present,
                       failure_reasons, result_payload, replay_payload, created_at
                FROM eval_case_results
                WHERE run_id = ?
                ORDER BY case_index ASC, id ASC
                """,
                (run_id,),
            ).fetchall()
        return [self._decode_eval_case_row(dict(row)) for row in rows]

    def upsert_session_memory(self, record: SessionMemoryRecord) -> None:
        now = datetime.now(timezone.utc).isoformat()
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO session_memories(
                    session_id, summary, key_facts, last_question, last_answer, created_at, updated_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(session_id) DO UPDATE SET
                    summary=excluded.summary,
                    key_facts=excluded.key_facts,
                    last_question=excluded.last_question,
                    last_answer=excluded.last_answer,
                    updated_at=excluded.updated_at
                """,
                (
                    record.session_id,
                    record.summary,
                    json.dumps(record.key_facts, ensure_ascii=False, sort_keys=True),
                    record.last_question,
                    record.last_answer,
                    now,
                    now,
                ),
            )

    def get_session_memory(self, session_id: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT session_id, summary, key_facts, last_question, last_answer, created_at, updated_at
                FROM session_memories
                WHERE session_id = ?
                LIMIT 1
                """,
                (session_id,),
            ).fetchone()
        if row is None:
            return None
        return self._decode_session_memory_row(dict(row))

    def list_session_memories(self, limit: int = 20) -> list[dict[str, object]]:
        with self._connect() as conn:
            rows = conn.execute(
                """
                SELECT session_id, summary, key_facts, last_question, last_answer, created_at, updated_at
                FROM session_memories
                ORDER BY updated_at DESC
                LIMIT ?
                """,
                (max(1, min(limit, 100)),),
            ).fetchall()
        return [self._decode_session_memory_row(dict(row)) for row in rows]

    def upsert_long_term_memory(self, record: LongTermMemoryRecord) -> None:
        now = datetime.now(timezone.utc).isoformat()
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO long_term_memories(
                    scope, memory_key, content, confidence, source_session_id,
                    memory_type, provenance, status, is_pinned, expires_at,
                    created_at, updated_at, last_used_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(scope, memory_key) DO UPDATE SET
                    content=excluded.content,
                    confidence=excluded.confidence,
                    source_session_id=excluded.source_session_id,
                    memory_type=excluded.memory_type,
                    provenance=excluded.provenance,
                    status=excluded.status,
                    is_pinned=excluded.is_pinned,
                    expires_at=excluded.expires_at,
                    updated_at=excluded.updated_at,
                    last_used_at=excluded.last_used_at
                """,
                (
                    record.scope,
                    record.memory_key,
                    record.content,
                    record.confidence,
                    record.source_session_id,
                    record.memory_type,
                    record.provenance,
                    record.status,
                    int(record.is_pinned),
                    record.expires_at,
                    now,
                    now,
                    now,
                ),
            )

    def get_long_term_memory(self, memory_id: int) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT id, scope, memory_key, content, confidence, source_session_id,
                       memory_type, provenance, status, is_pinned, expires_at,
                       created_at, updated_at, last_used_at
                FROM long_term_memories
                WHERE id = ?
                LIMIT 1
                """,
                (memory_id,),
            ).fetchone()
        return self._decode_long_term_memory_row(dict(row)) if row is not None else None

    def list_long_term_memories(self, scope: str = "global", limit: int = 20) -> list[dict[str, object]]:
        with self._connect() as conn:
            rows = conn.execute(
                """
                SELECT id, scope, memory_key, content, confidence, source_session_id,
                       memory_type, provenance, status, is_pinned, expires_at,
                       created_at, updated_at, last_used_at
                FROM long_term_memories
                WHERE scope = ?
                ORDER BY is_pinned DESC, updated_at DESC, id DESC
                LIMIT ?
                """,
                (scope, max(1, min(limit, 100))),
            ).fetchall()
        return [self._decode_long_term_memory_row(dict(row)) for row in rows]

    def update_long_term_memory(
        self,
        memory_id: int,
        *,
        content: str | None = None,
        confidence: float | None = None,
        memory_type: str | None = None,
        provenance: str | None = None,
        status: str | None = None,
        is_pinned: bool | None = None,
        expires_at: str | None = None,
    ) -> dict[str, object] | None:
        current = self.get_long_term_memory(memory_id)
        if current is None:
            return None
        updated = {
            "content": current.get("content", ""),
            "confidence": current.get("confidence", 0.5),
            "memory_type": current.get("memory_type", "preference"),
            "provenance": current.get("provenance", "manual_curated"),
            "status": current.get("status", "active"),
            "is_pinned": current.get("is_pinned", False),
            "expires_at": current.get("expires_at", ""),
        }
        if content is not None:
            updated["content"] = content
        if confidence is not None:
            updated["confidence"] = confidence
        if memory_type is not None:
            updated["memory_type"] = memory_type
        if provenance is not None:
            updated["provenance"] = provenance
        if status is not None:
            updated["status"] = status
        if is_pinned is not None:
            updated["is_pinned"] = is_pinned
        if expires_at is not None:
            updated["expires_at"] = expires_at
        with self._connect() as conn:
            conn.execute(
                """
                UPDATE long_term_memories
                SET content = ?, confidence = ?, memory_type = ?, provenance = ?, status = ?,
                    is_pinned = ?, expires_at = ?, updated_at = ?
                WHERE id = ?
                """,
                (
                    updated["content"],
                    updated["confidence"],
                    updated["memory_type"],
                    updated["provenance"],
                    updated["status"],
                    int(bool(updated["is_pinned"])),
                    updated["expires_at"],
                    datetime.now(timezone.utc).isoformat(),
                    memory_id,
                ),
            )
        return self.get_long_term_memory(memory_id)

    def delete_long_term_memory(self, memory_id: int) -> bool:
        with self._connect() as conn:
            cursor = conn.execute("DELETE FROM long_term_memories WHERE id = ?", (memory_id,))
        return cursor.rowcount > 0

    def touch_long_term_memories(self, memory_ids: list[int]) -> None:
        if not memory_ids:
            return
        placeholders = ",".join("?" for _ in memory_ids)
        with self._connect() as conn:
            conn.execute(
                f"""
                UPDATE long_term_memories
                SET last_used_at = ?, updated_at = updated_at
                WHERE id IN ({placeholders})
                """,
                (datetime.now(timezone.utc).isoformat(), *memory_ids),
            )

    def trim_session_memories(self, keep_limit: int) -> int:
        normalized_limit = max(1, keep_limit)
        with self._connect() as conn:
            rows = conn.execute(
                """
                SELECT session_id
                FROM session_memories
                ORDER BY updated_at DESC
                LIMIT -1 OFFSET ?
                """,
                (normalized_limit,),
            ).fetchall()
            stale_ids = [str(row["session_id"]) for row in rows]
            if not stale_ids:
                return 0
            placeholders = ",".join("?" for _ in stale_ids)
            conn.execute(
                f"DELETE FROM session_memories WHERE session_id IN ({placeholders})",
                tuple(stale_ids),
            )
        return len(stale_ids)

    def trim_long_term_memories(self, scope: str, keep_limit: int) -> int:
        normalized_limit = max(1, keep_limit)
        with self._connect() as conn:
            rows = conn.execute(
                """
                SELECT id, is_pinned
                FROM long_term_memories
                WHERE scope = ?
                ORDER BY is_pinned DESC, last_used_at DESC, updated_at DESC, id DESC
                """,
                (scope,),
            ).fetchall()
            stale_ids = [
                int(row["id"])
                for index, row in enumerate(rows)
                if index >= normalized_limit and not bool(row["is_pinned"])
            ]
            if not stale_ids:
                return 0
            placeholders = ",".join("?" for _ in stale_ids)
            conn.execute(
                f"DELETE FROM long_term_memories WHERE id IN ({placeholders})",
                tuple(stale_ids),
            )
        return len(stale_ids)

    def delete_session_memory(self, session_id: str) -> bool:
        with self._connect() as conn:
            cursor = conn.execute("DELETE FROM session_memories WHERE session_id = ?", (session_id,))
        return cursor.rowcount > 0

    def create_memory_candidates(self, records: list[MemoryCandidateRecord]) -> None:
        if not records:
            return
        now = datetime.now(timezone.utc).isoformat()
        with self._connect() as conn:
            conn.executemany(
                """
                INSERT OR REPLACE INTO memory_candidates(
                    candidate_id, session_id, scope, memory_key, content, rationale,
                    confidence, memory_type, provenance, risk_score, risk_flags,
                    status, reviewed_by, reviewed_at, first_reviewed_by, first_reviewed_at,
                    final_reviewed_by, final_reviewed_at, decision_note, created_at, updated_at
                )
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, '', '', '', '', '', '', '', ?, ?)
                """,
                [
                    (
                        record.candidate_id,
                        record.session_id,
                        record.scope,
                        record.memory_key,
                        record.content,
                        record.rationale,
                        record.confidence,
                        record.memory_type,
                        record.provenance,
                        record.risk_score,
                        json.dumps(record.risk_flags or [], ensure_ascii=False, sort_keys=True),
                        record.status,
                        now,
                        now,
                    )
                    for record in records
                ],
            )

    def get_pending_memory_candidate_by_key(self, scope: str, memory_key: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT candidate_id, session_id, scope, memory_key, content, rationale,
                       confidence, memory_type, provenance, risk_score, risk_flags,
                       status, reviewed_by, reviewed_at, first_reviewed_by, first_reviewed_at,
                       final_reviewed_by, final_reviewed_at, decision_note, created_at, updated_at
                FROM memory_candidates
                WHERE scope = ? AND memory_key = ? AND status = 'pending'
                ORDER BY updated_at DESC
                LIMIT 1
                """,
                (scope, memory_key),
            ).fetchone()
        return dict(row) if row is not None else None

    def merge_memory_candidate(
        self,
        candidate_id: str,
        session_id: str,
        content: str,
        rationale: str,
        confidence: float,
        provenance: str,
        risk_score: float,
        risk_flags: list[str],
    ) -> dict[str, object] | None:
        current = self.get_memory_candidate(candidate_id)
        if current is None:
            return None
        merged_rationale = str(current.get("rationale", "")).strip()
        incoming_rationale = rationale.strip()
        if incoming_rationale and incoming_rationale not in merged_rationale:
            merged_rationale = f"{merged_rationale} | {incoming_rationale}".strip(" |")
        merged_content = str(current.get("content", "")).strip()
        if content.strip() and content.strip() != merged_content:
            merged_content = content.strip()
        merged_provenance = str(current.get("provenance", "")).strip() or provenance.strip()
        if provenance.strip() and provenance.strip() not in merged_provenance.split("|"):
            merged_provenance = f"{merged_provenance}|{provenance.strip()}".strip("|")
        current_flags = [str(item) for item in current.get("risk_flags", []) if str(item).strip()]
        merged_flags = []
        for item in current_flags + [str(flag) for flag in risk_flags if str(flag).strip()]:
            if item not in merged_flags:
                merged_flags.append(item)
        with self._connect() as conn:
            conn.execute(
                """
                UPDATE memory_candidates
                SET session_id = ?, content = ?, rationale = ?, confidence = ?, provenance = ?,
                    risk_score = ?, risk_flags = ?, updated_at = ?
                WHERE candidate_id = ?
                """,
                (
                    session_id,
                    merged_content,
                    merged_rationale,
                    max(float(current.get("confidence", 0.0)), float(confidence)),
                    merged_provenance,
                    max(float(current.get("risk_score", 0.0)), float(risk_score)),
                    json.dumps(merged_flags, ensure_ascii=False, sort_keys=True),
                    datetime.now(timezone.utc).isoformat(),
                    candidate_id,
                ),
            )
        return self.get_memory_candidate(candidate_id)

    def list_memory_candidates(self, limit: int = 20, status: str = "") -> list[dict[str, object]]:
        clauses = []
        params: list[object] = []
        if status.strip():
            if status.strip() == "review_queue":
                clauses.append("status IN ('pending', 'stage1_approved')")
            else:
                clauses.append("status = ?")
                params.append(status.strip())
        where_sql = f"WHERE {' AND '.join(clauses)}" if clauses else ""
        params.append(max(1, min(limit, 100)))
        with self._connect() as conn:
            rows = conn.execute(
                f"""
                SELECT candidate_id, session_id, scope, memory_key, content, rationale,
                       confidence, memory_type, provenance, risk_score, risk_flags,
                       status, reviewed_by, reviewed_at, first_reviewed_by, first_reviewed_at,
                       final_reviewed_by, final_reviewed_at, decision_note, created_at, updated_at
                FROM memory_candidates
                {where_sql}
                ORDER BY created_at DESC
                LIMIT ?
                """,
                tuple(params),
            ).fetchall()
        return [dict(row) for row in rows]

    def get_memory_candidate(self, candidate_id: str) -> dict[str, object] | None:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT candidate_id, session_id, scope, memory_key, content, rationale,
                       confidence, memory_type, provenance, risk_score, risk_flags,
                       status, reviewed_by, reviewed_at, first_reviewed_by, first_reviewed_at,
                       final_reviewed_by, final_reviewed_at, decision_note, created_at, updated_at
                FROM memory_candidates
                WHERE candidate_id = ?
                LIMIT 1
                """,
                (candidate_id,),
            ).fetchone()
        return dict(row) if row is not None else None

    def review_memory_candidate(
        self,
        candidate_id: str,
        status: str,
        reviewed_by: str,
        decision_note: str = "",
        stage: str = "final",
    ) -> dict[str, object] | None:
        now = datetime.now(timezone.utc).isoformat()
        with self._connect() as conn:
            if stage == "stage1":
                conn.execute(
                    """
                    UPDATE memory_candidates
                    SET status = ?, reviewed_by = ?, reviewed_at = ?, first_reviewed_by = ?,
                        first_reviewed_at = ?, decision_note = ?, updated_at = ?
                    WHERE candidate_id = ?
                    """,
                    (
                        status,
                        reviewed_by,
                        now,
                        reviewed_by,
                        now,
                        decision_note,
                        now,
                        candidate_id,
                    ),
                )
            else:
                conn.execute(
                    """
                    UPDATE memory_candidates
                    SET status = ?, reviewed_by = ?, reviewed_at = ?, final_reviewed_by = ?,
                        final_reviewed_at = ?, decision_note = ?, updated_at = ?
                    WHERE candidate_id = ?
                    """,
                    (
                        status,
                        reviewed_by,
                        now,
                        reviewed_by,
                        now,
                        decision_note,
                        now,
                        candidate_id,
                    ),
                )
        return self.get_memory_candidate(candidate_id)

    def update_ota_batch_run_item(
        self,
        item_id: int,
        status: str,
        task_uuid: str = "",
        last_error: str = "",
        retry_count: int | None = None,
        last_checked_at: str = "",
    ) -> None:
        with self._connect() as conn:
            if retry_count is None:
                row = conn.execute(
                    "SELECT retry_count FROM ota_batch_run_items WHERE id = ? LIMIT 1",
                    (item_id,),
                ).fetchone()
                retry_count = int(row["retry_count"]) if row is not None else 0
            conn.execute(
                """
                UPDATE ota_batch_run_items
                SET status = ?, task_uuid = ?, last_error = ?, retry_count = ?,
                    last_checked_at = ?, updated_at = ?
                WHERE id = ?
                """,
                (
                    status,
                    task_uuid,
                    last_error,
                    retry_count,
                    last_checked_at,
                    datetime.now(timezone.utc).isoformat(),
                    item_id,
                ),
            )

    def _decode_approval_row(self, item: dict[str, object]) -> dict[str, object]:
        for key in ("request_payload", "result_payload"):
            try:
                item[key] = json.loads(str(item[key]))
            except Exception:
                item[key] = {}
        return item

    def _ensure_column(self, conn: sqlite3.Connection, table_name: str, column_name: str, definition: str) -> None:
        columns = {
            row["name"]
            for row in conn.execute(f"PRAGMA table_info({table_name})").fetchall()
        }
        if column_name not in columns:
            conn.execute(f"ALTER TABLE {table_name} ADD COLUMN {column_name} {definition}")

    def _decode_action_audit_row(self, item: dict[str, object]) -> dict[str, object]:
        for key in ("request_payload", "result_payload"):
            try:
                item[key] = json.loads(str(item[key]))
            except Exception:
                item[key] = {}
        return item

    def _decode_memory_audit_row(self, item: dict[str, object]) -> dict[str, object]:
        for key in ("before_state", "after_state"):
            try:
                item[key] = json.loads(str(item[key]))
            except Exception:
                item[key] = {}
        return item

    def _decode_realtime_event_row(self, item: dict[str, object]) -> dict[str, object]:
        try:
            item["payload"] = json.loads(str(item.get("payload", "{}")))
        except Exception:
            item["payload"] = {}
        return item

    def _decode_alert_row(self, item: dict[str, object]) -> dict[str, object]:
        try:
            item["payload"] = json.loads(str(item.get("payload", "{}")))
        except Exception:
            item["payload"] = {}
        return item

    def _decode_decision_row(self, item: dict[str, object]) -> dict[str, object]:
        try:
            item["payload"] = json.loads(str(item.get("payload", "{}")))
        except Exception:
            item["payload"] = {}
        item["requires_approval"] = bool(item.get("requires_approval", 0))
        return item

    def _decode_execution_row(self, item: dict[str, object]) -> dict[str, object]:
        for key in ("input_payload", "result_payload"):
            try:
                item[key] = json.loads(str(item.get(key, "{}")))
            except Exception:
                item[key] = {}
        return item

    def _decode_rollback_row(self, item: dict[str, object]) -> dict[str, object]:
        try:
            item["result_payload"] = json.loads(str(item.get("result_payload", "{}")))
        except Exception:
            item["result_payload"] = {}
        return item

    def _decode_decision_metric_snapshot_row(self, item: dict[str, object]) -> dict[str, object]:
        try:
            item["dimensions"] = json.loads(str(item.get("dimensions", "{}")))
        except Exception:
            item["dimensions"] = {}
        return item

    def _decode_tool_call_row(self, item: dict[str, object]) -> dict[str, object]:
        for key in ("tool_input", "tool_output"):
            try:
                item[key] = json.loads(str(item[key]))
            except Exception:
                item[key] = {}
        return item

    def _decode_batch_run_row(self, item: dict[str, object]) -> dict[str, object]:
        try:
            item["summary_payload"] = json.loads(str(item["summary_payload"]))
        except Exception:
            item["summary_payload"] = {}
        return item

    def _decode_eval_run_row(self, item: dict[str, object]) -> dict[str, object]:
        try:
            item["summary_payload"] = json.loads(str(item["summary_payload"]))
        except Exception:
            item["summary_payload"] = {}
        return item

    def _decode_eval_case_row(self, item: dict[str, object]) -> dict[str, object]:
        for key in (
            "expected_tools",
            "actual_tools",
            "missing_tools",
            "forbidden_called",
            "failure_reasons",
            "result_payload",
            "replay_payload",
        ):
            try:
                item[key] = json.loads(str(item[key]))
            except Exception:
                item[key] = [] if key != "result_payload" and key != "replay_payload" else {}
        for key in ("ok", "agent_ok", "interrupted", "tool_order_ok", "answer_present"):
            item[key] = bool(item.get(key))
        return item

    def _decode_session_memory_row(self, item: dict[str, object]) -> dict[str, object]:
        try:
            item["key_facts"] = json.loads(str(item["key_facts"]))
        except Exception:
            item["key_facts"] = {}
        return item

    def _decode_long_term_memory_row(self, item: dict[str, object]) -> dict[str, object]:
        item["is_pinned"] = bool(item.get("is_pinned"))
        return item

    def _decode_memory_candidate_row(self, item: dict[str, object]) -> dict[str, object]:
        try:
            item["risk_flags"] = json.loads(str(item.get("risk_flags", "[]")))
        except Exception:
            item["risk_flags"] = []
        return item
