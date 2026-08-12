from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
import re
from typing import Any
from uuid import uuid4
import csv
import io

from .agent import AgentExecutor, AgentPlanner, DiagnosisTeam
from .alerts import AlertPipeline
from .config import Settings, get_settings
from .decision_engine import DecisionEngine
from .diagnostics import DiagnosticsAnalyzer
from .evals import DEFAULT_AGENT_EVAL_CASES, compare_agent_eval_runs, run_live_agent_eval
from .llm_client import LLMClient
from .rag.indexer import build_and_save_index, ensure_index, index_status
from .rag.retriever import retrieve
from .realtime import EventEnvelope
from .reports import ReportStore
from .router import classify_question
from .sql.assistant_db import (
    ActionAuditLogRecord,
    ApprovalRequestRecord,
    AssistantDB,
    AlertRecord,
    DecisionRecord,
    DecisionMetricSnapshotRecord,
    ExecutionRecord,
    MemoryAuditLogRecord,
    MemoryCandidateRecord,
    LongTermMemoryRecord,
    OtaBatchRunItemRecord,
    OtaBatchRunRecord,
    QueryLogRecord,
    RealtimeEventRecord,
    RollbackRecord,
    NotificationRecord,
    ReportRecord,
    SessionMemoryRecord,
    TicketRecord,
    ToolCallLogRecord,
    EvalCaseResultRecord,
    EvalRunRecord,
)
from .sql.gateway_db import GatewayDB
from .sql.query_templates import (
    QueryResult,
    query_latest_sensor_event_by_name,
    query_ota_state_count,
    query_latest_sensor_event,
    query_ota_failure_reason_count,
    query_ota_failure_summary,
    query_recent_ota_events,
    query_ota_failed_tasks,
    query_ota_status,
    query_recent_sensor_events,
    query_recent_system_events,
    query_sensor_event_activity_summary,
    query_sensor_event_count,
    query_system_event_type_count,
)
from .sql.schema_inspector import inspect_sqlite
from .tools import OtaTools, RuntimeGateway
from .workflows.ota_workflow import OtaWorkflow
from .workflows.report_workflow import ReportWorkflow


DEMO_QUESTIONS = [
    "你会干什么？",
    "我应该怎么问你？",
    "DEVICE_001 当前状态正常吗？",
    "查询 sensor-01 最近 5 条传感器历史数据",
    "查询最近 10 条系统事件",
    "AA55 协议帧格式是什么？",
    "OTA 升级流程是什么？",
    "最近有哪些系统事件？",
    "最近有哪些传感器上报？",
    "查询 device_id=1 最近一次传感器数据",
    "查询 sensor-01 最近一次传感器数据",
    "最近有哪些 OTA 任务？",
    "统计 OTA 任务状态数量",
    "最近 OTA 失败任务变多，结合文档分析可能原因",
    "最近 OTA 失败任务有哪些？",
    "最近传感器上报减少，可能是什么原因？",
]


class AssistantService:
    def __init__(self, settings: Settings | None = None) -> None:
        self.settings = settings or get_settings()
        self.assistant_db = AssistantDB(self.settings.assistant_db)
        self.history_db = GatewayDB(self.settings.gateway_history_db)
        self.ota_db = GatewayDB(self.settings.gateway_ota_db)
        self.llm_client = LLMClient(self.settings)
        self.runtime_gateway = RuntimeGateway(self.settings)
        self.ota_tools = OtaTools(self.settings)
        self.ota_workflow = OtaWorkflow()
        self.report_workflow = ReportWorkflow()
        self.report_store = ReportStore(self.settings.reports_dir)
        self.diagnostics = DiagnosticsAnalyzer()
        self.diagnosis_team = DiagnosisTeam(self)
        self.agent_planner = AgentPlanner()
        self.agent_executor = AgentExecutor()
        self.alert_pipeline = AlertPipeline()
        self.decision_engine = DecisionEngine(self.settings.decision_policy_path)
        self._react_agent = None
        self._index_runtime_status = self._ensure_index_ready()
        self._role_rank = {"viewer": 1, "operator": 2, "approver": 3, "admin": 4}

    def _ensure_index_ready(self) -> dict[str, Any]:
        _, status = ensure_index(self.settings, self.assistant_db)
        return status

    def health(self) -> dict[str, Any]:
        current_index_status = index_status(self.settings)
        return {
            "status": "ok",
            "assistant_db": str(self.settings.assistant_db),
            "gateway_history_db_exists": self.history_db.exists(),
            "gateway_ota_db_exists": self.ota_db.exists(),
            "index_exists": self.settings.index_path.exists(),
            "index_status": current_index_status,
        }

    def schema_overview(self) -> dict[str, Any]:
        return {
            "history": inspect_sqlite(self.settings.gateway_history_db),
            "ota": inspect_sqlite(self.settings.gateway_ota_db),
        }

    def rebuild_index(self) -> dict[str, Any]:
        payload = build_and_save_index(self.settings, self.assistant_db)
        self._index_runtime_status = {
            "exists": True,
            "is_stale": False,
            "reason": "manual_rebuild",
            "backend": payload.get("backend", "unknown"),
            "schema_version": payload.get("schema_version", 1),
            "source_count": payload.get("source_count", 0),
        }
        return {
            "status": "ok",
            "index_path": str(self.settings.index_path),
            "total_chunks": payload["total_chunks"],
            "created_at": payload["created_at"],
            "backend": payload.get("backend", "unknown"),
            "schema_version": payload.get("schema_version", 1),
            "source_count": payload.get("source_count", 0),
        }

    def chat(self, question: str, session_id_override: str = "") -> dict[str, Any]:
        if self.settings.llm_api_key:
            try:
                result = self.chat_agent(question, session_id_override)
                if result.get("ok"):
                    return result
            except Exception:
                pass
        return self._chat_v1(question, session_id_override)

    def _chat_v1(self, question: str, session_id_override: str = "") -> dict[str, Any]:
        question = question.strip()
        decision = classify_question(question)
        session_id = session_id_override or self._new_session_id()
        runtime_result: dict[str, Any] | None = None
        sql_result: dict[str, Any] | None = None
        analysis_facts: list[str] = []
        retrieved_chunks: list[dict[str, Any]] = []
        tool_trace: list[dict[str, Any]] = []
        response_source = ""

        if decision.route_type == "Guide":
            analysis_facts = self._guide_facts()
            answer_payload = self.build_guide_answer(question, analysis_facts)
            answer = answer_payload["answer"]
            llm_mode = answer_payload["llm_mode"]
            response_source = "guide"
        elif decision.route_type == "Runtime":
            runtime_result, tool_trace = self.route_runtime_with_trace(question, session_id)
            analysis_facts = self._summarize_runtime_result(runtime_result)
            answer_payload = self.build_runtime_answer(question, runtime_result, analysis_facts)
            answer = answer_payload["answer"]
            llm_mode = answer_payload["llm_mode"]
            response_source = self._runtime_source_label(runtime_result)
        elif decision.route_type == "SQL":
            result = self.route_sql(question)
            sql_result = result.to_dict()
            analysis_facts = self._summarize_query_result(result)
            answer_payload = self.build_sql_answer(question, result, analysis_facts)
            answer = answer_payload["answer"]
            llm_mode = answer_payload["llm_mode"]
            response_source = "sql"
        elif decision.route_type == "Hybrid":
            sql_result = self.route_hybrid_sql(question)
            analysis_facts = self._summarize_hybrid_result(sql_result)
            retrieved_chunks = retrieve(question, self.settings, self.assistant_db, top_k=4)
            prompt = self._build_hybrid_prompt(question, sql_result, analysis_facts, retrieved_chunks)
            llm = self.llm_client.answer(prompt, "Hybrid", {"question": question})
            answer = llm.answer
            llm_mode = llm.mode
            response_source = "hybrid"
        else:
            retrieved_chunks = retrieve(question, self.settings, self.assistant_db, top_k=4)
            prompt = self._build_rag_prompt(question, retrieved_chunks)
            llm = self.llm_client.answer(prompt, "RAG", {"question": question})
            answer = llm.answer
            llm_mode = llm.mode
            response_source = "rag"

        self.assistant_db.log_query(
            QueryLogRecord(
                question=question,
                route_type=decision.route_type,
                sql_used=(sql_result or {}).get("sql", "") if sql_result else "",
                answer_summary=answer[:200],
            )
        )
        answer = self._format_answer_with_evidence(
            answer,
            confirmed=self._collect_confirmed_facts(analysis_facts, runtime_result, sql_result, tool_trace),
            inferred=self._collect_inferred_facts(analysis_facts, runtime_result, answer),
            user_stated=self._collect_user_stated_facts(question),
        )
        return {
            "route_type": decision.route_type,
            "route_reason": decision.reason,
            "answer": answer,
            "llm_mode": llm_mode,
            "response_source": response_source,
            "index_status": self._index_runtime_status,
            "runtime_result": runtime_result,
            "sql_result": sql_result,
            "analysis_facts": analysis_facts,
            "retrieved_chunks": retrieved_chunks,
            "tool_trace": tool_trace,
        }

    @property
    def react_agent(self):
        if self._react_agent is None:
            if not self.settings.llm_api_key:
                raise RuntimeError("LLM_API_KEY not configured. Agent mode requires a valid API key.")
            from .agent.react_agent import ReActAgent

            self._react_agent = ReActAgent(
                settings=self.settings,
                assistant_db=self.assistant_db,
                service=self,
            )
        return self._react_agent

    def chat_agent(self, question: str, session_id: str = "") -> dict[str, Any]:
        normalized_session_id = session_id or f"agent-{uuid4().hex[:12]}"
        result = self.react_agent.invoke(question.strip(), normalized_session_id)
        if result.get("ok"):
            result["answer"] = self._format_answer_with_evidence(
                str(result.get("answer", "")),
                confirmed=self._collect_confirmed_facts([], None, None, result.get("tool_trace", [])),
                inferred=self._collect_inferred_facts([], None, str(result.get("answer", ""))),
                user_stated=self._collect_user_stated_facts(question),
            )
        return result

    def chat_agent_stream(self, question: str, session_id: str = ""):
        normalized_session_id = session_id or f"agent-{uuid4().hex[:12]}"
        return self.react_agent.astream(question.strip(), normalized_session_id)

    def chat_agent_resume(
        self,
        session_id: str,
        decision: str = "approve",
        edit_args: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        result = self.react_agent.resume(session_id, decision=decision, edit_args=edit_args)
        if result.get("ok"):
            result["answer"] = self._format_answer_with_evidence(
                str(result.get("answer", "")),
                confirmed=[f"Resume decision applied: {decision}"],
                inferred=self._collect_inferred_facts([], None, str(result.get("answer", ""))),
                user_stated=[],
            )
        return result

    def run_agent_live_eval(self, cases: list[dict[str, Any]] | None = None) -> dict[str, Any]:
        if not self.settings.llm_api_key:
            return {
                "ok": False,
                "message": "LLM_API_KEY not configured. Live eval requires a real agent model.",
                "summary": {"total": 0, "passed": 0, "failed": 0, "pass_rate": 0.0},
                "cases": [],
            }
        payload = run_live_agent_eval(
            cases or DEFAULT_AGENT_EVAL_CASES,
            runner=lambda question, session_id: self.chat_agent(question, session_id),
            session_prefix="agent-live-eval",
        )
        return {"ok": True, **payload}

    def run_agent_eval_suite(self, cases: list[dict[str, Any]] | None = None) -> dict[str, Any]:
        eval_payload = self.run_agent_live_eval(cases)
        if not eval_payload.get("ok"):
            return eval_payload

        eval_type = "agent_live"
        previous_run = self.assistant_db.get_latest_eval_run(eval_type)
        previous_cases = (
            self.assistant_db.list_eval_case_results(str(previous_run.get("run_id", "")))
            if previous_run is not None
            else []
        )
        comparison = compare_agent_eval_runs(
            eval_payload.get("cases", []),
            previous_cases,
            baseline_run_id=str(previous_run.get("run_id", "")) if previous_run else "",
        )
        run_id = f"eval-{uuid4().hex[:12]}"
        summary_payload = {
            **eval_payload.get("summary", {}),
            "comparison": comparison,
        }
        self.assistant_db.create_eval_run(
            EvalRunRecord(
                run_id=run_id,
                eval_type=eval_type,
                status="completed",
                case_count=len(eval_payload.get("cases", [])),
                summary_payload=summary_payload,
                baseline_run_id=str(previous_run.get("run_id", "")) if previous_run else "",
            )
        )

        case_records: list[EvalCaseResultRecord] = []
        for index, item in enumerate(eval_payload.get("cases", []), start=1):
            session_id = str(item.get("session_id", "")).strip()
            replay_payload = self.export_agent_session_replay(session_id) if session_id else {"ok": False, "session_id": ""}
            case_records.append(
                EvalCaseResultRecord(
                    run_id=run_id,
                    case_index=index,
                    session_id=session_id,
                    question=str(item.get("question", "")),
                    description=str(item.get("description", "")),
                    ok=bool(item.get("ok", False)),
                    agent_ok=bool(item.get("agent_ok", False)),
                    interrupted=bool(item.get("interrupted", False)),
                    expected_tools=list(item.get("expected_tools", [])),
                    actual_tools=list(item.get("actual_tools", [])),
                    missing_tools=list(item.get("missing_tools", [])),
                    forbidden_called=list(item.get("forbidden_called", [])),
                    tool_order_ok=bool(item.get("tool_order_ok", False)),
                    answer_present=bool(item.get("answer_present", False)),
                    failure_reasons=list(item.get("failure_reasons", [])),
                    result_payload=dict(item.get("result", {})),
                    replay_payload=replay_payload,
                )
            )
        self.assistant_db.create_eval_case_results(case_records)
        return {
            "ok": True,
            "run_id": run_id,
            "eval_type": eval_type,
            "summary": eval_payload.get("summary", {}),
            "comparison": comparison,
            "cases": eval_payload.get("cases", []),
            "baseline_run_id": str(previous_run.get("run_id", "")) if previous_run else "",
        }

    def list_agent_eval_runs(self, limit: int = 20) -> dict[str, Any]:
        runs = self.assistant_db.list_eval_runs(limit=limit, eval_type="agent_live")
        return {"ok": True, "runs": runs, "count": len(runs)}

    def get_agent_eval_run(self, run_id: str) -> dict[str, Any]:
        run = self.assistant_db.get_eval_run(run_id)
        if run is None:
            return {"ok": False, "message": "Eval run was not found.", "run_id": run_id}
        cases = self.assistant_db.list_eval_case_results(run_id)
        return {"ok": True, "run": run, "cases": cases, "case_count": len(cases)}

    def build_guide_answer(self, question: str, analysis_facts: list[str]) -> dict[str, Any]:
        facts_text = "\n".join(f"- {item}" for item in analysis_facts)
        prompt = (
            "你是边缘网关智能运维助手的使用引导助手。\n"
            "根据给定能力清单，用简洁中文告诉用户你能做什么、怎么问，并给出几个示例。\n"
            "不要假装自己具备清单之外的能力。\n\n"
            f"用户问题：{question}\n\n"
            f"能力清单：\n{facts_text}"
        )
        llm = self.llm_client.answer(prompt, "Guide", {"question": question})
        return {
            "answer": llm.answer,
            "llm_mode": llm.mode,
        }

    def route_runtime(self, question: str) -> dict[str, Any]:
        result, _trace = self.route_runtime_with_trace(question, session_id=self._new_session_id())
        return result

    def route_runtime_with_trace(self, question: str, session_id: str) -> tuple[dict[str, Any], list[dict[str, Any]]]:
        normalized = question.lower()
        limit = self._extract_limit(question)
        task_id = self._extract_task_id(question)
        device_ref = self._extract_device_ref(question)
        tool_trace: list[dict[str, Any]] = []

        if "数据库概况" in question or "数据库摘要" in question or "database summary" in normalized:
            result, trace_item = self._run_runtime_tool(
                session_id,
                question,
                "get_database_summary",
                {},
                lambda: self.runtime_gateway.get_database_summary(),
            )
            tool_trace.append(trace_item)
            return result, tool_trace
        if "系统事件" in question:
            result, trace_item = self._run_runtime_tool(
                session_id,
                question,
                "get_system_events",
                {"limit": limit},
                lambda: self.runtime_gateway.get_system_events(limit=limit),
            )
            tool_trace.append(trace_item)
            return result, tool_trace
        if "ota" in normalized and task_id:
            result, trace_item = self._run_runtime_tool(
                session_id,
                question,
                "get_ota_task_status",
                {"task_id": task_id},
                lambda: self.runtime_gateway.get_ota_task_status(task_id),
            )
            tool_trace.append(trace_item)
            return result, tool_trace
        if device_ref and (
            "历史" in question
            or "最近传感器" in question
            or "最近一次" in question
            or "传感器" in question
            or "history" in normalized
        ):
            result, trace_item = self._run_runtime_tool(
                session_id,
                question,
                "get_sensor_history",
                {"device_ref": device_ref, "limit": limit},
                lambda: self.runtime_gateway.get_sensor_history(device_ref, limit=limit),
            )
            tool_trace.append(trace_item)
            return result, tool_trace
        if device_ref:
            result, trace_item = self._run_runtime_tool(
                session_id,
                question,
                "get_device_status",
                {"device_ref": device_ref},
                lambda: self.runtime_gateway.get_device_status(device_ref),
            )
            tool_trace.append(trace_item)
            return result, tool_trace
        if "ota" in normalized:
            return {
                "ok": False,
                "message": "当前问题提到了 OTA，但没有识别到具体 task_id。",
            }, tool_trace
        return {
            "ok": False,
            "message": "当前问题没有匹配到可执行的运行态查询目标。",
        }, tool_trace

    def route_sql(self, question: str) -> QueryResult:
        normalized = question.lower()
        device_id = self._extract_device_id(question)
        device_name = self._extract_device_name(question)
        if "ota" in normalized and ("状态" in question or "state" in normalized or "数量" in question or "统计" in question):
            return query_ota_state_count(self.ota_db)
        if "ota" in normalized and ("事件" in question or "event" in normalized):
            return query_recent_ota_events(self.ota_db)
        if "ota" in normalized and "失败" in question:
            return query_ota_failed_tasks(self.ota_db)
        if "ota" in normalized and "任务" in question:
            return query_ota_status(self.ota_db)
        if "最近一次" in question and ("传感器" in question or "sensor" in normalized):
            if device_name:
                return query_latest_sensor_event_by_name(self.history_db, device_name)
            if device_id is not None:
                return query_latest_sensor_event(self.history_db, device_id)
            return query_latest_sensor_event(self.history_db)
        if ("数量" in question or "统计" in question) and "系统事件" in question:
            return query_system_event_type_count(self.history_db)
        if "多少条" in question or ("数量" in question and "传感器" in question):
            return query_sensor_event_count(self.history_db)
        if "系统事件" in question:
            return query_recent_system_events(self.history_db)
        return query_recent_sensor_events(self.history_db)

    def build_sql_answer(self, question: str, result: QueryResult, analysis_facts: list[str]) -> dict[str, Any]:
        facts_text = "\n".join(f"- {item}" for item in analysis_facts) or "- 当前没有可提炼的事实摘要。"
        prompt = (
            "你是边缘网关 SQL 结果解释助手。\n"
            "只能依据 SQL 结果和事实摘要回答。\n"
            "不要编造数据库里不存在的信息；如果数据不足，要直接说明。\n"
            "回答尽量简洁，优先给结论，再列关键事实。\n\n"
            f"问题：{question}\n\n"
            f"事实摘要：\n{facts_text}\n\n"
            f"SQL：{result.sql}\n结果：{result.rows}\n备注：{result.note}"
        )
        llm = self.llm_client.answer(prompt, "SQL", {"question": question})
        return {
            "answer": llm.answer,
            "llm_mode": llm.mode,
        }

    def build_runtime_answer(self, question: str, runtime_result: dict[str, Any], analysis_facts: list[str]) -> dict[str, Any]:
        facts_text = "\n".join(f"- {item}" for item in analysis_facts) or "- 当前没有可提炼的事实摘要。"
        prompt = (
            "你是边缘网关运行态查询解释助手。\n"
            "只能依据工具返回结果回答。\n"
            "如果结果中带有 status_source=inferred_from_recent_events 或 message 提示为推断，必须明确告诉用户这是推断，不是专门状态表的直接结果。\n"
            "优先给结论，再给关键字段，不要编造不存在的状态或设备信息。\n\n"
            f"问题：{question}\n\n"
            f"事实摘要：\n{facts_text}\n\n"
            f"工具结果：{runtime_result}"
        )
        llm = self.llm_client.answer(prompt, "Runtime", {"question": question})
        return {
            "answer": llm.answer,
            "llm_mode": llm.mode,
        }

    def workflow_ota_risk_check(self, device_id: int, firmware_id: str, session_id_override: str = "") -> dict[str, Any]:
        session_id = session_id_override or self._new_session_id()
        question = f"ota_risk_check device_id={device_id} firmware_id={firmware_id}"
        tool_trace: list[dict[str, Any]] = []

        device_status, trace_item = self._run_tool(
            session_id,
            question,
            "get_device_status",
            {"device_ref": str(device_id)},
            lambda: self.runtime_gateway.get_device_status(str(device_id)),
        )
        tool_trace.append(trace_item)

        firmware_manifest, trace_item = self._run_tool(
            session_id,
            question,
            "get_firmware_manifest",
            {"firmware_id": firmware_id},
            lambda: self.ota_tools.get_firmware_manifest(firmware_id),
        )
        tool_trace.append(trace_item)

        recent_ota_tasks, trace_item = self._run_tool(
            session_id,
            question,
            "list_recent_ota_tasks_for_device",
            {"device_id": device_id, "limit": 10},
            lambda: self.ota_tools.list_recent_ota_tasks_for_device(device_id, limit=10),
        )
        tool_trace.append(trace_item)

        result = self.ota_workflow.build_risk_check(
            device_id=device_id,
            firmware_id=firmware_id,
            device_status=device_status,
            firmware_manifest_result=firmware_manifest,
            recent_ota_tasks_result=recent_ota_tasks,
        )
        result["tool_trace"] = tool_trace
        return result

    def workflow_ota_plan(self, device_id: int, firmware_id: str, session_id_override: str = "") -> dict[str, Any]:
        risk_result = self.workflow_ota_risk_check(device_id, firmware_id, session_id_override=session_id_override)
        plan = self.ota_workflow.build_upgrade_plan(device_id, firmware_id, risk_result)
        plan["risk_result"] = risk_result
        plan["tool_trace"] = risk_result.get("tool_trace", [])
        return plan

    def workflow_ota_request_create(
        self,
        device_id: int,
        firmware_id: str,
        transport: str = "serial",
        target: str = "127.0.0.1:19090",
        session_id_override: str = "",
    ) -> dict[str, Any]:
        plan = self.workflow_ota_plan(device_id, firmware_id, session_id_override=session_id_override)
        if not plan.get("ok"):
            return plan

        risk_level = plan.get("risk_level", "unknown")
        blocking_items = plan.get("blocking_items", [])
        approval_id = f"approval-{uuid4().hex[:12]}"
        resolved_transport, resolved_target, transport_resolution = self._resolve_ota_transport_target(
            device_id=device_id,
            transport=transport,
            target=target,
        )
        transport_guard = self._check_ota_transport_available(
            transport=resolved_transport,
            target=resolved_target,
            device_id=device_id,
        )
        if not transport_guard.get("ok"):
            return {
                "ok": False,
                "message": transport_guard.get("message", "Resolved OTA transport is unavailable."),
                "device_id": device_id,
                "firmware_id": firmware_id,
                "transport": resolved_transport,
                "target": resolved_target,
                "transport_resolution": transport_resolution,
                "transport_guard": transport_guard,
                "plan": plan,
            }
        request_payload = {
            "device_id": device_id,
            "firmware_id": firmware_id,
            "transport": resolved_transport,
            "target": resolved_target,
            "risk_level": risk_level,
            "transport_resolution": transport_resolution,
        }
        self.assistant_db.create_approval_request(
            ApprovalRequestRecord(
                approval_id=approval_id,
                request_type="ota_create_task",
                request_payload=request_payload,
                risk_summary=f"risk_level={risk_level}, blocking_items={len(blocking_items)}",
                status="pending",
            )
        )
        return {
            "ok": True,
            "approval_id": approval_id,
            "status": "pending",
            "approval_required": True,
            "request_payload": request_payload,
            "plan": plan,
            "message": "OTA approval request was created. Confirm it before creating the real OTA task.",
        }

    def workflow_ota_batch_plan(
        self,
        device_ids: list[int],
        firmware_id: str,
        transport: str = "serial",
        target: str = "127.0.0.1:19090",
        batch_size: int = 2,
        session_id_override: str = "",
    ) -> dict[str, Any]:
        session_id = session_id_override or self._new_session_id()
        normalized_ids = [int(item) for item in device_ids if int(item) > 0]
        normalized_batch_size = max(1, min(batch_size, max(1, len(normalized_ids) or 1)))
        device_plans: list[dict[str, Any]] = []
        aggregate_tool_trace: list[dict[str, Any]] = []
        risk_levels: dict[str, int] = {"low": 0, "medium": 0, "high": 0, "unknown": 0}
        device_transport_overrides: dict[str, dict[str, Any]] = {}

        for device_id in normalized_ids:
            plan = self.workflow_ota_plan(device_id, firmware_id, session_id_override=session_id)
            aggregate_tool_trace.extend(plan.get("tool_trace", []))
            risk_level = str(plan.get("risk_level", "unknown"))
            risk_levels[risk_level] = risk_levels.get(risk_level, 0) + 1
            resolved_transport, resolved_target, transport_resolution = self._resolve_ota_transport_target(
                device_id=device_id,
                transport=transport,
                target=target,
            )
            transport_guard = self._check_ota_transport_available(
                transport=resolved_transport,
                target=resolved_target,
                device_id=device_id,
            )
            guard_ok = bool(transport_guard.get("ok"))
            blocking_items = list(plan.get("blocking_items", []))
            if not guard_ok:
                blocking_items.append(str(transport_guard.get("message", "Resolved OTA transport is unavailable.")))
            device_transport_overrides[str(device_id)] = {
                "transport": resolved_transport,
                "target": resolved_target,
                "transport_resolution": transport_resolution,
                "transport_guard": transport_guard,
            }
            device_plans.append(
                {
                    "device_id": device_id,
                    "risk_level": risk_level,
                    "ready_to_create": bool(plan.get("ready_to_create", False)) and guard_ok,
                    "blocking_items": blocking_items,
                    "approval_required": bool(plan.get("approval_required", True)),
                    "transport": resolved_transport,
                    "target": resolved_target,
                }
            )

        rollout_batches = [
            normalized_ids[index : index + normalized_batch_size]
            for index in range(0, len(normalized_ids), normalized_batch_size)
        ]
        ready_device_ids = [item["device_id"] for item in device_plans if item["ready_to_create"]]
        blocked_device_ids = [item["device_id"] for item in device_plans if not item["ready_to_create"]]

        return {
            "ok": True,
            "device_ids": normalized_ids,
            "firmware_id": firmware_id,
            "transport": transport,
            "target": target,
            "batch_size": normalized_batch_size,
            "batch_count": len(rollout_batches),
            "rollout_batches": rollout_batches,
            "ready_device_ids": ready_device_ids,
            "blocked_device_ids": blocked_device_ids,
            "risk_levels": risk_levels,
            "device_plans": device_plans,
            "device_transport_overrides": device_transport_overrides,
            "tool_trace": aggregate_tool_trace,
            "message": "OTA batch plan was generated from per-device OTA risk assessments.",
        }

    def workflow_ota_batch_request_create(
        self,
        device_ids: list[int],
        firmware_id: str,
        transport: str = "serial",
        target: str = "127.0.0.1:19090",
        batch_size: int = 2,
        session_id_override: str = "",
    ) -> dict[str, Any]:
        session_id = session_id_override or self._new_session_id()
        batch_plan = self.workflow_ota_batch_plan(
            device_ids=device_ids,
            firmware_id=firmware_id,
            transport=transport,
            target=target,
            batch_size=batch_size,
            session_id_override=session_id,
        )
        if not batch_plan.get("ok"):
            return batch_plan

        approval_id = f"approval-batch-{uuid4().hex[:12]}"
        request_payload = {
            "device_ids": batch_plan.get("device_ids", []),
            "firmware_id": firmware_id,
            "transport": transport,
            "target": target,
            "batch_size": batch_plan.get("batch_size", 1),
            "batch_count": batch_plan.get("batch_count", 0),
            "rollout_batches": batch_plan.get("rollout_batches", []),
            "ready_device_ids": batch_plan.get("ready_device_ids", []),
            "blocked_device_ids": batch_plan.get("blocked_device_ids", []),
            "device_transport_overrides": batch_plan.get("device_transport_overrides", {}),
        }
        self.assistant_db.create_approval_request(
            ApprovalRequestRecord(
                approval_id=approval_id,
                request_type="ota_batch_create_task",
                request_payload=request_payload,
                risk_summary=(
                    f"device_count={len(batch_plan.get('device_ids', []))}, "
                    f"ready={len(batch_plan.get('ready_device_ids', []))}, "
                    f"blocked={len(batch_plan.get('blocked_device_ids', []))}"
                ),
                status="pending",
            )
        )
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=session_id,
                action_type="create_ota_batch_approval_request",
                operator="agent",
                request_payload=request_payload,
                result_payload={"approval_id": approval_id, "batch_count": batch_plan.get("batch_count", 0)},
                status="ok",
            )
        )
        return {
            "ok": True,
            "approval_id": approval_id,
            "status": "pending",
            "approval_required": True,
            "request_payload": request_payload,
            "batch_plan": batch_plan,
            "tool_trace": batch_plan.get("tool_trace", []),
            "message": "OTA batch approval request was created. Confirm per-device rollout manually after review.",
        }

    def workflow_ota_request_confirm(
        self,
        approval_id: str,
        approved_by: str = "operator",
        decision_note: str = "",
        session_id_override: str = "",
    ) -> dict[str, Any]:
        approval = self.assistant_db.get_approval_request(approval_id)
        if approval is None:
            return {
                "ok": False,
                "message": "Approval request was not found.",
                "approval_id": approval_id,
            }
        if approval.get("status") != "pending":
            return {
                "ok": False,
                "message": "Approval request is not pending anymore.",
                "approval_id": approval_id,
                "status": approval.get("status"),
            }

        payload = approval.get("request_payload", {})
        device_id = int(payload.get("device_id", 0))
        firmware_id = str(payload.get("firmware_id", ""))
        transport = str(payload.get("transport", "serial"))
        target = str(payload.get("target", "127.0.0.1:19090"))
        session_id = session_id_override or self._new_session_id()
        action_payload = {
            "approval_id": approval_id,
            "device_id": device_id,
            "firmware_id": firmware_id,
            "transport": transport,
            "target": target,
            "approved_by": approved_by,
        }
        transport_guard = self._check_ota_transport_available(
            transport=transport,
            target=target,
            device_id=device_id,
        )
        if not transport_guard.get("ok"):
            failure = self._mark_approval_execution_failed(
                approval_id=approval_id,
                approved_by=approved_by,
                decision_note=decision_note,
                result_payload={
                    "status": "execute_failed",
                    "reason": "gateway_transport_unavailable",
                    "transport_guard": transport_guard,
                },
            )
            self.assistant_db.log_action_audit(
                ActionAuditLogRecord(
                    session_id=session_id,
                    action_type="approve_and_create_ota_task",
                    operator=approved_by,
                    request_payload=action_payload,
                    result_payload=failure["result_payload"],
                    status="error",
                )
            )
            return {
                "ok": False,
                "message": str(transport_guard.get("message", "Gateway transport is unavailable.")),
                "approval_id": approval_id,
                "status": failure["status"],
                "approved_by": approved_by,
                "approved_at": failure["approved_at"],
                "transport_guard": transport_guard,
            }

        firmware_manifest = self.ota_tools.get_firmware_manifest(firmware_id)
        if not firmware_manifest.get("ok"):
            failure = self._mark_approval_execution_failed(
                approval_id=approval_id,
                approved_by=approved_by,
                decision_note=decision_note,
                result_payload={
                    "status": "execute_failed",
                    "reason": "firmware_manifest_lookup_failed",
                    "firmware_manifest": firmware_manifest,
                },
            )
            self.assistant_db.log_action_audit(
                ActionAuditLogRecord(
                    session_id=session_id,
                    action_type="approve_and_create_ota_task",
                    operator=approved_by,
                    request_payload=action_payload,
                    result_payload=failure["result_payload"],
                    status="error",
                )
            )
            return {
                "ok": False,
                "message": "Cannot confirm OTA request because manifest lookup failed.",
                "approval_id": approval_id,
                "status": failure["status"],
                "approved_by": approved_by,
                "approved_at": failure["approved_at"],
                "firmware_manifest": firmware_manifest,
            }
        device_type = firmware_manifest.get("manifest", {}).get("device_type", "")
        if not device_type:
            failure = self._mark_approval_execution_failed(
                approval_id=approval_id,
                approved_by=approved_by,
                decision_note=decision_note,
                result_payload={
                    "status": "execute_failed",
                    "reason": "device_type_missing",
                    "firmware_id": firmware_id,
                },
            )
            self.assistant_db.log_action_audit(
                ActionAuditLogRecord(
                    session_id=session_id,
                    action_type="approve_and_create_ota_task",
                    operator=approved_by,
                    request_payload=action_payload,
                    result_payload=failure["result_payload"],
                    status="error",
                )
            )
            return {
                "ok": False,
                "message": "Cannot confirm OTA request because device_type is missing in manifest.",
                "approval_id": approval_id,
                "status": failure["status"],
                "approved_by": approved_by,
                "approved_at": failure["approved_at"],
                "firmware_id": firmware_id,
            }
        question = f"ota_request_confirm approval_id={approval_id}"
        create_result, trace_item = self._run_tool(
            session_id,
            question,
            "create_ota_task",
            {
                "device_id": device_id,
                "device_type": device_type,
                "firmware_id": firmware_id,
                "transport": transport,
                "target": target,
            },
            lambda: self.ota_tools.create_ota_task(
                device_id=device_id,
                device_type=device_type,
                firmware_id=firmware_id,
                transport=transport,
                target=target,
            ),
        )
        if not create_result.get("ok"):
            failure = self._mark_approval_execution_failed(
                approval_id=approval_id,
                approved_by=approved_by,
                decision_note=decision_note,
                result_payload={
                    "status": "execute_failed",
                    "reason": "gateway_ota_task_create_failed",
                    "create_result": create_result,
                },
            )
            self.assistant_db.log_action_audit(
                ActionAuditLogRecord(
                    session_id=session_id,
                    action_type="approve_and_create_ota_task",
                    operator=approved_by,
                    request_payload=action_payload,
                    result_payload=failure["result_payload"],
                    status="error",
                )
            )
            return {
                "ok": False,
                "message": "Approval confirmed, but gateway OTA task creation failed.",
                "approval_id": approval_id,
                "status": failure["status"],
                "approved_by": approved_by,
                "approved_at": failure["approved_at"],
                "create_result": create_result,
                "tool_trace": [trace_item],
            }

        approved_at = datetime.now(timezone.utc).isoformat()
        self.assistant_db.update_approval_request_status(
            approval_id=approval_id,
            status="approved",
            approved_by=approved_by,
            approved_at=approved_at,
            decision_note=decision_note,
            result_payload=create_result,
        )
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=session_id,
                action_type="approve_and_create_ota_task",
                operator=approved_by,
                request_payload=action_payload,
                result_payload=create_result,
                status="ok",
            )
        )
        return {
            "ok": True,
            "approval_id": approval_id,
            "status": "approved",
            "approved_by": approved_by,
            "approved_at": approved_at,
            "create_result": create_result,
            "tool_trace": [trace_item],
            "message": "Approval confirmed and OTA task was created through the gateway API.",
        }

    def workflow_ota_batch_request_confirm(
        self,
        approval_id: str,
        approved_by: str = "operator",
        decision_note: str = "",
        session_id_override: str = "",
    ) -> dict[str, Any]:
        approval = self.assistant_db.get_approval_request(approval_id)
        if approval is None:
            return {"ok": False, "message": "Approval request was not found.", "approval_id": approval_id}
        if approval.get("status") != "pending":
            return {
                "ok": False,
                "message": "Approval request is not pending anymore.",
                "approval_id": approval_id,
                "status": approval.get("status"),
            }

        payload = approval.get("request_payload", {})
        firmware_id = str(payload.get("firmware_id", ""))
        transport = str(payload.get("transport", "serial"))
        target = str(payload.get("target", "127.0.0.1:19090"))
        batch_count = int(payload.get("batch_count", 0) or 0)
        ready_device_ids = [int(item) for item in payload.get("ready_device_ids", [])]
        blocked_device_ids = [int(item) for item in payload.get("blocked_device_ids", [])]
        session_id = session_id_override or self._new_session_id()

        firmware_manifest = self.ota_tools.get_firmware_manifest(firmware_id)
        if not firmware_manifest.get("ok"):
            failure = self._mark_approval_execution_failed(
                approval_id=approval_id,
                approved_by=approved_by,
                decision_note=decision_note,
                result_payload={
                    "status": "execute_failed",
                    "reason": "firmware_manifest_lookup_failed",
                    "firmware_manifest": firmware_manifest,
                },
            )
            return {
                "ok": False,
                "message": "Cannot confirm OTA batch request because manifest lookup failed.",
                "approval_id": approval_id,
                "status": failure["status"],
                "approved_by": approved_by,
                "approved_at": failure["approved_at"],
                "firmware_manifest": firmware_manifest,
            }
        device_type = firmware_manifest.get("manifest", {}).get("device_type", "")
        if not device_type:
            failure = self._mark_approval_execution_failed(
                approval_id=approval_id,
                approved_by=approved_by,
                decision_note=decision_note,
                result_payload={
                    "status": "execute_failed",
                    "reason": "device_type_missing",
                    "firmware_id": firmware_id,
                },
            )
            return {
                "ok": False,
                "message": "Cannot confirm OTA batch request because device_type is missing in manifest.",
                "approval_id": approval_id,
                "status": failure["status"],
                "approved_by": approved_by,
                "approved_at": failure["approved_at"],
                "firmware_id": firmware_id,
            }

        batch_run_id = f"batch-run-{uuid4().hex[:12]}"
        rollout_batches = payload.get("rollout_batches", [])
        batch_index_by_device: dict[int, int] = {}
        if isinstance(rollout_batches, list):
            for idx, batch in enumerate(rollout_batches, start=1):
                if isinstance(batch, list):
                    for device_id in batch:
                        try:
                            batch_index_by_device[int(device_id)] = idx
                        except Exception:
                            continue

        batch_items: list[OtaBatchRunItemRecord] = []
        first_batch_index = min(batch_index_by_device.values()) if batch_index_by_device else 1
        for device_id in ready_device_ids:
            resolved_batch_index = batch_index_by_device.get(device_id, first_batch_index)
            batch_items.append(
                OtaBatchRunItemRecord(
                    batch_run_id=batch_run_id,
                    batch_index=resolved_batch_index,
                    device_id=device_id,
                    status="queued" if resolved_batch_index == first_batch_index else "waiting_batch",
                    task_uuid="",
                    last_error="",
                    retry_count=0,
                    last_checked_at="",
                )
            )
        approved_at = datetime.now(timezone.utc).isoformat()
        self.assistant_db.create_ota_batch_run(
            OtaBatchRunRecord(
                batch_run_id=batch_run_id,
                approval_id=approval_id,
                firmware_id=firmware_id,
                transport=transport,
                target=target,
                batch_size=int(payload.get("batch_size", 1) or 1),
                batch_count=batch_count,
                total_devices=len(ready_device_ids),
                status="pending_batch_start",
                created_by=approved_by,
                summary_payload=self._summarize_batch_run_items_dicts(
                    [
                        {
                            "batch_index": item.batch_index,
                            "status": item.status,
                            "device_id": item.device_id,
                            "task_uuid": item.task_uuid,
                            "last_error": item.last_error,
                        }
                        for item in batch_items
                    ],
                    blocked_device_ids=blocked_device_ids,
                ),
            )
        )
        self.assistant_db.create_ota_batch_run_items(batch_items)
        result_payload = {
            "status": "approved",
            "created_count": 0,
            "failed_count": 0,
            "ready_device_ids": ready_device_ids,
            "blocked_device_ids": blocked_device_ids,
            "per_device_results": [],
            "failed_device_ids": [],
            "retry_history": [],
            "batch_run_id": batch_run_id,
            "current_batch_index": 0,
        }
        self.assistant_db.update_approval_request_status(
            approval_id=approval_id,
            status="approved",
            approved_by=approved_by,
            approved_at=approved_at,
            decision_note=decision_note,
            result_payload=result_payload,
        )
        initial_start = self.start_next_batch(
            batch_run_id=batch_run_id,
            operator=approved_by,
            decision_note=f"{decision_note} | start_batch=1".strip(" |"),
            session_id_override=session_id,
        )
        batch_run_snapshot = self.assistant_db.get_ota_batch_run(batch_run_id) or {}
        items_snapshot = self.assistant_db.list_ota_batch_run_items(batch_run_id)
        summary_payload = batch_run_snapshot.get("summary_payload", {})
        self.assistant_db.update_approval_request_status(
            approval_id=approval_id,
            status=str(batch_run_snapshot.get("status", "approved")),
            approved_by=approved_by,
            approved_at=approved_at,
            decision_note=decision_note,
            result_payload={
                **result_payload,
                "status": batch_run_snapshot.get("status", "approved"),
                "created_count": int(summary_payload.get("task_created_count", 0))
                + int(summary_payload.get("running_count", 0))
                + int(summary_payload.get("success_count", 0)),
                "failed_count": int(summary_payload.get("failed_count", 0)),
                "failed_device_ids": summary_payload.get("failed_device_ids", []),
                "per_device_results": initial_start.get("per_device_results", []),
                "current_batch_index": initial_start.get("started_batch_index", 0),
                "pending_batch_indexes": initial_start.get("pending_batch_indexes", []),
            },
        )
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=session_id,
                action_type="approve_and_create_ota_batch_run",
                operator=approved_by,
                request_payload={
                    "approval_id": approval_id,
                    "firmware_id": firmware_id,
                    "transport": transport,
                    "target": target,
                    "ready_device_ids": ready_device_ids,
                    "blocked_device_ids": blocked_device_ids,
                    "batch_count": batch_count,
                },
                result_payload={
                    "batch_run_id": batch_run_id,
                    "status": batch_run_snapshot.get("status", "approved"),
                    "started_batch_index": initial_start.get("started_batch_index", 0),
                },
                status="ok" if initial_start.get("ok", False) else "error",
            )
        )
        return {
            "ok": bool(initial_start.get("ok", False)),
            "approval_id": approval_id,
            "status": batch_run_snapshot.get("status", "approved"),
            "approved_by": approved_by,
            "approved_at": approved_at,
            "ready_device_ids": ready_device_ids,
            "blocked_device_ids": blocked_device_ids,
            "batch_run_id": batch_run_id,
            "started_batch_index": initial_start.get("started_batch_index", 0),
            "pending_batch_indexes": initial_start.get("pending_batch_indexes", []),
            "per_device_results": initial_start.get("per_device_results", []),
            "failed_device_ids": summary_payload.get("failed_device_ids", []),
            "tool_trace": initial_start.get("tool_trace", []),
            "batch_run": batch_run_snapshot,
            "items": items_snapshot,
            "message": "OTA batch approval was converted into a tracked batch run and the first batch was started.",
        }

    def workflow_generate_report(
        self,
        report_type: str = "fault_ticket",
        device_id: int | None = None,
        task_id: str = "",
        limit: int = 10,
        diagnosis_result_override: dict[str, Any] | None = None,
        session_id_override: str = "",
    ) -> dict[str, Any]:
        session_id = session_id_override or self._new_session_id()
        question = (
            f"report_generate report_type={report_type} "
            f"device_id={device_id if device_id is not None else 'none'} task_id={task_id or 'none'}"
        )
        normalized_limit = max(1, min(limit, 20))
        tool_trace: list[dict[str, Any]] = []
        team_trace: list[dict[str, Any]] = []
        collection_mode = "serial_report_collection"

        device_status: dict[str, Any] | None = None
        sensor_history: dict[str, Any] | None = None
        recent_ota_tasks: dict[str, Any] | None = None
        system_events: dict[str, Any] | None = None
        if report_type.strip() == "fault_ticket" and device_id is not None:
            runtime_inputs = self._collect_diagnosis_runtime_inputs(
                session_id=session_id,
                question=question,
                device_id=device_id,
                normalized_limit=normalized_limit,
            )
            device_status = runtime_inputs["device_status"]
            sensor_history = runtime_inputs["sensor_history"]
            system_events = runtime_inputs["system_events"]
            recent_ota_tasks = runtime_inputs["recent_ota_tasks"]
            tool_trace.extend(runtime_inputs["tool_trace"])
            team_trace = runtime_inputs.get("team_trace", [])
            collection_mode = runtime_inputs.get("collection_mode", collection_mode)
        else:
            if device_id is not None:
                device_status, trace_item = self._run_tool(
                    session_id,
                    question,
                    "get_device_status",
                    {"device_ref": str(device_id)},
                    lambda: self.runtime_gateway.get_device_status(str(device_id)),
                )
                tool_trace.append(trace_item)
                sensor_history, trace_item = self._run_tool(
                    session_id,
                    question,
                    "get_sensor_history",
                    {"device_ref": str(device_id), "limit": normalized_limit},
                    lambda: self.runtime_gateway.get_sensor_history(str(device_id), limit=normalized_limit),
                )
                tool_trace.append(trace_item)
                recent_ota_tasks, trace_item = self._run_tool(
                    session_id,
                    question,
                    "list_recent_ota_tasks_for_device",
                    {"device_id": device_id, "limit": normalized_limit},
                    lambda: self.ota_tools.list_recent_ota_tasks_for_device(device_id, limit=normalized_limit),
                )
                tool_trace.append(trace_item)

            system_events, trace_item = self._run_tool(
                session_id,
                question,
                "get_system_events",
                {"limit": normalized_limit},
                lambda: self.runtime_gateway.get_system_events(limit=normalized_limit),
            )
            tool_trace.append(trace_item)

        ota_status: dict[str, Any] | None = None
        if task_id.strip():
            ota_status, trace_item = self._run_tool(
                session_id,
                question,
                "get_ota_task_status",
                {"task_id": task_id.strip()},
                lambda: self.runtime_gateway.get_ota_task_status(task_id.strip()),
            )
            tool_trace.append(trace_item)

        diagnosis_result: dict[str, Any] | None = diagnosis_result_override
        if diagnosis_result is None and report_type.strip() == "fault_ticket" and device_id is not None:
            diagnosis_result = self.diagnostics.analyze(
                device_id=device_id,
                device_status=device_status or {},
                sensor_history=sensor_history or {},
                system_events=system_events,
                recent_ota_tasks=recent_ota_tasks,
            )

        result = self.report_workflow.build_report(
            report_type=report_type,
            device_id=device_id,
            limit=normalized_limit,
            device_status=device_status,
            sensor_history=sensor_history,
            system_events=system_events,
            ota_status=ota_status,
            diagnosis_result=diagnosis_result,
        )
        persisted = self.report_store.persist(result)
        self.assistant_db.create_report_record(
            ReportRecord(
                report_id=persisted["report_id"],
                report_type=str(result.get("report_type", report_type)),
                title=str(result.get("title", "")),
                device_id=device_id,
                task_id=task_id.strip(),
                summary=str(result.get("summary", "")),
                markdown_path=persisted["markdown_path"],
                json_path=persisted["json_path"],
            )
        )
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=session_id,
                action_type="generate_report",
                operator="agent",
                request_payload={
                    "report_type": report_type,
                    "device_id": device_id,
                    "task_id": task_id.strip(),
                    "limit": normalized_limit,
                },
                result_payload={
                    "report_id": persisted["report_id"],
                    "report_type": result.get("report_type"),
                    "title": result.get("title"),
                },
                status="ok",
            )
        )
        result["report_id"] = persisted["report_id"]
        result["created_at"] = persisted["created_at"]
        result["markdown_path"] = persisted["markdown_path"]
        result["json_path"] = persisted["json_path"]
        result["device_id"] = device_id
        result["task_id"] = task_id.strip()
        if diagnosis_result is not None:
            result["diagnosis_result"] = diagnosis_result
        result["tool_trace"] = tool_trace
        result["team_trace"] = team_trace
        result["collection_mode"] = collection_mode
        return result

    def workflow_diagnose_fault(self, device_id: int, limit: int = 10, session_id_override: str = "") -> dict[str, Any]:
        session_id = session_id_override or self._new_session_id()
        question = f"diagnose_fault device_id={device_id} limit={limit}"
        normalized_limit = max(1, min(limit, 20))
        runtime_inputs = self._collect_diagnosis_runtime_inputs(
            session_id=session_id,
            question=question,
            device_id=device_id,
            normalized_limit=normalized_limit,
        )
        device_status = runtime_inputs["device_status"]
        sensor_history = runtime_inputs["sensor_history"]
        system_events = runtime_inputs["system_events"]
        recent_ota_tasks = runtime_inputs["recent_ota_tasks"]
        tool_trace = runtime_inputs["tool_trace"]

        result = self.diagnostics.analyze(
            device_id=device_id,
            device_status=device_status,
            sensor_history=sensor_history,
            system_events=system_events,
            recent_ota_tasks=recent_ota_tasks,
        )
        result["runtime_inputs"] = {
            "device_status": device_status,
            "sensor_history": sensor_history,
            "system_events": system_events,
            "recent_ota_tasks": recent_ota_tasks,
        }
        result["tool_trace"] = tool_trace
        result["collection_mode"] = runtime_inputs["collection_mode"]
        result["team_trace"] = runtime_inputs.get("team_trace", [])
        return result

    def _collect_diagnosis_runtime_inputs(
        self,
        session_id: str,
        question: str,
        device_id: int,
        normalized_limit: int,
    ) -> dict[str, Any]:
        return self.diagnosis_team.collect_runtime_inputs(
            session_id=session_id,
            question=question,
            device_id=device_id,
            normalized_limit=normalized_limit,
        )

    def list_approval_requests(self, limit: int = 20, status: str = "") -> dict[str, Any]:
        items = self.assistant_db.list_approval_requests(limit=limit, status=status)
        return {"ok": True, "count": len(items), "items": items}

    def get_approval(self, approval_id: str) -> dict[str, Any]:
        approval = self.assistant_db.get_approval_request(approval_id)
        if approval is None:
            return {"ok": False, "message": "Approval request was not found.", "approval_id": approval_id}
        return {"ok": True, "approval": approval}

    def reject_approval_request(
        self,
        approval_id: str,
        rejected_by: str = "operator",
        decision_note: str = "",
    ) -> dict[str, Any]:
        approval = self.assistant_db.get_approval_request(approval_id)
        if approval is None:
            return {"ok": False, "message": "Approval request was not found.", "approval_id": approval_id}
        if approval.get("status") != "pending":
            return {
                "ok": False,
                "message": "Approval request is not pending anymore.",
                "approval_id": approval_id,
                "status": approval.get("status"),
            }
        acted_at = datetime.now(timezone.utc).isoformat()
        self.assistant_db.update_approval_request_status(
            approval_id=approval_id,
            status="rejected",
            approved_by=rejected_by,
            approved_at=acted_at,
            decision_note=decision_note,
            result_payload={"status": "rejected"},
        )
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=self._new_session_id(),
                action_type="reject_approval_request",
                operator=rejected_by,
                request_payload={"approval_id": approval_id},
                result_payload={"approval_id": approval_id, "status": "rejected"},
                status="ok",
            )
        )
        return {
            "ok": True,
            "approval_id": approval_id,
            "status": "rejected",
            "approved_by": rejected_by,
            "approved_at": acted_at,
            "decision_note": decision_note,
        }

    def cancel_approval_request(
        self,
        approval_id: str,
        operator: str = "operator",
        decision_note: str = "",
    ) -> dict[str, Any]:
        approval = self.assistant_db.get_approval_request(approval_id)
        if approval is None:
            return {"ok": False, "message": "Approval request was not found.", "approval_id": approval_id}
        if approval.get("status") != "pending":
            return {
                "ok": False,
                "message": "Only pending approval requests can be cancelled.",
                "approval_id": approval_id,
                "status": approval.get("status"),
            }
        acted_at = datetime.now(timezone.utc).isoformat()
        self.assistant_db.update_approval_request_status(
            approval_id=approval_id,
            status="cancelled",
            approved_by=operator,
            approved_at=acted_at,
            decision_note=decision_note,
            result_payload={"status": "cancelled"},
        )
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=self._new_session_id(),
                action_type="cancel_approval_request",
                operator=operator,
                request_payload={"approval_id": approval_id, "decision_note": decision_note},
                result_payload={"approval_id": approval_id, "status": "cancelled"},
                status="ok",
            )
        )
        return {
            "ok": True,
            "approval_id": approval_id,
            "status": "cancelled",
            "approved_by": operator,
            "approved_at": acted_at,
            "decision_note": decision_note,
        }

    def start_next_batch(
        self,
        batch_run_id: str,
        operator: str = "operator",
        decision_note: str = "",
        session_id_override: str = "",
    ) -> dict[str, Any]:
        run = self.assistant_db.get_ota_batch_run(batch_run_id)
        if run is None:
            return {"ok": False, "message": "Batch run was not found.", "batch_run_id": batch_run_id}
        if str(run.get("status", "")) == "terminated":
            return {"ok": False, "message": "Terminated batch runs cannot start new batches.", "batch_run_id": batch_run_id}
        items = self.assistant_db.list_ota_batch_run_items(batch_run_id)
        if not items:
            return {"ok": False, "message": "Batch run has no executable devices.", "batch_run_id": batch_run_id}
        next_batch_index = self._next_batch_index(items)
        if next_batch_index is None:
            return {"ok": False, "message": "There is no pending batch to start.", "batch_run_id": batch_run_id}
        blocking = self._validate_previous_batches_before_start(items, next_batch_index)
        if blocking:
            return {
                "ok": False,
                "message": blocking,
                "batch_run_id": batch_run_id,
                "next_batch_index": next_batch_index,
            }

        firmware_manifest = self.ota_tools.get_firmware_manifest(str(run.get("firmware_id", "")))
        if not firmware_manifest.get("ok"):
            return {
                "ok": False,
                "message": "Cannot start next batch because manifest lookup failed.",
                "batch_run_id": batch_run_id,
                "firmware_manifest": firmware_manifest,
            }
        device_type = str(firmware_manifest.get("manifest", {}).get("device_type", ""))
        if not device_type:
            return {
                "ok": False,
                "message": "Cannot start next batch because device_type is missing in manifest.",
                "batch_run_id": batch_run_id,
            }

        session_id = session_id_override or self._new_session_id()
        candidate_items = [item for item in items if int(item.get("batch_index", 0)) == next_batch_index and str(item.get("status", "")) in {"queued", "waiting_batch"}]
        per_device_results: list[dict[str, Any]] = []
        tool_trace: list[dict[str, Any]] = []
        acted_at = datetime.now(timezone.utc).isoformat()
        for item in candidate_items:
            device_id = int(item.get("device_id", 0))
            resolved_transport, resolved_target = self._resolve_batch_device_transport(run, device_id)
            transport_guard = self._check_ota_transport_available(
                transport=resolved_transport,
                target=resolved_target,
                device_id=device_id,
            )
            if not transport_guard.get("ok"):
                create_result = {
                    "ok": False,
                    "message": str(transport_guard.get("message", "Gateway transport is unavailable.")),
                    "reason": "gateway_transport_unavailable",
                    "transport_guard": transport_guard,
                }
                trace_item = {
                    "tool_name": "gateway_transport_guard",
                    "tool_input": {
                        "device_id": device_id,
                        "transport": resolved_transport,
                        "target": resolved_target,
                    },
                    "tool_output": create_result,
                    "status": "error",
                }
            else:
                create_result, trace_item = self._run_tool(
                    session_id,
                    f"start_next_batch batch_run_id={batch_run_id} batch_index={next_batch_index} device_id={device_id}",
                    "create_ota_task",
                    {
                        "device_id": device_id,
                        "device_type": device_type,
                        "firmware_id": str(run.get("firmware_id", "")),
                        "transport": resolved_transport,
                        "target": resolved_target,
                    },
                    lambda device_id=device_id: self.ota_tools.create_ota_task(
                        device_id=device_id,
                        device_type=device_type,
                        firmware_id=str(run.get("firmware_id", "")),
                        transport=resolved_transport,
                        target=resolved_target,
                    ),
                )
            tool_trace.append(trace_item)
            task = create_result.get("task", {}) if isinstance(create_result.get("task"), dict) else {}
            mapped_status = "task_created" if create_result.get("ok") else "submit_failed"
            self.assistant_db.update_ota_batch_run_item(
                item_id=int(item["id"]),
                status=mapped_status,
                task_uuid=str(task.get("task_uuid", "")),
                last_error="" if create_result.get("ok") else str(create_result.get("message", "")),
                retry_count=int(item.get("retry_count", 0)),
                last_checked_at=acted_at,
            )
            per_device_results.append({"device_id": device_id, "result": create_result})

        refreshed_items = self.assistant_db.list_ota_batch_run_items(batch_run_id)
        summary = self._summarize_batch_run_items_dicts(
            refreshed_items,
            blocked_device_ids=run.get("summary_payload", {}).get("blocked_device_ids", []),
        )
        control = dict(summary.get("control", {}))
        control["paused"] = False
        control["last_action"] = "start_next_batch"
        control["last_operator"] = operator
        summary["control"] = control
        run_status = self._derive_batch_run_status(summary)
        self.assistant_db.update_ota_batch_run_status(batch_run_id, run_status, summary)
        self._sync_approval_from_batch_run(run, summary, operator, decision_note)
        pending_batch_indexes = self._pending_batch_indexes(refreshed_items)
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=session_id,
                action_type="start_next_ota_batch",
                operator=operator,
                request_payload={"batch_run_id": batch_run_id, "batch_index": next_batch_index, "decision_note": decision_note},
                result_payload={"status": run_status, "pending_batch_indexes": pending_batch_indexes},
                status="ok" if all(item["result"].get("ok") for item in per_device_results) else "error",
            )
        )
        return {
            "ok": all(item["result"].get("ok") for item in per_device_results),
            "batch_run_id": batch_run_id,
            "status": run_status,
            "started_batch_index": next_batch_index,
            "pending_batch_indexes": pending_batch_indexes,
            "per_device_results": per_device_results,
            "tool_trace": tool_trace,
            "message": "Next OTA batch has been started.",
        }

    def pause_batch_run(
        self,
        batch_run_id: str,
        operator: str = "operator",
        decision_note: str = "",
    ) -> dict[str, Any]:
        run = self.assistant_db.get_ota_batch_run(batch_run_id)
        if run is None:
            return {"ok": False, "message": "Batch run was not found.", "batch_run_id": batch_run_id}
        if str(run.get("status", "")) == "terminated":
            return {"ok": False, "message": "Terminated batch runs cannot be paused.", "batch_run_id": batch_run_id}
        items = self.assistant_db.list_ota_batch_run_items(batch_run_id)
        if any(str(item.get("status", "")) in {"task_created", "running"} for item in items):
            return {
                "ok": False,
                "message": "Current batch is still active. Refresh or wait for completion before pausing the rollout.",
                "batch_run_id": batch_run_id,
            }
        if not self._pending_batch_indexes(items):
            return {"ok": False, "message": "There is no pending batch to pause.", "batch_run_id": batch_run_id}
        summary = self._summarize_batch_run_items_dicts(
            items,
            blocked_device_ids=run.get("summary_payload", {}).get("blocked_device_ids", []),
        )
        control = dict(summary.get("control", {}))
        control.update(
            {
                "paused": True,
                "paused_at": datetime.now(timezone.utc).isoformat(),
                "pause_operator": operator,
                "pause_note": decision_note,
                "last_action": "pause_batch_run",
            }
        )
        summary["control"] = control
        self.assistant_db.update_ota_batch_run_status(batch_run_id, "paused", summary)
        self._sync_approval_from_batch_run(run, summary, operator, decision_note)
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=self._new_session_id(),
                action_type="pause_ota_batch_run",
                operator=operator,
                request_payload={"batch_run_id": batch_run_id, "decision_note": decision_note},
                result_payload={"status": "paused"},
                status="ok",
            )
        )
        return {"ok": True, "batch_run_id": batch_run_id, "status": "paused", "message": "Batch rollout was paused before the next batch."}

    def terminate_batch_run(
        self,
        batch_run_id: str,
        operator: str = "operator",
        decision_note: str = "",
    ) -> dict[str, Any]:
        run = self.assistant_db.get_ota_batch_run(batch_run_id)
        if run is None:
            return {"ok": False, "message": "Batch run was not found.", "batch_run_id": batch_run_id}
        items = self.assistant_db.list_ota_batch_run_items(batch_run_id)
        acted_at = datetime.now(timezone.utc).isoformat()
        cancelled_items = 0
        for item in items:
            if str(item.get("status", "")) in {"queued", "waiting_batch"}:
                self.assistant_db.update_ota_batch_run_item(
                    item_id=int(item["id"]),
                    status="cancelled",
                    task_uuid=str(item.get("task_uuid", "")),
                    last_error="rollout terminated by operator",
                    retry_count=int(item.get("retry_count", 0)),
                    last_checked_at=acted_at,
                )
                cancelled_items += 1
        refreshed_items = self.assistant_db.list_ota_batch_run_items(batch_run_id)
        summary = self._summarize_batch_run_items_dicts(
            refreshed_items,
            blocked_device_ids=run.get("summary_payload", {}).get("blocked_device_ids", []),
        )
        control = dict(summary.get("control", {}))
        control.update(
            {
                "terminated": True,
                "terminated_at": acted_at,
                "terminate_operator": operator,
                "terminate_note": decision_note,
                "last_action": "terminate_batch_run",
            }
        )
        summary["control"] = control
        self.assistant_db.update_ota_batch_run_status(batch_run_id, "terminated", summary)
        self._sync_approval_from_batch_run(run, summary, operator, decision_note)
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=self._new_session_id(),
                action_type="terminate_ota_batch_run",
                operator=operator,
                request_payload={"batch_run_id": batch_run_id, "decision_note": decision_note},
                result_payload={"status": "terminated", "cancelled_items": cancelled_items},
                status="ok",
            )
        )
        return {
            "ok": True,
            "batch_run_id": batch_run_id,
            "status": "terminated",
            "cancelled_items": cancelled_items,
            "message": "Pending devices in the batch rollout were cancelled. Existing gateway OTA tasks were not force-stopped.",
        }

    def retry_batch_failed_devices(
        self,
        approval_id: str,
        operator: str = "operator",
        decision_note: str = "",
        session_id_override: str = "",
    ) -> dict[str, Any]:
        approval = self.assistant_db.get_approval_request(approval_id)
        if approval is None:
            return {"ok": False, "message": "Approval request was not found.", "approval_id": approval_id}
        if str(approval.get("request_type", "")) != "ota_batch_create_task":
            return {"ok": False, "message": "Only batch OTA approvals support failed-device retry.", "approval_id": approval_id}
        payload = approval.get("request_payload", {})
        result_payload = approval.get("result_payload", {})
        failed_device_ids = [int(item) for item in result_payload.get("failed_device_ids", [])]
        if not failed_device_ids:
            return {"ok": False, "message": "There are no failed devices to retry.", "approval_id": approval_id}

        firmware_id = str(payload.get("firmware_id", ""))
        transport = str(payload.get("transport", "serial"))
        target = str(payload.get("target", "127.0.0.1:19090"))
        session_id = session_id_override or self._new_session_id()
        firmware_manifest = self.ota_tools.get_firmware_manifest(firmware_id)
        if not firmware_manifest.get("ok"):
            return {
                "ok": False,
                "message": "Cannot retry OTA batch because manifest lookup failed.",
                "approval_id": approval_id,
                "firmware_manifest": firmware_manifest,
            }
        device_type = firmware_manifest.get("manifest", {}).get("device_type", "")
        if not device_type:
            return {
                "ok": False,
                "message": "Cannot retry OTA batch because device_type is missing in manifest.",
                "approval_id": approval_id,
            }

        retry_results: list[dict[str, Any]] = []
        tool_trace: list[dict[str, Any]] = []
        for device_id in failed_device_ids:
            resolved_transport, resolved_target = self._resolve_batch_device_transport(approval, device_id)
            transport_guard = self._check_ota_transport_available(
                transport=resolved_transport,
                target=resolved_target,
                device_id=device_id,
            )
            if not transport_guard.get("ok"):
                create_result = {
                    "ok": False,
                    "message": str(transport_guard.get("message", "Gateway transport is unavailable.")),
                    "reason": "gateway_transport_unavailable",
                    "transport_guard": transport_guard,
                }
                trace_item = {
                    "tool_name": "gateway_transport_guard",
                    "tool_input": {
                        "device_id": device_id,
                        "transport": resolved_transport,
                        "target": resolved_target,
                    },
                    "tool_output": create_result,
                    "status": "error",
                }
            else:
                create_result, trace_item = self._run_tool(
                    session_id,
                    f"retry_batch_failed_devices approval_id={approval_id} device_id={device_id}",
                    "create_ota_task",
                    {
                        "device_id": device_id,
                        "device_type": device_type,
                        "firmware_id": firmware_id,
                        "transport": resolved_transport,
                        "target": resolved_target,
                    },
                    lambda device_id=device_id: self.ota_tools.create_ota_task(
                        device_id=device_id,
                        device_type=device_type,
                        firmware_id=firmware_id,
                        transport=resolved_transport,
                        target=resolved_target,
                    ),
                )
            tool_trace.append(trace_item)
            retry_results.append({"device_id": device_id, "result": create_result})

        retry_failed = [item for item in retry_results if not item["result"].get("ok")]
        acted_at = datetime.now(timezone.utc).isoformat()
        previous_retry_history = result_payload.get("retry_history", [])
        merged_retry_history = list(previous_retry_history) + [
            {
                "acted_by": operator,
                "acted_at": acted_at,
                "decision_note": decision_note,
                "device_ids": failed_device_ids,
                "results": retry_results,
            }
        ]
        new_status = "approved" if not retry_failed else "partial_failed"
        updated_result_payload = dict(result_payload)
        updated_result_payload["status"] = new_status
        updated_result_payload["retry_history"] = merged_retry_history
        updated_result_payload["failed_device_ids"] = [int(item["device_id"]) for item in retry_failed]
        updated_result_payload["failed_count"] = len(retry_failed)
        updated_result_payload["retried_device_ids"] = failed_device_ids
        updated_result_payload["last_retry_results"] = retry_results
        if "created_count" in updated_result_payload:
            updated_result_payload["created_count"] = int(updated_result_payload.get("created_count", 0)) + (
                len(retry_results) - len(retry_failed)
            )
        batch_run_id = str(result_payload.get("batch_run_id", ""))
        if batch_run_id:
            run_items = self.assistant_db.list_ota_batch_run_items(batch_run_id)
            for item in run_items:
                if int(item.get("device_id", 0)) not in failed_device_ids:
                    continue
                matched = next((x for x in retry_results if int(x["device_id"]) == int(item.get("device_id", 0))), None)
                if matched is None:
                    continue
                task = matched["result"].get("task", {}) if isinstance(matched["result"].get("task"), dict) else {}
                self.assistant_db.update_ota_batch_run_item(
                    item_id=int(item["id"]),
                    status="task_created" if matched["result"].get("ok") else "submit_failed",
                    task_uuid=str(task.get("task_uuid", item.get("task_uuid", ""))),
                    last_error="" if matched["result"].get("ok") else str(matched["result"].get("message", "")),
                    retry_count=int(item.get("retry_count", 0)) + 1,
                    last_checked_at=acted_at,
                )
            refreshed_items = self.assistant_db.list_ota_batch_run_items(batch_run_id)
            self.assistant_db.update_ota_batch_run_status(
                batch_run_id,
                new_status,
                self._summarize_batch_run_items_dicts(
                    refreshed_items,
                    blocked_device_ids=payload.get("blocked_device_ids", []),
                ),
            )
        self.assistant_db.update_approval_request_status(
            approval_id=approval_id,
            status=new_status,
            approved_by=operator,
            approved_at=acted_at,
            decision_note=decision_note,
            result_payload=updated_result_payload,
        )
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=session_id,
                action_type="retry_failed_ota_batch_tasks",
                operator=operator,
                request_payload={"approval_id": approval_id, "device_ids": failed_device_ids, "decision_note": decision_note},
                result_payload={"status": new_status, "failed_count": len(retry_failed)},
                status="ok" if not retry_failed else "error",
            )
        )
        return {
            "ok": len(retry_failed) == 0,
            "approval_id": approval_id,
            "status": new_status,
            "approved_by": operator,
            "approved_at": acted_at,
            "decision_note": decision_note,
            "retried_device_ids": failed_device_ids,
            "retry_results": retry_results,
            "remaining_failed_device_ids": updated_result_payload["failed_device_ids"],
            "tool_trace": tool_trace,
            "message": "Retry for failed batch OTA devices has finished.",
        }

    def list_batch_runs(self, limit: int = 20) -> dict[str, Any]:
        items = self.assistant_db.list_ota_batch_runs(limit=limit)
        return {"ok": True, "count": len(items), "items": items}

    def get_batch_run(self, batch_run_id: str) -> dict[str, Any]:
        run = self.assistant_db.get_ota_batch_run(batch_run_id)
        if run is None:
            return {"ok": False, "message": "Batch run was not found.", "batch_run_id": batch_run_id}
        items = self.assistant_db.list_ota_batch_run_items(batch_run_id)
        run["summary_payload"] = self._summarize_batch_run_items_dicts(items, run.get("summary_payload", {}).get("blocked_device_ids", []))
        run["summary_payload"]["sla"] = self._build_batch_run_sla(run, items)
        run["status"] = self._compose_batch_run_status(run, run["summary_payload"])
        return {"ok": True, "batch_run": run, "items": items}

    def refresh_batch_run(self, batch_run_id: str) -> dict[str, Any]:
        run = self.assistant_db.get_ota_batch_run(batch_run_id)
        if run is None:
            return {"ok": False, "message": "Batch run was not found.", "batch_run_id": batch_run_id}
        items = self.assistant_db.list_ota_batch_run_items(batch_run_id)
        refreshed_items: list[dict[str, Any]] = []
        for item in items:
            task_uuid = str(item.get("task_uuid", "")).strip()
            if not task_uuid:
                refreshed_items.append(item)
                continue
            task_status = self.runtime_gateway.get_ota_task_status(task_uuid)
            task = task_status.get("task", {}) if isinstance(task_status.get("task"), dict) else {}
            mapped_status = self._normalize_batch_item_status(
                str(task.get("state", item.get("status", ""))),
                fallback=item.get("status", "unknown"),
            )
            last_error = str(task.get("last_error", "")) or str(task_status.get("message", ""))
            checked_at = datetime.now(timezone.utc).isoformat()
            self.assistant_db.update_ota_batch_run_item(
                item_id=int(item["id"]),
                status=mapped_status,
                task_uuid=task_uuid,
                last_error=last_error if mapped_status in {"failed", "cancelled"} else "",
                retry_count=int(item.get("retry_count", 0)),
                last_checked_at=checked_at,
            )
            updated = dict(item)
            updated["status"] = mapped_status
            updated["last_error"] = last_error if mapped_status in {"failed", "cancelled"} else ""
            updated["last_checked_at"] = checked_at
            refreshed_items.append(updated)
        summary = self._summarize_batch_run_items_dicts(
            refreshed_items,
            blocked_device_ids=run.get("summary_payload", {}).get("blocked_device_ids", []),
        )
        summary["sla"] = self._build_batch_run_sla(run, refreshed_items)
        run_status = self._compose_batch_run_status(run, summary)
        self.assistant_db.update_ota_batch_run_status(batch_run_id, run_status, summary)
        updated_run = self.assistant_db.get_ota_batch_run(batch_run_id)
        if updated_run is not None:
            self._sync_approval_from_batch_run(updated_run, summary, "system", "refresh_batch_run")
        return {"ok": True, "batch_run": updated_run, "items": refreshed_items}

    def approve_request(
        self,
        approval_id: str,
        operator: str = "operator",
        decision_note: str = "",
    ) -> dict[str, Any]:
        approval = self.assistant_db.get_approval_request(approval_id)
        if approval is None:
            return {"ok": False, "message": "Approval request was not found.", "approval_id": approval_id}
        request_type = str(approval.get("request_type", ""))
        if request_type == "ota_batch_create_task":
            return self.workflow_ota_batch_request_confirm(approval_id, operator, decision_note)
        return self.workflow_ota_request_confirm(approval_id, operator, decision_note)

    def _resolve_ota_transport_target(
        self,
        device_id: int,
        transport: str,
        target: str,
    ) -> tuple[str, str, dict[str, Any]]:
        normalized_transport = str(transport or "serial").strip() or "serial"
        normalized_target = str(target or "127.0.0.1:19090").strip() or "127.0.0.1:19090"
        resolution = {
            "mode": "explicit",
            "requested_transport": normalized_transport,
            "requested_target": normalized_target,
        }
        if normalized_transport != "serial" or normalized_target != "127.0.0.1:19090":
            return normalized_transport, normalized_target, resolution

        status = self.runtime_gateway.get_device_status(str(device_id))
        resolved_transport = "serial"
        if status.get("ok"):
            device = status.get("device", {}) if isinstance(status.get("device"), dict) else {}
            resolved_transport = str(
                device.get("active_transport")
                or device.get("link_type")
                or (
                    status.get("latest_sensor_event", {}).get("link_type")
                    if isinstance(status.get("latest_sensor_event"), dict)
                    else ""
                )
                or "serial"
            ).strip() or "serial"
        if resolved_transport not in {"serial", "tcp_binary", "mqtt"}:
            resolved_transport = "serial"
        resolution = {
            "mode": "auto_active_transport",
            "requested_transport": normalized_transport,
            "requested_target": normalized_target,
            "resolved_transport": resolved_transport,
            "resolved_target": normalized_target,
            "status_ok": bool(status.get("ok")),
            "status_source": status.get("status_source", ""),
        }
        return resolved_transport, normalized_target, resolution

    def _resolve_batch_device_transport(self, payload_owner: dict[str, Any], device_id: int) -> tuple[str, str]:
        overrides = payload_owner.get("device_transport_overrides", {})
        if (not isinstance(overrides, dict) or not overrides) and payload_owner.get("approval_id"):
            approval = self.assistant_db.get_approval_request(str(payload_owner.get("approval_id", "")))
            if approval is not None:
                overrides = approval.get("request_payload", {}).get("device_transport_overrides", {})
        if isinstance(overrides, dict):
            override = overrides.get(str(device_id), {})
            if isinstance(override, dict):
                override_transport = str(override.get("transport", "")).strip()
                override_target = str(override.get("target", "")).strip()
                if override_transport:
                    return override_transport, override_target or "127.0.0.1:19090"
        return (
            str(payload_owner.get("transport", "serial")),
            str(payload_owner.get("target", "127.0.0.1:19090")),
        )

    def _check_ota_transport_available(self, transport: str, target: str, device_id: int | None = None) -> dict[str, Any]:
        normalized_transport = str(transport or "serial").strip() or "serial"
        normalized_target = str(target or "127.0.0.1:19090").strip() or "127.0.0.1:19090"
        if normalized_transport != "serial":
            return {
                "ok": True,
                "transport": normalized_transport,
                "target": normalized_target,
                "mode": "bypass_non_serial",
                "message": "Non-serial transport does not depend on gateway serial availability.",
            }

        gateway_status = self.runtime_gateway.get_gateway_status()
        serial_available = gateway_status.get("serial_available")
        if gateway_status.get("ok") and serial_available is False:
            return {
                "ok": False,
                "transport": normalized_transport,
                "target": normalized_target,
                "device_id": device_id,
                "mode": "gateway_status",
                "gateway_status": gateway_status,
                "message": "Gateway serial transport is unavailable. Start the serial link or switch this OTA to tcp_binary/mqtt.",
            }

        if gateway_status.get("ok"):
            return {
                "ok": True,
                "transport": normalized_transport,
                "target": normalized_target,
                "mode": "gateway_status",
                "gateway_status": gateway_status,
                "message": "Gateway serial transport is available.",
            }

        return {
            "ok": True,
            "transport": normalized_transport,
            "target": normalized_target,
            "device_id": device_id,
            "mode": "gateway_status_unknown",
            "gateway_status": gateway_status,
            "message": "Gateway serial transport status is unknown. Continuing with degraded guard because runtime probe failed.",
        }

    def _mark_approval_execution_failed(
        self,
        approval_id: str,
        approved_by: str,
        decision_note: str,
        result_payload: dict[str, Any],
    ) -> dict[str, Any]:
        failed_at = datetime.now(timezone.utc).isoformat()
        merged_payload = {"status": "execute_failed", **result_payload}
        self.assistant_db.update_approval_request_status(
            approval_id=approval_id,
            status="execute_failed",
            approved_by=approved_by,
            approved_at=failed_at,
            decision_note=decision_note,
            result_payload=merged_payload,
        )
        return {
            "status": "execute_failed",
            "approved_at": failed_at,
            "result_payload": merged_payload,
        }

    def list_tool_call_logs(self, limit: int = 50, session_id: str = "") -> dict[str, Any]:
        items = self.assistant_db.list_tool_call_logs(limit=limit, session_id=session_id)
        return {"ok": True, "count": len(items), "items": items}

    def list_action_audits(self, limit: int = 50, session_id: str = "") -> dict[str, Any]:
        items = self.assistant_db.list_action_audits(limit=limit, session_id=session_id)
        return {"ok": True, "count": len(items), "items": items}

    def ingest_realtime_event(
        self,
        event_type: str,
        *,
        source: str = "api",
        severity: str = "info",
        device_id: int | None = None,
        payload: dict[str, Any] | None = None,
        happened_at: str = "",
    ) -> dict[str, Any]:
        normalized_payload = payload or {}
        envelope = EventEnvelope(
            event_type=event_type.strip(),
            source=source.strip() or "api",
            severity=severity.strip() or "info",
            payload=normalized_payload,
            device_id=device_id,
            happened_at=happened_at.strip(),
        )
        event_id = f"evt-{uuid4().hex[:12]}"
        event_record = RealtimeEventRecord(
            event_id=event_id,
            event_type=envelope.event_type,
            source=envelope.source,
            payload=envelope.payload,
            severity=envelope.severity,
            device_id=envelope.device_id,
            happened_at=envelope.happened_at,
            ingest_status="processed",
        )
        self.assistant_db.create_realtime_event(event_record)
        stored_event = self.assistant_db.get_realtime_event(event_id) or {}
        alert_records: list[dict[str, Any]] = []
        decision_records: list[dict[str, Any]] = []
        for proposal in self.alert_pipeline.evaluate(envelope):
            existing_alert = self.assistant_db.get_open_alert_by_dedupe_key(proposal.dedupe_key)
            policy_meta = self.decision_engine.first_policy_for_rule(proposal.rule_id) or {}
            if existing_alert is None:
                alert_id = f"alert-{uuid4().hex[:12]}"
                self.assistant_db.create_alert_record(
                    AlertRecord(
                        alert_id=alert_id,
                        rule_id=proposal.rule_id,
                        title=proposal.title,
                        severity=proposal.severity,
                        status="open",
                        summary=proposal.summary,
                        dedupe_key=proposal.dedupe_key,
                        payload=proposal.payload,
                        event_id=event_id,
                        device_id=device_id,
                    )
                )
                alert = self.assistant_db.get_alert_record(alert_id) or {}
                if proposal.severity in {"high", "critical"}:
                    self._create_notification(
                        source_type="alert",
                        source_id=alert_id,
                        target=str(policy_meta.get("notification_target", "oncall")),
                        severity=proposal.severity,
                        title=proposal.title,
                        body=proposal.summary,
                    )
            else:
                suppression_until = str(existing_alert.get("suppressed_until", "")).strip()
                if str(existing_alert.get("status", "")) == "suppressed" and suppression_until and suppression_until > datetime.now(timezone.utc).isoformat():
                    alert = self.assistant_db.update_alert_record(
                        str(existing_alert.get("alert_id", "")),
                        status="suppressed",
                        severity=proposal.severity,
                        summary=proposal.summary,
                        payload=proposal.payload,
                        event_id=event_id,
                        occurrence_count=int(existing_alert.get("occurrence_count", 1)) + 1,
                    ) or existing_alert
                    alert_records.append(alert)
                    continue
                next_occurrence_count = int(existing_alert.get("occurrence_count", 1)) + 1
                next_severity = proposal.severity
                escalation_level = int(existing_alert.get("escalation_level", 0))
                escalated_at = str(existing_alert.get("escalated_at", ""))
                if next_occurrence_count >= 3:
                    next_severity = self._escalate_alert_severity(str(existing_alert.get("severity", proposal.severity)))
                    if next_severity != str(existing_alert.get("severity", proposal.severity)):
                        escalation_level += 1
                        escalated_at = datetime.now(timezone.utc).isoformat()
                        self._create_notification(
                            source_type="alert_escalation",
                            source_id=str(existing_alert.get("alert_id", "")),
                            target=str(policy_meta.get("escalation_target", policy_meta.get("notification_target", "supervisor"))),
                            severity=next_severity,
                            title=f"Escalated: {proposal.title}",
                            body=f"{proposal.summary} | escalation_level={escalation_level}",
                        )
                alert = self.assistant_db.update_alert_record(
                    str(existing_alert.get("alert_id", "")),
                    status="open",
                    severity=next_severity,
                    summary=proposal.summary,
                    payload=proposal.payload,
                    event_id=event_id,
                    escalation_level=escalation_level,
                    escalated_at=escalated_at,
                    occurrence_count=next_occurrence_count,
                ) or existing_alert
            alert_records.append(alert)
            for decision_proposal in self.decision_engine.evaluate_alert(alert):
                existing_decision = self.assistant_db.get_active_decision_by_policy(
                    str(alert.get("alert_id", "")),
                    decision_proposal.policy.policy_id,
                )
                if existing_decision is not None:
                    decision = self.assistant_db.update_decision_record(
                        str(existing_decision.get("decision_id", "")),
                        reason=decision_proposal.reason,
                        payload=decision_proposal.payload,
                    ) or existing_decision
                else:
                    decision_id = f"decision-{uuid4().hex[:12]}"
                    self.assistant_db.create_decision_record(
                        DecisionRecord(
                            decision_id=decision_id,
                            policy_id=decision_proposal.policy.policy_id,
                            policy_version=decision_proposal.policy.version,
                            action_type=decision_proposal.policy.action_type,
                            reason=decision_proposal.reason,
                            risk_level=decision_proposal.policy.risk_level,
                            status="proposed",
                            payload=decision_proposal.payload,
                            alert_id=str(alert.get("alert_id", "")),
                            event_id=event_id,
                            target_type=decision_proposal.policy.target_type,
                            target_id=decision_proposal.target_id,
                            requires_approval=decision_proposal.policy.requires_approval,
                        )
                    )
                    decision = self.assistant_db.get_decision_record(decision_id) or {}
                decision_records.append(decision)
        self.snapshot_decision_metrics(reason=f"event_ingested:{envelope.event_type}")
        return {
            "ok": True,
            "event": stored_event,
            "alerts": alert_records,
            "alert_count": len(alert_records),
            "decisions": decision_records,
            "decision_count": len(decision_records),
        }

    def list_realtime_events(
        self,
        limit: int = 50,
        event_type: str = "",
        severity: str = "",
        device_id: int | None = None,
    ) -> dict[str, Any]:
        items = self.assistant_db.list_realtime_events(limit=limit, event_type=event_type, severity=severity, device_id=device_id)
        return {"ok": True, "count": len(items), "items": items}

    def list_alert_records(self, limit: int = 50, status: str = "", severity: str = "") -> dict[str, Any]:
        items = self.assistant_db.list_alert_records(limit=limit, status=status, severity=severity)
        return {"ok": True, "count": len(items), "items": items}

    def suppress_alert(self, alert_id: str, actor: str = "operator", minutes: int = 30, reason: str = "") -> dict[str, Any]:
        permission = self._require_actor_role(actor, "operator")
        if not permission.get("ok"):
            return permission
        suppressed_until = datetime.now(timezone.utc).timestamp() + max(1, minutes) * 60
        until_iso = datetime.fromtimestamp(suppressed_until, timezone.utc).isoformat()
        updated = self.assistant_db.update_alert_record(
            alert_id,
            status="suppressed",
            suppressed_until=until_iso,
            suppression_reason=reason.strip() or f"Suppressed by {actor}",
        )
        if updated is None:
            return {"ok": False, "message": "Alert was not found.", "alert_id": alert_id}
        return {"ok": True, "alert": updated, "actor_role": permission["actor_role"]}

    def acknowledge_alert(self, alert_id: str, actor: str = "operator") -> dict[str, Any]:
        permission = self._require_actor_role(actor, "operator")
        if not permission.get("ok"):
            return permission
        updated = self.assistant_db.update_alert_record(
            alert_id,
            status="acknowledged",
            acknowledged_by=actor,
        )
        if updated is None:
            return {"ok": False, "message": "Alert was not found.", "alert_id": alert_id}
        return {"ok": True, "alert": updated, "actor_role": permission["actor_role"]}

    def resolve_alert(self, alert_id: str, actor: str = "operator") -> dict[str, Any]:
        permission = self._require_actor_role(actor, "operator")
        if not permission.get("ok"):
            return permission
        updated = self.assistant_db.update_alert_record(
            alert_id,
            status="resolved",
            resolved_at=datetime.now(timezone.utc).isoformat(),
        )
        if updated is None:
            return {"ok": False, "message": "Alert was not found.", "alert_id": alert_id}
        return {"ok": True, "alert": updated, "actor_role": permission["actor_role"]}

    def list_decision_records(self, limit: int = 50, status: str = "", risk_level: str = "") -> dict[str, Any]:
        items = self.assistant_db.list_decision_records(limit=limit, status=status, risk_level=risk_level)
        return {"ok": True, "count": len(items), "items": items}

    def list_decision_policies(self) -> dict[str, Any]:
        items = self.decision_engine.list_policies()
        return {"ok": True, "count": len(items), "items": items}

    def reload_decision_policies(self) -> dict[str, Any]:
        items = self.decision_engine.reload()
        return {"ok": True, "count": len(items), "items": items}

    def list_execution_records(self, limit: int = 50, status: str = "") -> dict[str, Any]:
        items = self.assistant_db.list_execution_records(limit=limit, status=status)
        return {"ok": True, "count": len(items), "items": items}

    def list_rollback_records(self, limit: int = 50, status: str = "") -> dict[str, Any]:
        items = self.assistant_db.list_rollback_records(limit=limit, status=status)
        return {"ok": True, "count": len(items), "items": items}

    def list_notification_records(self, limit: int = 50, status: str = "", severity: str = "") -> dict[str, Any]:
        items = self.assistant_db.list_notification_records(limit=limit, status=status, severity=severity)
        return {"ok": True, "count": len(items), "items": items}

    def list_decision_metric_snapshots(self, limit: int = 100, metric_name: str = "") -> dict[str, Any]:
        items = self.assistant_db.list_decision_metric_snapshots(limit=limit, metric_name=metric_name)
        return {"ok": True, "count": len(items), "items": items, "metric_name": metric_name}

    def export_decision_metrics_csv(self, metric_name: str = "", limit: int = 200) -> dict[str, Any]:
        snapshots = self.assistant_db.list_decision_metric_snapshots(limit=limit, metric_name=metric_name)
        buffer = io.StringIO()
        writer = csv.DictWriter(buffer, fieldnames=["snapshot_id", "metric_name", "metric_value", "dimensions", "created_at"])
        writer.writeheader()
        for item in reversed(snapshots):
            writer.writerow(
                {
                    "snapshot_id": item.get("snapshot_id", ""),
                    "metric_name": item.get("metric_name", ""),
                    "metric_value": item.get("metric_value", 0),
                    "dimensions": json.dumps(item.get("dimensions", {}), ensure_ascii=False, sort_keys=True),
                    "created_at": item.get("created_at", ""),
                }
            )
        return {"ok": True, "csv": buffer.getvalue(), "count": len(snapshots), "metric_name": metric_name}

    def execute_decision(
        self,
        decision_id: str,
        *,
        actor: str = "operator",
        decision_note: str = "",
        force_execute: bool = False,
    ) -> dict[str, Any]:
        permission = self._require_actor_role(actor, "operator")
        if not permission.get("ok"):
            return permission
        decision = self.assistant_db.get_decision_record(decision_id)
        if decision is None:
            return {"ok": False, "message": "Decision was not found.", "decision_id": decision_id}
        guardrail = self._evaluate_decision_guardrails(decision, actor=actor, force_execute=force_execute)
        if not guardrail.get("ok"):
            execution_id = f"exec-{uuid4().hex[:12]}"
            self.assistant_db.create_execution_record(
                ExecutionRecord(
                    execution_id=execution_id,
                    decision_id=decision_id,
                    action_type=str(decision.get("action_type", "")),
                    operator=actor,
                    status="blocked",
                    guardrail_status=str(guardrail.get("guardrail_status", "blocked")),
                    input_payload={"decision_note": decision_note, "force_execute": force_execute},
                    result_payload=guardrail,
                )
            )
            return {"ok": False, "decision": decision, "execution": self.assistant_db.get_execution_record(execution_id), **guardrail}
        execution_id = f"exec-{uuid4().hex[:12]}"
        self.assistant_db.create_execution_record(
            ExecutionRecord(
                execution_id=execution_id,
                decision_id=decision_id,
                action_type=str(decision.get("action_type", "")),
                operator=actor,
                status="executing",
                guardrail_status=str(guardrail.get("guardrail_status", "passed")),
                input_payload={"decision_note": decision_note, "force_execute": force_execute},
                result_payload={"message": "Execution started."},
            )
        )
        result = self._run_decision_action(decision, actor=actor, decision_note=decision_note)
        execution_status = "executed" if result.get("ok") else "failed"
        updated_execution = self.assistant_db.update_execution_record(
            execution_id,
            status=execution_status,
            guardrail_status="passed",
            result_payload=result,
        )
        self.assistant_db.update_decision_record(
            decision_id,
            status="executed" if result.get("ok") else "failed",
            reason=f"{decision.get('reason', '')} | executed_by={actor}",
            payload={**dict(decision.get("payload", {})), "execution_id": execution_id, "latest_result": result},
        )
        self.snapshot_decision_metrics(reason=f"decision_execute:{decision_id}")
        return {
            "ok": bool(result.get("ok")),
            "decision": self.assistant_db.get_decision_record(decision_id),
            "execution": updated_execution,
            "result": result,
        }

    def rollback_decision_execution(
        self,
        decision_id: str,
        *,
        actor: str = "operator",
        reason: str = "",
    ) -> dict[str, Any]:
        permission = self._require_actor_role(actor, "approver")
        if not permission.get("ok"):
            return permission
        decision = self.assistant_db.get_decision_record(decision_id)
        if decision is None:
            return {"ok": False, "message": "Decision was not found.", "decision_id": decision_id}
        execution = self.assistant_db.get_latest_execution_for_decision(decision_id)
        if execution is None:
            return {"ok": False, "message": "No execution record exists for this decision.", "decision_id": decision_id, "status": "blocked"}
        if str(execution.get("status", "")) != "executed":
            return {"ok": False, "message": "Only executed decisions can be rolled back.", "decision_id": decision_id, "status": "blocked"}
        rollback_result = self._run_decision_rollback(decision, execution, actor=actor, reason=reason)
        rollback_id = f"rollback-{uuid4().hex[:12]}"
        self.assistant_db.create_rollback_record(
            RollbackRecord(
                rollback_id=rollback_id,
                execution_id=str(execution.get("execution_id", "")),
                decision_id=decision_id,
                operator=actor,
                status="rolled_back" if rollback_result.get("ok") else "rollback_failed",
                reason=reason,
                result_payload=rollback_result,
            )
        )
        if rollback_result.get("ok"):
            self.assistant_db.update_decision_record(
                decision_id,
                status="rolled_back",
                payload={**dict(decision.get("payload", {})), "rollback_id": rollback_id, "rollback_result": rollback_result},
            )
        self.snapshot_decision_metrics(reason=f"decision_rollback:{decision_id}")
        return {
            "ok": bool(rollback_result.get("ok")),
            "decision": self.assistant_db.get_decision_record(decision_id),
            "execution": execution,
            "rollback": self.assistant_db.list_rollback_records(limit=1)[0] if self.assistant_db.list_rollback_records(limit=1) else None,
            "result": rollback_result,
        }

    def decision_metrics(self) -> dict[str, Any]:
        alerts = self.assistant_db.list_alert_records(limit=200)
        decisions = self.assistant_db.list_decision_records(limit=200)
        executions = self.assistant_db.list_execution_records(limit=200)
        rollbacks = self.assistant_db.list_rollback_records(limit=200)
        total_alerts = len(alerts)
        open_alerts = len([item for item in alerts if str(item.get("status", "")) == "open"])
        total_decisions = len(decisions)
        approval_required = len([item for item in decisions if bool(item.get("requires_approval"))])
        risk_counts: dict[str, int] = {}
        for item in decisions:
            risk_level = str(item.get("risk_level", "unknown"))
            risk_counts[risk_level] = risk_counts.get(risk_level, 0) + 1
        execution_status_counts: dict[str, int] = {}
        blocked_reason_counts: dict[str, int] = {}
        for item in executions:
            status = str(item.get("status", "unknown"))
            execution_status_counts[status] = execution_status_counts.get(status, 0) + 1
            if status == "blocked":
                reason = str(dict(item.get("result_payload", {})).get("guardrail_status", "unknown"))
                blocked_reason_counts[reason] = blocked_reason_counts.get(reason, 0) + 1
        executed_count = execution_status_counts.get("executed", 0)
        failed_count = execution_status_counts.get("failed", 0)
        blocked_count = execution_status_counts.get("blocked", 0)
        rollback_count = len([item for item in rollbacks if str(item.get("status", "")) == "rolled_back"])
        rollback_failed_count = len([item for item in rollbacks if str(item.get("status", "")) == "rollback_failed"])
        execution_attempts = executed_count + failed_count
        success_rate = round((executed_count / execution_attempts), 4) if execution_attempts else 0.0
        rollback_rate = round((rollback_count / executed_count), 4) if executed_count else 0.0
        blocked_rate = round((blocked_count / len(executions)), 4) if executions else 0.0
        latest_snapshots = self.assistant_db.list_decision_metric_snapshots(limit=20)
        return {
            "ok": True,
            "metrics": {
                "total_events": len(self.assistant_db.list_realtime_events(limit=200)),
                "total_alerts": total_alerts,
                "open_alerts": open_alerts,
                "resolved_alerts": len([item for item in alerts if str(item.get("status", "")) == "resolved"]),
                "total_decisions": total_decisions,
                "approval_required_decisions": approval_required,
                "approval_rate": round((approval_required / total_decisions), 4) if total_decisions else 0.0,
                "executed_decisions": executed_count,
                "failed_executions": failed_count,
                "blocked_executions": blocked_count,
                "execution_success_rate": success_rate,
                "rollback_count": rollback_count,
                "rollback_failed_count": rollback_failed_count,
                "rollback_rate": rollback_rate,
                "blocked_rate": blocked_rate,
                "risk_counts": risk_counts,
                "execution_status_counts": execution_status_counts,
                "blocked_reason_counts": blocked_reason_counts,
            }
            ,
            "history": latest_snapshots,
        }

    def list_memory_audits(
        self,
        limit: int = 50,
        memory_key: str = "",
        actor: str = "",
        action_type: str = "",
    ) -> dict[str, Any]:
        items = self.assistant_db.list_memory_audits(
            limit=limit,
            memory_key=memory_key,
            actor=actor,
            action_type=action_type,
        )
        return {
            "ok": True,
            "count": len(items),
            "items": items,
            "memory_key": memory_key,
            "actor": actor,
            "action_type": action_type,
        }

    def list_reports(self, limit: int = 20) -> dict[str, Any]:
        items = self.assistant_db.list_report_records(limit=limit)
        return {"ok": True, "count": len(items), "items": items}

    def get_report(self, report_id: str) -> dict[str, Any]:
        item = self.assistant_db.get_report_record(report_id)
        if item is None:
            return {"ok": False, "message": "Report was not found.", "report_id": report_id}
        return {"ok": True, "report": item}

    def get_report_markdown(self, report_id: str) -> dict[str, Any]:
        item = self.assistant_db.get_report_record(report_id)
        if item is None:
            return {"ok": False, "message": "Report was not found.", "report_id": report_id}
        markdown_path = item.get("markdown_path", "")
        try:
            markdown = Path(str(markdown_path)).read_text(encoding="utf-8")
        except Exception as exc:
            return {
                "ok": False,
                "message": "Failed to read markdown report.",
                "report_id": report_id,
                "error": f"{type(exc).__name__}: {exc}",
            }
        return {"ok": True, "report_id": report_id, "markdown": markdown, "report": item}

    def create_ticket(
        self,
        title: str = "",
        severity: str = "medium",
        device_id: int | None = None,
        report_id: str = "",
        description: str = "",
    ) -> dict[str, Any]:
        if report_id:
            report = self.assistant_db.get_report_record(report_id)
            if report is None:
                return {"ok": False, "message": "Report was not found.", "report_id": report_id}
            if not title:
                title = str(report.get("title", "Generated Ticket"))
            if device_id is None:
                raw_device_id = report.get("device_id")
                device_id = int(raw_device_id) if raw_device_id is not None else None
            if not description:
                description = str(report.get("summary", ""))
        ticket_id = f"ticket-{uuid4().hex[:12]}"
        final_title = title or "Generated Ticket"
        final_description = description or "No description provided."
        self.assistant_db.create_ticket_record(
            TicketRecord(
                ticket_id=ticket_id,
                title=final_title,
                severity=severity,
                status="open",
                device_id=device_id,
                report_id=report_id,
                description=final_description,
            )
        )
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=self._new_session_id(),
                action_type="create_ticket",
                operator="agent",
                request_payload={
                    "title": final_title,
                    "severity": severity,
                    "device_id": device_id,
                    "report_id": report_id,
                },
                result_payload={"ticket_id": ticket_id, "status": "open"},
                status="ok",
            )
        )
        return {
            "ok": True,
            "ticket_id": ticket_id,
            "title": final_title,
            "severity": severity,
            "status": "open",
            "device_id": device_id,
            "report_id": report_id,
            "description": final_description,
        }

    def list_tickets(self, limit: int = 20, status: str = "", severity: str = "", assignee: str = "") -> dict[str, Any]:
        items = self.assistant_db.list_ticket_records(limit=limit, status=status, severity=severity, assignee=assignee)
        return {"ok": True, "count": len(items), "items": items}

    def get_ticket(self, ticket_id: str) -> dict[str, Any]:
        item = self.assistant_db.get_ticket_record(ticket_id)
        if item is None:
            return {"ok": False, "message": "Ticket was not found.", "ticket_id": ticket_id}
        return {"ok": True, "ticket": item}

    def assign_ticket(self, ticket_id: str, assignee: str) -> dict[str, Any]:
        item = self.assistant_db.get_ticket_record(ticket_id)
        if item is None:
            return {"ok": False, "message": "Ticket was not found.", "ticket_id": ticket_id}
        self.assistant_db.update_ticket_assignment(ticket_id, assignee)
        updated = self.assistant_db.get_ticket_record(ticket_id)
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=self._new_session_id(),
                action_type="assign_ticket",
                operator=assignee,
                request_payload={"ticket_id": ticket_id, "assignee": assignee},
                result_payload={"ticket_id": ticket_id, "status": updated.get("status", ""), "assignee": assignee},
                status="ok",
            )
        )
        return {"ok": True, "ticket": updated}

    def close_ticket(self, ticket_id: str) -> dict[str, Any]:
        item = self.assistant_db.get_ticket_record(ticket_id)
        if item is None:
            return {"ok": False, "message": "Ticket was not found.", "ticket_id": ticket_id}
        self.assistant_db.update_ticket_status(ticket_id, "closed")
        updated = self.assistant_db.get_ticket_record(ticket_id)
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=self._new_session_id(),
                action_type="close_ticket",
                operator="operator",
                request_payload={"ticket_id": ticket_id},
                result_payload={"ticket_id": ticket_id, "status": "closed"},
                status="ok",
            )
        )
        return {"ok": True, "ticket": updated}

    def reopen_ticket(self, ticket_id: str) -> dict[str, Any]:
        item = self.assistant_db.get_ticket_record(ticket_id)
        if item is None:
            return {"ok": False, "message": "Ticket was not found.", "ticket_id": ticket_id}
        self.assistant_db.update_ticket_status(ticket_id, "open")
        updated = self.assistant_db.get_ticket_record(ticket_id)
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=self._new_session_id(),
                action_type="reopen_ticket",
                operator="operator",
                request_payload={"ticket_id": ticket_id},
                result_payload={"ticket_id": ticket_id, "status": "open"},
                status="ok",
            )
        )
        return {"ok": True, "ticket": updated}

    def agent_plan(self, question: str) -> dict[str, Any]:
        return self.agent_planner.plan(question)

    def agent_execute(self, question: str) -> dict[str, Any]:
        session_id = f"agent-{uuid4().hex[:12]}"
        plan = self.agent_planner.plan(question)
        execution = self.agent_executor.execute(plan, self, session_id=session_id)
        execution_summary = execution.get("execution_summary", {})
        self.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id=session_id,
                action_type="agent_execute",
                operator="agent",
                request_payload={
                    "question": question,
                    "goal": plan.get("goal", ""),
                    "step_count": plan.get("step_count", 0),
                    "approval_required": plan.get("approval_required", False),
                },
                result_payload={
                    "final_status": execution_summary.get("final_status", "unknown"),
                    "completed_steps": execution_summary.get("completed_steps", 0),
                    "failed_step_id": execution_summary.get("failed_step_id"),
                    "final_action": execution_summary.get("final_action", ""),
                    "tool_call_count": execution_summary.get("tool_call_count", 0),
                },
                status="ok" if execution.get("ok") else "error",
            )
        )
        return {
            "ok": execution.get("ok", False),
            "session_id": session_id,
            "plan": plan,
            "execution": execution,
            "execution_summary": execution_summary,
        }

    def get_agent_session(self, session_id: str) -> dict[str, Any]:
        action_audits = self.assistant_db.list_action_audits(limit=50, session_id=session_id)
        tool_calls = self.assistant_db.list_tool_call_logs(limit=200, session_id=session_id)
        agent_context: dict[str, Any] | None = None
        if self.settings.llm_api_key:
            try:
                agent_context = self.react_agent.inspect_session(session_id)
            except Exception:
                agent_context = None
        return {
            "ok": True,
            "session_id": session_id,
            "action_audits": action_audits,
            "tool_calls": tool_calls,
            "action_count": len(action_audits),
            "tool_call_count": len(tool_calls),
            "agent_context": agent_context,
            "context_warning": bool(agent_context and agent_context.get("context_warning")),
            "memory": self.get_agent_memory_snapshot(session_id),
        }

    def export_agent_session_replay(self, session_id: str) -> dict[str, Any]:
        session = self.get_agent_session(session_id)
        action_audits = list(session.get("action_audits", []))
        tool_calls = list(session.get("tool_calls", []))
        timeline = self._build_session_timeline(action_audits, tool_calls)
        return {
            "ok": True,
            "session_id": session_id,
            "timeline": timeline,
            "timeline_count": len(timeline),
            "action_audits": action_audits,
            "tool_calls": tool_calls,
            "agent_context": session.get("agent_context"),
            "context_warning": session.get("context_warning", False),
            "memory": session.get("memory", {}),
        }

    def get_agent_memory_snapshot(self, session_id: str, question: str = "") -> dict[str, Any]:
        session_memory = self.assistant_db.get_session_memory(session_id)
        long_term_memories = self._filter_active_long_term_memories(
            self.assistant_db.list_long_term_memories(
                scope="global",
                limit=self.settings.agent_memory_long_term_limit,
            )
        )
        all_long_term_memories = self.assistant_db.list_long_term_memories(
            scope="global",
            limit=self.settings.agent_memory_long_term_limit,
        )
        recent_memory_audits = self.assistant_db.list_memory_audits(limit=10)
        related_session_memories = self._retrieve_related_session_memories(session_id, question)
        if long_term_memories:
            self.assistant_db.touch_long_term_memories(
                [int(item["id"]) for item in long_term_memories if str(item.get("id", "")).isdigit()]
            )
        return {
            "ok": True,
            "session_id": session_id,
            "session_memory": session_memory,
            "related_session_memories": related_session_memories,
            "related_session_count": len(related_session_memories),
            "long_term_memories": long_term_memories,
            "long_term_count": len(long_term_memories),
            "inactive_long_term_count": max(0, len(all_long_term_memories) - len(long_term_memories)),
            "memory_audits": recent_memory_audits,
            "memory_audit_count": len(recent_memory_audits),
        }

    def list_agent_memories(self, limit: int = 20) -> dict[str, Any]:
        session_memories = self.assistant_db.list_session_memories(limit=limit)
        long_term_memories = self.assistant_db.list_long_term_memories(scope="global", limit=limit)
        memory_candidates = self.assistant_db.list_memory_candidates(limit=limit, status="review_queue")
        memory_audits = self.assistant_db.list_memory_audits(limit=limit)
        return {
            "ok": True,
            "session_memories": session_memories,
            "long_term_memories": long_term_memories,
            "memory_candidates": memory_candidates,
            "memory_audits": memory_audits,
            "session_count": len(session_memories),
            "long_term_count": len(long_term_memories),
            "candidate_count": len(memory_candidates),
            "memory_audit_count": len(memory_audits),
        }

    def list_memory_candidates(self, limit: int = 20, status: str = "review_queue") -> dict[str, Any]:
        items = self.assistant_db.list_memory_candidates(limit=limit, status=status)
        return {"ok": True, "items": items, "count": len(items), "status": status}

    def approve_memory_candidate(
        self,
        candidate_id: str,
        reviewed_by: str = "operator",
        decision_note: str = "",
    ) -> dict[str, Any]:
        permission = self._require_actor_role(reviewed_by, "approver")
        if not permission.get("ok"):
            return permission
        candidate = self.assistant_db.get_memory_candidate(candidate_id)
        if candidate is None:
            return {"ok": False, "message": "Memory candidate was not found.", "candidate_id": candidate_id}
        candidate_status = str(candidate.get("status", ""))
        requires_second_review = self._memory_candidate_requires_second_review(candidate)
        if candidate_status not in {"pending", "stage1_approved"}:
            return {"ok": False, "message": "Memory candidate is not pending anymore.", "candidate_id": candidate_id}
        if requires_second_review and candidate_status == "pending":
            reviewed = self.assistant_db.review_memory_candidate(
                candidate_id,
                status="stage1_approved",
                reviewed_by=reviewed_by,
                decision_note=decision_note,
                stage="stage1",
            )
            self.assistant_db.log_memory_audit(
                MemoryAuditLogRecord(
                    memory_scope=str(candidate.get("scope", "global")),
                    memory_key=str(candidate.get("memory_key", "")),
                    candidate_id=candidate_id,
                    action_type="candidate_stage1_approved",
                    actor=reviewed_by,
                    actor_role=str(permission.get("actor_role", "")),
                    before_state=candidate,
                    after_state=reviewed or {},
                    reason=decision_note.strip() or "High-risk candidate passed stage-1 review.",
                )
            )
            return {
                "ok": True,
                "candidate": reviewed,
                "stage": "stage1",
                "requires_second_review": True,
                "message": "Memory candidate requires a second approver before promotion.",
            }
        if requires_second_review and candidate_status == "stage1_approved":
            first_reviewer = str(candidate.get("first_reviewed_by", "")).strip()
            if first_reviewer and first_reviewer == reviewed_by:
                return {
                    "ok": False,
                    "message": "High-risk memory candidate requires a different second approver.",
                    "candidate_id": candidate_id,
                    "required_role": "approver",
                }
        memory_result = self.create_or_update_long_term_memory(
            memory_key=str(candidate.get("memory_key", "")),
            content=str(candidate.get("content", "")),
            scope=str(candidate.get("scope", "global")),
            confidence=float(candidate.get("confidence", 0.6)),
            source_session_id=str(candidate.get("session_id", "")),
            memory_type=str(candidate.get("memory_type", "fact")),
            provenance=self._normalize_memory_provenance(str(candidate.get("provenance", "user_stated"))),
            status="active",
            actor=reviewed_by,
        )
        if not memory_result.get("ok"):
            return memory_result
        reviewed = self.assistant_db.review_memory_candidate(
            candidate_id,
            status="approved",
            reviewed_by=reviewed_by,
            decision_note=decision_note,
            stage="final",
        )
        self.assistant_db.log_memory_audit(
            MemoryAuditLogRecord(
                memory_scope=str(candidate.get("scope", "global")),
                memory_key=str(candidate.get("memory_key", "")),
                memory_id=int(memory_result.get("memory", {}).get("id")) if str(memory_result.get("memory", {}).get("id", "")).isdigit() else None,
                candidate_id=candidate_id,
                action_type="candidate_approved",
                actor=reviewed_by,
                actor_role=str(permission.get("actor_role", "")),
                before_state=candidate,
                after_state=reviewed or {},
                reason=decision_note.strip(),
            )
        )
        return {
            "ok": True,
            "candidate": reviewed,
            "memory": memory_result.get("memory"),
            "stage": "final",
            "requires_second_review": requires_second_review,
        }

    def reject_memory_candidate(
        self,
        candidate_id: str,
        reviewed_by: str = "operator",
        decision_note: str = "",
    ) -> dict[str, Any]:
        permission = self._require_actor_role(reviewed_by, "approver")
        if not permission.get("ok"):
            return permission
        candidate = self.assistant_db.get_memory_candidate(candidate_id)
        if candidate is None:
            return {"ok": False, "message": "Memory candidate was not found.", "candidate_id": candidate_id}
        reviewed = self.assistant_db.review_memory_candidate(
            candidate_id,
            status="rejected",
            reviewed_by=reviewed_by,
            decision_note=decision_note,
            stage="final",
        )
        self.assistant_db.log_memory_audit(
            MemoryAuditLogRecord(
                memory_scope=str(candidate.get("scope", "global")),
                memory_key=str(candidate.get("memory_key", "")),
                candidate_id=candidate_id,
                action_type="candidate_rejected",
                actor=reviewed_by,
                actor_role=str(permission.get("actor_role", "")),
                before_state=candidate,
                after_state=reviewed or {},
                reason=decision_note.strip(),
            )
        )
        return {"ok": True, "candidate": reviewed}

    def upsert_agent_session_memory(
        self,
        session_id: str,
        summary: str,
        key_facts: dict[str, Any] | None = None,
        last_question: str = "",
        last_answer: str = "",
        actor: str = "operator",
    ) -> dict[str, Any]:
        permission = self._require_actor_role(actor, "operator")
        if not permission.get("ok"):
            return permission
        self.assistant_db.upsert_session_memory(
            SessionMemoryRecord(
                session_id=session_id.strip(),
                summary=summary.strip(),
                key_facts=key_facts or {},
                last_question=last_question.strip(),
                last_answer=self._truncate_memory_text(last_answer),
            )
        )
        return {"ok": True, "session_memory": self.assistant_db.get_session_memory(session_id.strip()), "actor_role": permission["actor_role"]}

    def delete_agent_session_memory(self, session_id: str, actor: str = "operator") -> dict[str, Any]:
        permission = self._require_actor_role(actor, "operator")
        if not permission.get("ok"):
            return permission
        deleted = self.assistant_db.delete_session_memory(session_id.strip())
        return {"ok": deleted, "session_id": session_id.strip(), "deleted": deleted, "actor_role": permission["actor_role"]}

    def create_or_update_long_term_memory(
        self,
        memory_key: str,
        content: str,
        *,
        scope: str = "global",
        confidence: float = 0.7,
        source_session_id: str = "",
        memory_type: str = "manual",
        provenance: str = "manual_curated",
        status: str = "active",
        is_pinned: bool = False,
        expires_at: str = "",
        actor: str = "operator",
    ) -> dict[str, Any]:
        permission = self._require_actor_role(actor, "approver")
        if not permission.get("ok"):
            return permission
        existing_memory = next(
            (
                item
                for item in self.assistant_db.list_long_term_memories(scope=scope.strip() or "global", limit=100)
                if str(item.get("memory_key", "")) == memory_key.strip()
            ),
            None,
        )
        self.assistant_db.upsert_long_term_memory(
            LongTermMemoryRecord(
                scope=scope.strip() or "global",
                memory_key=memory_key.strip(),
                content=content.strip(),
                confidence=max(0.0, min(float(confidence), 1.0)),
                source_session_id=source_session_id.strip(),
                memory_type=memory_type.strip() or "manual",
                provenance=self._normalize_memory_provenance(provenance),
                status=status.strip() or "active",
                is_pinned=bool(is_pinned),
                expires_at=expires_at.strip(),
            )
        )
        memory = next(
            (
                item
                for item in self.assistant_db.list_long_term_memories(scope=scope.strip() or "global", limit=100)
                if str(item.get("memory_key", "")) == memory_key.strip()
            ),
            None,
        )
        self.assistant_db.log_memory_audit(
            MemoryAuditLogRecord(
                memory_scope=scope.strip() or "global",
                memory_key=memory_key.strip(),
                memory_id=int(memory.get("id")) if memory and str(memory.get("id", "")).isdigit() else None,
                action_type="long_term_upserted",
                actor=actor,
                actor_role=str(permission.get("actor_role", "")),
                before_state=existing_memory or {},
                after_state=memory or {},
                reason="Manual create/update long-term memory." if actor != "system" else "System promoted or refreshed long-term memory.",
            )
        )
        self._prune_agent_memories()
        return {"ok": memory is not None, "memory": memory, "actor_role": permission["actor_role"]}

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
        actor: str = "operator",
    ) -> dict[str, Any]:
        permission = self._require_actor_role(actor, "approver")
        if not permission.get("ok"):
            return permission
        before = self.assistant_db.get_long_term_memory(memory_id)
        updated = self.assistant_db.update_long_term_memory(
            memory_id,
            content=content,
            confidence=confidence,
            memory_type=memory_type,
            provenance=self._normalize_memory_provenance(provenance) if provenance is not None else None,
            status=status,
            is_pinned=is_pinned,
            expires_at=expires_at,
        )
        if updated is not None:
            self.assistant_db.log_memory_audit(
                MemoryAuditLogRecord(
                    memory_scope=str(updated.get("scope", "global")),
                    memory_key=str(updated.get("memory_key", "")),
                    memory_id=memory_id,
                    action_type="long_term_updated",
                    actor=actor,
                    actor_role=str(permission.get("actor_role", "")),
                    before_state=before or {},
                    after_state=updated,
                    reason="Manual update long-term memory.",
                )
            )
        return {"ok": updated is not None, "memory": updated, "memory_id": memory_id, "actor_role": permission["actor_role"]}

    def delete_long_term_memory(self, memory_id: int, actor: str = "operator") -> dict[str, Any]:
        permission = self._require_actor_role(actor, "admin")
        if not permission.get("ok"):
            return permission
        before = self.assistant_db.get_long_term_memory(memory_id)
        deleted = self.assistant_db.delete_long_term_memory(memory_id)
        if deleted:
            self.assistant_db.log_memory_audit(
                MemoryAuditLogRecord(
                    memory_scope=str((before or {}).get("scope", "global")),
                    memory_key=str((before or {}).get("memory_key", "")),
                    memory_id=memory_id,
                    action_type="long_term_deleted",
                    actor=actor,
                    actor_role=str(permission.get("actor_role", "")),
                    before_state=before or {},
                    after_state={},
                    reason="Manual delete long-term memory.",
                )
            )
        return {"ok": deleted, "memory_id": memory_id, "deleted": deleted, "actor_role": permission["actor_role"]}

    def build_agent_memory_context(self, session_id: str, question: str = "") -> dict[str, Any]:
        snapshot = self.get_agent_memory_snapshot(session_id, question=question)
        session_memory = snapshot.get("session_memory") or {}
        related_session_memories = snapshot.get("related_session_memories", [])
        long_term_memories = snapshot.get("long_term_memories", [])
        lines: list[str] = []
        if session_memory:
            summary = str(session_memory.get("summary", "")).strip()
            if summary:
                lines.append(f"Session summary: {summary}")
            key_facts = session_memory.get("key_facts", {})
            if isinstance(key_facts, dict) and key_facts:
                lines.append(f"Session facts: {json.dumps(key_facts, ensure_ascii=False, sort_keys=True)}")
        if related_session_memories:
            lines.append("Related session memories:")
            for item in related_session_memories:
                related_summary = str(item.get("summary", "")).strip()
                if related_summary:
                    lines.append(f"- {item.get('session_id', '')}: {related_summary}")
        if long_term_memories:
            lines.append("Long-term operator preferences:")
            for item in long_term_memories:
                lines.append(
                    f"- {item.get('memory_key', '')}: {item.get('content', '')} "
                    f"[type={item.get('memory_type', '')}, provenance={item.get('provenance', '')}]"
                )
        return {
            "ok": True,
            "session_id": session_id,
            "memory_prompt": "\n".join(lines).strip(),
            "snapshot": snapshot,
        }

    def remember_agent_interaction(
        self,
        session_id: str,
        question: str,
        answer: str,
        tool_trace: list[dict[str, Any]] | None = None,
    ) -> None:
        tool_trace = tool_trace or []
        summary = self._build_session_memory_summary(question, answer, tool_trace)
        key_facts = self._extract_memory_key_facts(question, answer, tool_trace)
        self.assistant_db.upsert_session_memory(
            SessionMemoryRecord(
                session_id=session_id,
                summary=summary,
                key_facts=key_facts,
                last_question=question.strip(),
                last_answer=self._truncate_memory_text(answer),
            )
        )
        for memory_key, content, confidence in self._extract_long_term_preferences(question):
            self.create_or_update_long_term_memory(
                memory_key=memory_key,
                content=content,
                scope="global",
                confidence=confidence,
                source_session_id=session_id,
                memory_type="preference",
                provenance="user_stated",
                status="active",
                actor="system",
            )
        candidates = self._extract_memory_candidates(session_id, question, answer, key_facts)
        if candidates:
            to_create: list[MemoryCandidateRecord] = []
            existing_memories = self.assistant_db.list_long_term_memories(scope="global", limit=100)
            for candidate in candidates:
                existing_candidate = self.assistant_db.get_pending_memory_candidate_by_key(candidate.scope, candidate.memory_key)
                if existing_candidate is not None:
                    merged = self.assistant_db.merge_memory_candidate(
                        str(existing_candidate.get("candidate_id", "")),
                        session_id=candidate.session_id,
                        content=candidate.content,
                        rationale=candidate.rationale,
                        confidence=candidate.confidence,
                        provenance=candidate.provenance,
                        risk_score=candidate.risk_score,
                        risk_flags=candidate.risk_flags or [],
                    )
                    if merged is not None:
                        self.assistant_db.log_memory_audit(
                            MemoryAuditLogRecord(
                                memory_scope=candidate.scope,
                                memory_key=candidate.memory_key,
                                candidate_id=str(existing_candidate.get("candidate_id", "")),
                                action_type="candidate_merged",
                                actor="system",
                                actor_role="admin",
                                before_state=existing_candidate,
                                after_state=merged,
                                reason="Merged repeated pending memory candidate by dedupe key.",
                            )
                        )
                    continue
                if any(
                    str(item.get("memory_key", "")) == candidate.memory_key and str(item.get("content", "")) == candidate.content
                    for item in existing_memories
                ):
                    continue
                to_create.append(candidate)
            if to_create:
                self.assistant_db.create_memory_candidates(to_create)
                for candidate in to_create:
                    self.assistant_db.log_memory_audit(
                        MemoryAuditLogRecord(
                            memory_scope=candidate.scope,
                            memory_key=candidate.memory_key,
                            candidate_id=candidate.candidate_id,
                            action_type="candidate_created",
                            actor="system",
                            actor_role="admin",
                            before_state={},
                            after_state={
                                "candidate_id": candidate.candidate_id,
                                "session_id": candidate.session_id,
                                "scope": candidate.scope,
                                "memory_key": candidate.memory_key,
                                "content": candidate.content,
                                "rationale": candidate.rationale,
                                "confidence": candidate.confidence,
                                "memory_type": candidate.memory_type,
                                "provenance": candidate.provenance,
                                "risk_score": candidate.risk_score,
                                "risk_flags": candidate.risk_flags or [],
                                "status": candidate.status,
                            },
                            reason="Auto-generated memory candidate from user interaction.",
                        )
                    )
        self._prune_agent_memories()

    def route_hybrid_sql(self, question: str) -> dict[str, Any]:
        normalized = question.lower()
        if "传感器上报减少" in question or ("传感器" in question and "减少" in question):
            summary = query_sensor_event_activity_summary(self.history_db).to_dict()
            latest = query_latest_sensor_event(self.history_db).to_dict()
            return {
                "summary_type": "sensor_activity",
                "activity_summary": summary,
                "latest_sensor_event": latest,
            }
        if "ota" in normalized and ("失败" in question or "失败率" in question):
            summary = query_ota_failure_summary(self.ota_db).to_dict()
            reasons = query_ota_failure_reason_count(self.ota_db).to_dict()
            recent_failed = query_ota_failed_tasks(self.ota_db, limit=10).to_dict()
            return {
                "summary_type": "ota_failure",
                "failure_summary": summary,
                "failure_reasons": reasons,
                "recent_failed_tasks": recent_failed,
            }
        result = self.route_sql(question)
        return {
            "summary_type": "generic",
            "sql_result": result.to_dict(),
        }

    def _build_rag_prompt(self, question: str, retrieved_chunks: list[dict[str, Any]]) -> str:
        chunks_text = "\n\n".join(
            f"[引用] {item.get('citation', item['source_path'])}\n"
            f"[来源] {item['source_path']}\n"
            f"[章节] {item.get('section_title', '')}\n"
            f"[分数] {item['score']}\n"
            f"[片段]\n{item['chunk_text']}"
            for item in retrieved_chunks
        )
        return (
            "你是边缘网关文档问答助手。\n"
            "只允许依据给定片段回答，不允许根据示例自行推断未出现的字段定义。\n"
            "如果片段不能直接支持答案，必须明确回答“当前知识库未找到直接依据”。\n"
            "回答时先给结论，再给出处依据。\n"
            "引用时优先使用我提供的 [引用] 字段，格式尽量写成“依据：xxx”。\n\n"
            f"用户问题：{question}\n\n"
            f"检索片段：\n{chunks_text or '无'}"
        )

    def _build_hybrid_prompt(
        self,
        question: str,
        sql_result: dict[str, Any] | None,
        analysis_facts: list[str],
        retrieved_chunks: list[dict[str, Any]],
    ) -> str:
        chunks_text = "\n\n".join(
            f"[引用] {item.get('citation', item['source_path'])}\n"
            f"[来源] {item['source_path']}\n"
            f"[章节] {item.get('section_title', '')}\n"
            f"[分数] {item['score']}\n"
            f"[片段]\n{item['chunk_text']}"
            for item in retrieved_chunks
        )
        facts_text = "\n".join(f"- {item}" for item in analysis_facts) or "- 当前没有可提炼的事实摘要。"
        return (
            "你是边缘网关混合分析助手。\n"
            "先总结 SQL 结果中可确认的事实，再结合文档片段给出可能原因。\n"
            "必须区分“事实”和“推断”；如果文档依据不足，要明确说明。\n"
            "不要把示例命令、示例帧误写成协议正式定义。\n\n"
            "引用文档依据时优先使用我提供的 [引用] 字段。\n\n"
            f"用户问题：{question}\n\n"
            f"事实摘要：\n{facts_text}\n\n"
            f"SQL 结果：{sql_result}\n\n"
            f"检索片段：\n{chunks_text or '无'}"
        )

    def _guide_facts(self) -> list[str]:
        return [
            "可查询设备运行状态，例如 DEVICE_001 当前状态正常吗。",
            "可查询传感器历史数据，例如 查询 sensor-01 最近 5 条传感器历史数据。",
            "可查询系统事件，例如 查询最近 10 条系统事件。",
            "可查询 OTA 任务状态，例如 查询 task_id=ota-001 的状态。",
            "可回答协议、流程、接口类文档问题，例如 AA55 协议帧格式是什么。",
            "可结合数据和文档做混合分析，例如 最近 OTA 失败任务变多，结合文档分析可能原因。",
            "运行态问题优先走 HTTP API，失败时回退 SQLite。",
            "如果设备状态是根据最近事件推断出来的，会明确标注 inferred_from_recent_events。",
        ]

    def _extract_limit(self, question: str) -> int:
        match = re.search(r"limit\s*=\s*(\d+)", question, flags=re.IGNORECASE)
        if not match:
            match = re.search(r"最近\s*(\d+)\s*条", question)
        if not match:
            match = re.search(r"(\d+)\s*条", question)
        if not match:
            return 10
        return max(1, min(int(match.group(1)), 100))

    def _extract_device_id(self, question: str) -> int | None:
        match = re.search(r"device_id\s*=\s*(\d+)", question, flags=re.IGNORECASE)
        if not match:
            match = re.search(r"device\s+(\d+)", question, flags=re.IGNORECASE)
        if not match:
            return None
        return int(match.group(1))

    def _extract_device_ref(self, question: str) -> str | None:
        match = re.search(r"\b((?:sensor|device|node|wifi-node)[-_]?\d+)\b", question, flags=re.IGNORECASE)
        if match:
            return match.group(1)
        match = re.search(r"device_id\s*=\s*(\d+)", question, flags=re.IGNORECASE)
        if match:
            return match.group(1)
        match = re.search(r"(?:设备|device|节点)\s*([0-9]+)", question, flags=re.IGNORECASE)
        if match:
            return match.group(1)
        return None

    def _extract_device_name(self, question: str) -> str | None:
        match = re.search(r"\b(sensor-\d+)\b", question, flags=re.IGNORECASE)
        if not match:
            return None
        return match.group(1)

    def _extract_task_id(self, question: str) -> str | None:
        patterns = (
            r"task_id\s*=\s*([A-Za-z0-9._:-]+)",
            r"task\s+([A-Za-z0-9._:-]+)",
            r"任务\s*([A-Za-z0-9._:-]+)",
        )
        for pattern in patterns:
            match = re.search(pattern, question, flags=re.IGNORECASE)
            if match:
                return match.group(1)
        return None

    def _summarize_query_result(self, result: QueryResult) -> list[str]:
        if result.note:
            return [result.note]
        if not result.rows:
            return ["查询结果为空。"]
        columns = result.columns
        rows = result.rows
        facts: list[str] = [f"返回 {len(rows)} 行结果。"]
        if columns == ["id", "ts_unix_ms", "event_type", "device_id", "detail"]:
            latest = rows[0]
            facts.append(
                f"最新系统事件是 {latest[2]}，设备 {latest[3]}，时间 {self._format_ts_ms(latest[1])}，详情 {latest[4]}。"
            )
        elif "sensor_event_count" in columns:
            facts.append(f"窗口内传感器事件数量为 {rows[0][0]}。")
        elif "state" in columns and "task_count" in columns:
            top = rows[0]
            facts.append(f"OTA 任务最多的状态是 {top[0]}，数量 {top[1]}。")
        elif columns[:2] == ["task_uuid", "device_id"] and "last_error" in columns:
            facts.append(f"共返回 {len(rows)} 条 OTA 任务记录。")
            failed = [row for row in rows if len(row) >= 6 and row[5]]
            if failed:
                facts.append(f"样例失败原因为 {failed[0][5]}。")
        elif "device_name" in columns and "temperature" in columns:
            latest = rows[0]
            facts.append(
                f"最新传感器记录来自 {latest[3]}，时间 {self._format_ts_ms(latest[1])}，温度 {latest[5]}，湿度 {latest[6]}，光照 {latest[7]}。"
            )
        elif "state" in columns and "detail" in columns:
            latest = rows[0]
            facts.append(f"最新 OTA 事件状态为 {latest[3]}，时间 {self._format_ts_ms(latest[2])}，详情 {latest[4]}。")
        return facts

    def _summarize_hybrid_result(self, sql_result: dict[str, Any]) -> list[str]:
        summary_type = sql_result.get("summary_type")
        facts: list[str] = []
        if summary_type == "sensor_activity":
            rows = sql_result.get("activity_summary", {}).get("rows", [])
            if rows:
                recent_count, previous_count, recent_device_count, latest_ts = rows[0]
                facts.append(f"最近一个分析窗口内传感器事件数为 {recent_count}。")
                facts.append(f"前一个同长度窗口内传感器事件数为 {previous_count}。")
                delta = recent_count - previous_count
                facts.append(f"两个窗口相比事件数量变化为 {delta}。")
                facts.append(f"最近窗口涉及设备数为 {recent_device_count}。")
                facts.append(f"表内最新传感器时间为 {self._format_ts_ms(latest_ts)}。")
            latest_rows = sql_result.get("latest_sensor_event", {}).get("rows", [])
            if latest_rows:
                row = latest_rows[0]
                facts.append(f"最新传感器记录设备为 {row[3]}，温度 {row[5]}，湿度 {row[6]}，光照 {row[7]}。")
            return facts or ["当前没有可用的传感器活动摘要。"]
        if summary_type == "ota_failure":
            summary_rows = sql_result.get("failure_summary", {}).get("rows", [])
            if summary_rows:
                failed_total, failed_last_10_tasks, sample_last_error = summary_rows[0]
                facts.append(f"OTA 失败任务总数为 {failed_total}。")
                facts.append(f"最近 10 个任务里失败数为 {failed_last_10_tasks}。")
                if sample_last_error:
                    facts.append(f"失败样例错误原因为 {sample_last_error}。")
            reason_rows = sql_result.get("failure_reasons", {}).get("rows", [])
            if reason_rows:
                top_reason, top_count = reason_rows[0]
                facts.append(f"最常见失败原因是 {top_reason}，出现 {top_count} 次。")
            task_rows = sql_result.get("recent_failed_tasks", {}).get("rows", [])
            if task_rows:
                facts.append(f"最近失败任务返回了 {len(task_rows)} 条样例。")
            return facts or ["当前没有可用的 OTA 失败摘要。"]
        generic = sql_result.get("sql_result")
        if generic:
            result = QueryResult(
                sql=generic.get("sql", ""),
                columns=generic.get("columns", []),
                rows=generic.get("rows", []),
                note=generic.get("note", ""),
            )
            return self._summarize_query_result(result)
        return ["当前没有可提炼的混合分析事实摘要。"]

    def _summarize_runtime_result(self, runtime_result: dict[str, Any]) -> list[str]:
        if not runtime_result:
            return ["运行态查询没有返回结果。"]
        if not runtime_result.get("ok", False):
            return [runtime_result.get("message", "运行态查询失败。")]

        facts: list[str] = []
        if "status" in runtime_result:
            facts.append(f"设备当前状态为 {runtime_result.get('status')}。")
            if runtime_result.get("status_source"):
                facts.append(f"状态来源为 {runtime_result['status_source']}。")
            if runtime_result.get("last_seen_readable_time"):
                facts.append(f"最近一次设备活动时间为 {runtime_result['last_seen_readable_time']}。")
            if runtime_result.get("ota_status"):
                facts.append(f"该设备最近一次 OTA 状态为 {runtime_result['ota_status']}。")
        elif "records" in runtime_result:
            facts.append(f"共返回 {runtime_result.get('count', 0)} 条记录。")
            records = runtime_result.get("records") or []
            if records:
                latest = records[0]
                if "event_type" in latest:
                    facts.append(
                        f"最新系统事件为 {latest.get('event_type')}，时间 {self._format_ts_ms(latest.get('ts_unix_ms'))}。"
                    )
                else:
                    facts.append(
                        f"最新传感器记录时间 {self._format_ts_ms(latest.get('ts_unix_ms'))}，温度 {latest.get('temperature')}，湿度 {latest.get('humidity')}。"
                    )
        elif "task" in runtime_result:
            task = runtime_result["task"]
            facts.append(f"OTA 任务状态为 {task.get('state', 'unknown')}。")
            if task.get("last_error"):
                facts.append(f"最近错误为 {task['last_error']}。")
            facts.append(f"共返回 {runtime_result.get('total_events', 0)} 条 OTA 事件。")
        elif "summary" in runtime_result:
            summary = runtime_result["summary"]
            if "sensor_events" in summary:
                facts.append(f"传感器历史总量为 {summary['sensor_events'].get('count', 0)}。")
            if "ota_tasks" in summary:
                facts.append(f"OTA 任务总量为 {summary['ota_tasks'].get('count', 0)}。")
        else:
            facts.append(runtime_result.get("message", "运行态查询成功。"))
        return facts

    def _normalize_batch_item_status(self, raw_state: str, fallback: Any = "unknown") -> str:
        state = str(raw_state or "").strip().lower()
        if not state:
            return str(fallback)
        mapping = {
            "created": "task_created",
            "pending": "task_created",
            "queued": "task_created",
            "running": "running",
            "sending": "running",
            "in_progress": "running",
            "success": "success",
            "succeeded": "success",
            "done": "success",
            "failed": "failed",
            "error": "failed",
            "submit_failed": "submit_failed",
            "cancelled": "cancelled",
            "canceled": "cancelled",
            "task_created": "task_created",
        }
        return mapping.get(state, state)

    def _summarize_batch_run_items_dicts(
        self,
        items: list[dict[str, Any]],
        blocked_device_ids: list[Any] | None = None,
    ) -> dict[str, Any]:
        blocked = [int(item) for item in (blocked_device_ids or [])]
        status_counts: dict[str, int] = {}
        batch_counts: dict[str, int] = {}
        batch_statuses: dict[int, dict[str, Any]] = {}
        failed_device_ids: list[int] = []
        for item in items:
            status = str(item.get("status", "unknown"))
            status_counts[status] = status_counts.get(status, 0) + 1
            batch_index = int(item.get("batch_index", 0) or 0)
            batch_key = str(batch_index)
            batch_counts[batch_key] = batch_counts.get(batch_key, 0) + 1
            group = batch_statuses.setdefault(batch_index, {"batch_index": batch_index, "status_counts": {}, "device_ids": []})
            group["status_counts"][status] = group["status_counts"].get(status, 0) + 1
            group["device_ids"].append(int(item.get("device_id", 0)))
            if status in {"failed", "submit_failed", "cancelled"}:
                failed_device_ids.append(int(item.get("device_id", 0)))
        batches = []
        for batch_index in sorted(batch_statuses):
            group = batch_statuses[batch_index]
            batches.append(
                {
                    "batch_index": batch_index,
                    "device_count": len(group["device_ids"]),
                    "device_ids": group["device_ids"],
                    "status_counts": group["status_counts"],
                }
            )
        return {
            "device_count": len(items),
            "blocked_device_ids": blocked,
            "blocked_count": len(blocked),
            "status_counts": status_counts,
            "batch_counts": batch_counts,
            "batches": batches,
            "failed_device_ids": failed_device_ids,
            "success_count": status_counts.get("success", 0),
            "running_count": status_counts.get("running", 0),
            "task_created_count": status_counts.get("task_created", 0),
            "queued_count": status_counts.get("queued", 0),
            "waiting_batch_count": status_counts.get("waiting_batch", 0),
            "failed_count": (
                status_counts.get("failed", 0)
                + status_counts.get("submit_failed", 0)
                + status_counts.get("cancelled", 0)
            ),
        }

    def _derive_batch_run_status(self, summary: dict[str, Any]) -> str:
        status_counts = summary.get("status_counts", {})
        device_count = int(summary.get("device_count", 0))
        if device_count == 0:
            return "empty"
        if status_counts.get("waiting_batch", 0) > 0 and (
            status_counts.get("running", 0) > 0 or status_counts.get("task_created", 0) > 0
        ):
            return "running"
        if int(summary.get("failed_count", 0)) > 0:
            if (
                int(summary.get("success_count", 0))
                + int(summary.get("running_count", 0))
                + int(summary.get("task_created_count", 0))
            ) > 0:
                return "partial_failed"
            return "failed"
        if status_counts.get("running", 0) > 0:
            return "running"
        if status_counts.get("task_created", 0) > 0:
            return "created"
        if status_counts.get("queued", 0) > 0 or status_counts.get("waiting_batch", 0) > 0:
            return "waiting_next_batch"
        if status_counts.get("success", 0) == device_count:
            return "success"
        return "approved"

    def _compose_batch_run_status(self, run: dict[str, Any], summary: dict[str, Any]) -> str:
        stored_status = str(run.get("status", ""))
        control = summary.get("control", {})
        if control.get("terminated") or stored_status == "terminated":
            return "terminated"
        if control.get("paused") or stored_status == "paused":
            return "paused"
        return self._derive_batch_run_status(summary)

    def _next_batch_index(self, items: list[dict[str, Any]]) -> int | None:
        candidate_indexes = sorted(
            {
                int(item.get("batch_index", 0))
                for item in items
                if str(item.get("status", "")) in {"queued", "waiting_batch"}
            }
        )
        return candidate_indexes[0] if candidate_indexes else None

    def _pending_batch_indexes(self, items: list[dict[str, Any]]) -> list[int]:
        return sorted(
            {
                int(item.get("batch_index", 0))
                for item in items
                if str(item.get("status", "")) in {"queued", "waiting_batch"}
            }
        )

    def _validate_previous_batches_before_start(self, items: list[dict[str, Any]], next_batch_index: int) -> str:
        previous_items = [item for item in items if int(item.get("batch_index", 0)) < next_batch_index]
        if any(str(item.get("status", "")) in {"task_created", "running"} for item in previous_items):
            return "Previous OTA batch is still running. Refresh the batch run before starting the next one."
        if any(str(item.get("status", "")) in {"submit_failed", "failed", "cancelled"} for item in previous_items):
            return "Previous OTA batch contains failed devices. Retry or resolve them before starting the next batch."
        return ""

    def _sync_approval_from_batch_run(
        self,
        run: dict[str, Any],
        summary: dict[str, Any],
        operator: str,
        decision_note: str,
    ) -> None:
        approval_id = str(run.get("approval_id", ""))
        if not approval_id:
            return
        approval = self.assistant_db.get_approval_request(approval_id)
        if approval is None:
            return
        previous_payload = approval.get("result_payload", {})
        current_status = self._compose_batch_run_status(run, summary)
        updated_payload = dict(previous_payload)
        updated_payload["status"] = current_status
        updated_payload["created_count"] = int(summary.get("task_created_count", 0)) + int(summary.get("running_count", 0)) + int(summary.get("success_count", 0))
        updated_payload["failed_count"] = int(summary.get("failed_count", 0))
        updated_payload["failed_device_ids"] = summary.get("failed_device_ids", [])
        updated_payload["pending_batch_indexes"] = self._pending_batch_indexes(self.assistant_db.list_ota_batch_run_items(str(run.get("batch_run_id", ""))))
        updated_payload["current_batch_index"] = max(
            [int(item.get("batch_index", 0)) for item in self.assistant_db.list_ota_batch_run_items(str(run.get("batch_run_id", ""))) if str(item.get("status", "")) not in {"waiting_batch", "queued"}]
            or [0]
        )
        updated_payload["sla"] = self._build_batch_run_sla(run, self.assistant_db.list_ota_batch_run_items(str(run.get("batch_run_id", ""))))
        self.assistant_db.update_approval_request_status(
            approval_id=approval_id,
            status=current_status,
            approved_by=operator,
            approved_at=datetime.now(timezone.utc).isoformat(),
            decision_note=decision_note or str(approval.get("decision_note", "")),
            result_payload=updated_payload,
        )

    def _build_batch_run_sla(self, run: dict[str, Any], items: list[dict[str, Any]]) -> dict[str, Any]:
        now = datetime.now(timezone.utc)
        created_at = self._parse_iso(run.get("created_at", ""))
        updated_at = self._parse_iso(run.get("updated_at", ""))
        batches: dict[int, list[dict[str, Any]]] = {}
        for item in items:
            batches.setdefault(int(item.get("batch_index", 0) or 0), []).append(item)
        batch_metrics = []
        for batch_index in sorted(batches):
            batch_items = batches[batch_index]
            first_created = min((self._parse_iso(item.get("created_at", "")) for item in batch_items), default=None)
            last_updated = max((self._parse_iso(item.get("updated_at", "")) for item in batch_items), default=None)
            completed = [
                item for item in batch_items if str(item.get("status", "")) in {"success", "failed", "cancelled"}
            ]
            submitted = [
                item for item in batch_items if str(item.get("status", "")) not in {"queued", "waiting_batch"}
            ]
            batch_metrics.append(
                {
                    "batch_index": batch_index,
                    "device_count": len(batch_items),
                    "submitted_count": len(submitted),
                    "completed_count": len(completed),
                    "unfinished_count": len(batch_items) - len(completed),
                    "max_retry_count": max((int(item.get("retry_count", 0)) for item in batch_items), default=0),
                    "elapsed_sec": self._elapsed_seconds(first_created, now),
                    "since_last_update_sec": self._elapsed_seconds(last_updated, now),
                }
            )
        completed_items = [
            item for item in items if str(item.get("status", "")) in {"success", "failed", "cancelled"}
        ]
        return {
            "run_age_sec": self._elapsed_seconds(created_at, now),
            "since_last_update_sec": self._elapsed_seconds(updated_at, now),
            "completion_ratio": round(len(completed_items) / len(items), 4) if items else 0.0,
            "pending_batch_count": len(self._pending_batch_indexes(items)),
            "active_batch_indexes": sorted({int(item.get("batch_index", 0)) for item in items if str(item.get("status", "")) in {"task_created", "running"}}),
            "batch_metrics": batch_metrics,
        }

    def _parse_iso(self, value: Any) -> datetime | None:
        text = str(value or "").strip()
        if not text:
            return None
        try:
            return datetime.fromisoformat(text)
        except ValueError:
            return None

    def _elapsed_seconds(self, start: datetime | None, end: datetime) -> int | None:
        if start is None:
            return None
        return max(0, int((end - start).total_seconds()))

    def _runtime_source_label(self, runtime_result: dict[str, Any] | None) -> str:
        if not runtime_result:
            return "runtime"
        source = runtime_result.get("source")
        status_source = runtime_result.get("status_source")
        if source == "http":
            return "runtime:http"
        if source == "sqlite" and status_source == "inferred_from_recent_events":
            return "runtime:sqlite_inferred"
        if source == "sqlite":
            return "runtime:sqlite"
        return "runtime"

    def _new_session_id(self) -> str:
        return f"chat-{uuid4().hex[:12]}"

    def _build_session_memory_summary(
        self,
        question: str,
        answer: str,
        tool_trace: list[dict[str, Any]],
    ) -> str:
        tools = [
            str(item.get("tool") or item.get("tool_name") or "").strip()
            for item in tool_trace
            if str(item.get("tool") or item.get("tool_name") or "").strip()
        ]
        tool_text = ", ".join(tools[:5]) if tools else "none"
        question_text = question.strip().replace("\n", " ")
        answer_text = self._truncate_memory_text(answer)
        return (
            f"Latest request: {question_text}. "
            f"Tools used: {tool_text}. "
            f"Latest answer summary: {answer_text}"
        ).strip()

    def _truncate_memory_text(self, text: str) -> str:
        normalized = " ".join(str(text or "").split())
        limit = max(80, int(self.settings.agent_memory_answer_char_limit))
        if len(normalized) <= limit:
            return normalized
        return normalized[: limit - 3].rstrip() + "..."

    def _extract_memory_key_facts(
        self,
        question: str,
        answer: str,
        tool_trace: list[dict[str, Any]],
    ) -> dict[str, Any]:
        facts: dict[str, Any] = {}
        device_id = self._extract_device_id(question)
        if device_id is not None:
            facts["device_id"] = device_id
        device_ref = self._extract_device_ref(question)
        if device_ref:
            facts["device_ref"] = device_ref
        task_id = self._extract_task_id(question)
        if task_id:
            facts["task_id"] = task_id
        approval_match = re.search(r"approval[_-]?id\s*=\s*([A-Za-z0-9._:-]+)", question, flags=re.IGNORECASE)
        if approval_match:
            facts["approval_id"] = approval_match.group(1)
        batch_match = re.search(r"batch[_-]?run[_-]?id\s*=\s*([A-Za-z0-9._:-]+)", question, flags=re.IGNORECASE)
        if batch_match:
            facts["batch_run_id"] = batch_match.group(1)
        firmware_match = re.search(r"firmware[_-]?id\s*=\s*([A-Za-z0-9._:-]+)", question, flags=re.IGNORECASE)
        if firmware_match:
            facts["firmware_id"] = firmware_match.group(1)
        tools = [
            str(item.get("tool") or item.get("tool_name") or "").strip()
            for item in tool_trace
            if str(item.get("tool") or item.get("tool_name") or "").strip()
        ]
        if tools:
            facts["tools_used"] = tools[:8]
        if answer.strip():
            facts["last_answer_excerpt"] = self._truncate_memory_text(answer)
        return facts

    def _extract_long_term_preferences(self, question: str) -> list[tuple[str, str, float]]:
        normalized = question.strip().lower()
        memories: list[tuple[str, str, float]] = []
        if ("默认" in question or "以后" in question or "优先" in question) and "tcp_binary" in normalized:
            memories.append(("preferred_transport", "Prefer tcp_binary transport for future OTA operations.", 0.9))
        elif ("默认" in question or "以后" in question or "优先" in question) and "serial" in normalized:
            memories.append(("preferred_transport", "Prefer serial transport for future OTA operations.", 0.9))
        if "简洁" in question:
            memories.append(("answer_style", "Prefer concise answers.", 0.8))
        elif "详细" in question:
            memories.append(("answer_style", "Prefer detailed answers with more explanation.", 0.8))
        if "英文" in question:
            memories.append(("response_language", "Prefer English responses when possible.", 0.7))
        elif "中文" in question:
            memories.append(("response_language", "Prefer Chinese responses when possible.", 0.7))
        return memories

    def _extract_memory_candidates(
        self,
        session_id: str,
        question: str,
        answer: str,
        key_facts: dict[str, Any],
    ) -> list[MemoryCandidateRecord]:
        if not any(cue in question for cue in ("记住", "记一下", "记下来", "后续", "以后")):
            return []
        candidates: list[MemoryCandidateRecord] = []
        if key_facts.get("device_id") is not None:
            device_id = key_facts["device_id"]
            risk_score, risk_flags = self._score_memory_candidate_risk(
                f"focus_device_{device_id}",
                0.7,
                "fact",
                "user_stated",
                "Question includes memory cue and an explicit device_id.",
            )
            candidates.append(
                MemoryCandidateRecord(
                    candidate_id=f"memcand-{uuid4().hex[:12]}",
                    session_id=session_id,
                    scope="global",
                    memory_key=f"focus_device_{device_id}",
                    content=f"Operator is actively focused on device_id={device_id} for follow-up investigation.",
                    rationale=self._build_memory_candidate_rationale(
                        "Question includes memory cue and an explicit device_id.",
                        question,
                    ),
                    confidence=0.7,
                    memory_type="fact",
                    provenance="user_stated",
                    risk_score=risk_score,
                    risk_flags=risk_flags,
                )
            )
        if key_facts.get("approval_id"):
            approval_id = key_facts["approval_id"]
            risk_score, risk_flags = self._score_memory_candidate_risk(
                f"tracked_approval_{approval_id}",
                0.75,
                "fact",
                "user_stated",
                "Question includes memory cue and an explicit approval_id.",
            )
            candidates.append(
                MemoryCandidateRecord(
                    candidate_id=f"memcand-{uuid4().hex[:12]}",
                    session_id=session_id,
                    scope="global",
                    memory_key=f"tracked_approval_{approval_id}",
                    content=f"Track approval_id={approval_id} as an important pending governance item.",
                    rationale=self._build_memory_candidate_rationale(
                        "Question includes memory cue and an explicit approval_id.",
                        question,
                    ),
                    confidence=0.75,
                    memory_type="fact",
                    provenance="user_stated",
                    risk_score=risk_score,
                    risk_flags=risk_flags,
                )
            )
        if key_facts.get("batch_run_id"):
            batch_run_id = key_facts["batch_run_id"]
            risk_score, risk_flags = self._score_memory_candidate_risk(
                f"tracked_batch_run_{batch_run_id}",
                0.75,
                "fact",
                "user_stated",
                "Question includes memory cue and an explicit batch_run_id.",
            )
            candidates.append(
                MemoryCandidateRecord(
                    candidate_id=f"memcand-{uuid4().hex[:12]}",
                    session_id=session_id,
                    scope="global",
                    memory_key=f"tracked_batch_run_{batch_run_id}",
                    content=f"Track batch_run_id={batch_run_id} as an important rollout to revisit later.",
                    rationale=self._build_memory_candidate_rationale(
                        "Question includes memory cue and an explicit batch_run_id.",
                        question,
                    ),
                    confidence=0.75,
                    memory_type="fact",
                    provenance="user_stated",
                    risk_score=risk_score,
                    risk_flags=risk_flags,
                )
            )
        if key_facts.get("task_id"):
            task_id = key_facts["task_id"]
            risk_score, risk_flags = self._score_memory_candidate_risk(
                f"tracked_task_{task_id}",
                0.7,
                "fact",
                "user_stated",
                "Question includes memory cue and an explicit task_id.",
            )
            candidates.append(
                MemoryCandidateRecord(
                    candidate_id=f"memcand-{uuid4().hex[:12]}",
                    session_id=session_id,
                    scope="global",
                    memory_key=f"tracked_task_{task_id}",
                    content=f"Track task_id={task_id} as an important OTA/runtime task for follow-up.",
                    rationale=self._build_memory_candidate_rationale(
                        "Question includes memory cue and an explicit task_id.",
                        question,
                    ),
                    confidence=0.7,
                    memory_type="fact",
                    provenance="user_stated",
                    risk_score=risk_score,
                    risk_flags=risk_flags,
                )
            )
        if not candidates and answer.strip():
            risk_score, risk_flags = self._score_memory_candidate_risk(
                f"session_note_{session_id}",
                0.55,
                "note",
                "user_stated",
                "Question asked the system to remember something, but only a generic answer summary was available.",
            )
            candidates.append(
                MemoryCandidateRecord(
                    candidate_id=f"memcand-{uuid4().hex[:12]}",
                    session_id=session_id,
                    scope="global",
                    memory_key=f"session_note_{session_id}",
                    content=self._truncate_memory_text(answer),
                    rationale=self._build_memory_candidate_rationale(
                        "Question asked the system to remember something, but only a generic answer summary was available.",
                        question,
                    ),
                    confidence=0.55,
                    memory_type="note",
                    provenance="user_stated",
                    risk_score=risk_score,
                    risk_flags=risk_flags,
                )
            )
        deduped: list[MemoryCandidateRecord] = []
        seen_keys: set[tuple[str, str]] = set()
        for item in candidates:
            dedupe_key = (item.scope, item.memory_key)
            if dedupe_key in seen_keys:
                continue
            seen_keys.add(dedupe_key)
            deduped.append(item)
        return deduped

    def _build_memory_candidate_rationale(self, base: str, question: str) -> str:
        normalized_question = " ".join(str(question or "").split())
        question_excerpt = normalized_question[:117].rstrip()
        if len(normalized_question) > 117:
            question_excerpt += "..."
        if not question_excerpt:
            return base
        return f"{base} User memory cue: {question_excerpt}"

    def _score_memory_candidate_risk(
        self,
        memory_key: str,
        confidence: float,
        memory_type: str,
        provenance: str,
        rationale: str,
    ) -> tuple[float, list[str]]:
        score = 0.0
        flags: list[str] = []
        normalized_key = str(memory_key or "").strip().lower()
        normalized_type = str(memory_type or "").strip().lower()
        normalized_provenance = str(provenance or "").strip().lower()
        normalized_rationale = str(rationale or "").strip().lower()
        if normalized_provenance == "inferred":
            score += 0.45
            flags.append("inferred_source")
        if normalized_type == "note":
            score += 0.2
            flags.append("note_memory")
        if float(confidence) < 0.7:
            score += 0.2
            flags.append("low_confidence")
        if normalized_key.startswith("session_note_"):
            score += 0.15
            flags.append("generic_session_note")
        if "generic answer summary" in normalized_rationale:
            score += 0.15
            flags.append("summary_only")
        return (min(1.0, round(score, 2)), flags)

    def _memory_candidate_requires_second_review(self, candidate: dict[str, Any]) -> bool:
        return float(candidate.get("risk_score", 0.0)) >= 0.4

    def _collect_user_stated_facts(self, question: str) -> list[str]:
        facts: list[str] = []
        key_facts = self._extract_memory_key_facts(question, "", [])
        for key in ("device_id", "device_ref", "task_id", "approval_id", "batch_run_id", "firmware_id"):
            value = key_facts.get(key)
            if value not in (None, ""):
                facts.append(f"{key}={value}")
        if not facts and question.strip():
            facts.append(self._truncate_memory_text(question))
        return facts[:4]

    def _collect_confirmed_facts(
        self,
        analysis_facts: list[str],
        runtime_result: dict[str, Any] | None,
        sql_result: dict[str, Any] | None,
        tool_trace: list[dict[str, Any]] | None,
    ) -> list[str]:
        confirmed: list[str] = []
        for item in analysis_facts[:3]:
            if item not in confirmed:
                confirmed.append(item)
        for item in tool_trace or []:
            tool_name = str(item.get("tool", "")).strip()
            if tool_name:
                confirmed.append(f"Executed tool {tool_name}.")
        if runtime_result and runtime_result.get("status_source") and runtime_result.get("status_source") != "inferred_from_recent_events":
            confirmed.append(f"Runtime source: {runtime_result.get('status_source')}.")
        if sql_result and sql_result.get("sql"):
            confirmed.append("Answer included direct SQL-backed facts.")
        return confirmed[:4]

    def _collect_inferred_facts(
        self,
        analysis_facts: list[str],
        runtime_result: dict[str, Any] | None,
        answer: str,
    ) -> list[str]:
        inferred: list[str] = []
        if runtime_result and runtime_result.get("status_source") == "inferred_from_recent_events":
            inferred.append("Runtime status was inferred from recent events.")
        if any(token in str(answer) for token in ("可能", "推断", "大概率", "估计")):
            inferred.append("Answer contains model-level inference language.")
        if not inferred:
            for item in analysis_facts:
                if "变化" in item or "样例" in item:
                    inferred.append(item)
                    break
        return inferred[:3]

    def _format_answer_with_evidence(
        self,
        answer: str,
        *,
        confirmed: list[str],
        inferred: list[str],
        user_stated: list[str],
    ) -> str:
        base_answer = str(answer or "").strip()
        sections = [base_answer] if base_answer else []
        sections.append("Confirmed:")
        sections.extend(f"- {item}" for item in (confirmed or ["None."]))
        sections.append("Inferred:")
        sections.extend(f"- {item}" for item in (inferred or ["None."]))
        sections.append("User-stated:")
        sections.extend(f"- {item}" for item in (user_stated or ["None."]))
        return "\n".join(sections).strip()

    def _retrieve_related_session_memories(self, session_id: str, question: str, limit: int | None = None) -> list[dict[str, Any]]:
        normalized_limit = max(1, limit or self.settings.agent_memory_related_session_limit)
        current_facts = self._extract_memory_key_facts(question, "", [])
        current_tokens = self._memory_query_tokens(question)
        candidates = self.assistant_db.list_session_memories(limit=self.settings.agent_memory_session_store_limit)
        scored: list[tuple[int, dict[str, Any]]] = []
        for item in candidates:
            candidate_session_id = str(item.get("session_id", "")).strip()
            if not candidate_session_id or candidate_session_id == session_id:
                continue
            score = 0
            key_facts = item.get("key_facts", {})
            if not isinstance(key_facts, dict):
                key_facts = {}
            for fact_key in ("device_id", "device_ref", "task_id", "approval_id", "batch_run_id", "firmware_id"):
                current_value = current_facts.get(fact_key)
                candidate_value = key_facts.get(fact_key)
                if current_value and candidate_value and str(current_value) == str(candidate_value):
                    score += 5
            haystack = " ".join(
                [
                    str(item.get("summary", "")),
                    str(item.get("last_question", "")),
                    str(item.get("last_answer", "")),
                ]
            ).lower()
            token_hits = sum(1 for token in current_tokens if token in haystack)
            score += token_hits
            if score > 0:
                enriched = dict(item)
                enriched["relevance_score"] = score
                scored.append((score, enriched))
        scored.sort(key=lambda pair: (pair[0], str(pair[1].get("updated_at", ""))), reverse=True)
        return [item for _, item in scored[:normalized_limit]]

    def _filter_active_long_term_memories(self, memories: list[dict[str, Any]]) -> list[dict[str, Any]]:
        now_iso = datetime.now(timezone.utc).isoformat()
        active_items: list[dict[str, Any]] = []
        for item in memories:
            status = str(item.get("status", "active")).strip().lower()
            expires_at = str(item.get("expires_at", "")).strip()
            if status not in {"active", ""}:
                continue
            if expires_at and expires_at <= now_iso:
                continue
            active_items.append(item)
        return active_items

    def _memory_query_tokens(self, question: str) -> list[str]:
        tokens = {
            token.lower()
            for token in re.findall(r"[A-Za-z0-9._:-]+", question)
            if len(token.strip()) >= 3
        }
        return sorted(tokens)

    def _resolve_actor_role(self, actor: str) -> str:
        normalized = str(actor or "").strip().lower()
        if normalized in {"system", "service"}:
            return "admin"
        if normalized in {item.lower() for item in self.settings.agent_admin_users}:
            return "admin"
        if normalized in {item.lower() for item in self.settings.agent_approver_users}:
            return "approver"
        if normalized in {item.lower() for item in self.settings.agent_operator_users}:
            return "operator"
        if normalized in {item.lower() for item in self.settings.agent_viewer_users}:
            return "viewer"
        if normalized == "admin":
            return "admin"
        if normalized in {"approver", "reviewer"}:
            return "approver"
        if normalized == "operator":
            return "operator"
        if normalized == "viewer":
            return "viewer"
        return "viewer"

    def _normalize_memory_provenance(self, provenance: str) -> str:
        normalized = str(provenance or "").strip().lower().replace(" ", "_")
        allowed = {"manual_curated", "user_stated", "observed", "inferred", "tool_verified"}
        if normalized in allowed:
            return normalized
        return "manual_curated"

    def _evaluate_decision_guardrails(self, decision: dict[str, Any], *, actor: str, force_execute: bool) -> dict[str, Any]:
        action_type = str(decision.get("action_type", ""))
        risk_level = str(decision.get("risk_level", "medium"))
        if str(decision.get("status", "")) in {"executed", "rolled_back"}:
            return {
                "ok": False,
                "status": "blocked",
                "guardrail_status": "already_finalized",
                "message": "Decision is already finalized and cannot be executed again.",
            }
        if bool(decision.get("requires_approval")) and self._resolve_actor_role(actor) not in {"approver", "admin"} and not force_execute:
            return {
                "ok": False,
                "status": "blocked",
                "guardrail_status": "approval_required",
                "message": f"Decision action '{action_type}' requires approver/admin execution.",
            }
        if risk_level == "critical" and not force_execute and self._resolve_actor_role(actor) != "admin":
            return {
                "ok": False,
                "status": "blocked",
                "guardrail_status": "admin_required_for_critical",
                "message": "Critical decisions require admin execution unless force_execute is explicitly set.",
            }
        return {"ok": True, "guardrail_status": "passed"}

    def _run_decision_action(self, decision: dict[str, Any], *, actor: str, decision_note: str) -> dict[str, Any]:
        action_type = str(decision.get("action_type", ""))
        payload = dict(decision.get("payload", {}))
        alert = self.assistant_db.get_alert_record(str(decision.get("alert_id", ""))) if str(decision.get("alert_id", "")) else None
        if action_type == "create_ticket":
            alert_summary = str(payload.get("alert_summary", "")).strip() or str((alert or {}).get("summary", "Decision-generated alert"))
            target_device = None
            if str(decision.get("target_id", "")).isdigit():
                target_device = int(str(decision.get("target_id", "")))
            result = self.create_ticket(
                title=f"Alert Follow-up: {str((alert or {}).get('title', 'Decision'))}",
                severity=self._decision_risk_to_ticket_severity(str(decision.get("risk_level", "medium"))),
                device_id=target_device,
                description=alert_summary,
            )
            if result.get("ok"):
                self.assistant_db.update_alert_record(str(decision.get("alert_id", "")), status="acknowledged", acknowledged_by=actor)
            return result
        if action_type == "pause_batch":
            batch_run_id = str(dict((alert or {}).get("payload", {})).get("batch_run_id", "") or payload.get("alert_payload", {}).get("batch_run_id", ""))
            if not batch_run_id:
                return {"ok": False, "message": "Decision payload did not contain a batch_run_id for pause_batch."}
            return self.pause_batch_run(batch_run_id, actor, decision_note or "decision-engine pause")
        if action_type == "manual_review":
            return {
                "ok": True,
                "status": "manual_review_queued",
                "message": "Decision marked for manual review; no automatic action executed.",
                "payload": payload,
            }
        return {"ok": False, "message": f"Unsupported decision action: {action_type}"}

    def _run_decision_rollback(
        self,
        decision: dict[str, Any],
        execution: dict[str, Any],
        *,
        actor: str,
        reason: str,
    ) -> dict[str, Any]:
        action_type = str(decision.get("action_type", ""))
        result_payload = dict(execution.get("result_payload", {}))
        if action_type == "create_ticket":
            ticket_id = str(result_payload.get("ticket_id", ""))
            if not ticket_id:
                ticket_id = str(result_payload.get("result", {}).get("ticket_id", ""))
            if not ticket_id:
                return {"ok": False, "message": "Rollback requires a ticket_id from the execution result."}
            closed = self.close_ticket(ticket_id)
            return {**closed, "rollback_action": "close_ticket", "actor": actor, "reason": reason}
        if action_type == "pause_batch":
            batch_run_id = str(result_payload.get("batch_run_id", ""))
            if not batch_run_id:
                batch_run_id = str(result_payload.get("result", {}).get("batch_run_id", ""))
            if not batch_run_id:
                return {"ok": False, "message": "Rollback requires a batch_run_id from the execution result."}
            run = self.assistant_db.get_ota_batch_run(batch_run_id)
            if run is None:
                return {"ok": False, "message": "Batch run for rollback was not found."}
            summary = dict(run.get("summary_payload", {}))
            control = dict(summary.get("control", {}))
            control.update(
                {
                    "paused": False,
                    "resumed_by_rollback": True,
                    "resume_operator": actor,
                    "resume_note": reason,
                    "last_action": "rollback_pause_batch_run",
                }
            )
            summary["control"] = control
            self.assistant_db.update_ota_batch_run_status(batch_run_id, "waiting_next_batch", summary)
            return {"ok": True, "batch_run_id": batch_run_id, "status": "waiting_next_batch", "rollback_action": "resume_batch"}
        if action_type == "manual_review":
            return {"ok": True, "message": "Manual review decisions do not require rollback.", "rollback_action": "noop"}
        return {"ok": False, "message": f"Unsupported rollback action for decision type: {action_type}"}

    def _decision_risk_to_ticket_severity(self, risk_level: str) -> str:
        normalized = str(risk_level or "").strip().lower()
        if normalized == "critical":
            return "high"
        if normalized == "medium":
            return "medium"
        return "low"

    def _escalate_alert_severity(self, severity: str) -> str:
        order = ["info", "medium", "warning", "high", "critical"]
        normalized = str(severity or "info").strip().lower()
        if normalized not in order:
            return "high"
        index = order.index(normalized)
        return order[min(index + 1, len(order) - 1)]

    def _create_notification(
        self,
        *,
        source_type: str,
        source_id: str,
        target: str,
        severity: str,
        title: str,
        body: str,
        channel: str = "console",
    ) -> None:
        self.assistant_db.create_notification_record(
            NotificationRecord(
                notification_id=f"notif-{uuid4().hex[:12]}",
                source_type=source_type,
                source_id=source_id,
                target=target,
                channel=channel,
                severity=severity,
                status="queued",
                title=title,
                body=body,
            )
        )

    def snapshot_decision_metrics(self, reason: str = "manual") -> dict[str, Any]:
        metrics_payload = self.decision_metrics()
        metrics = dict(metrics_payload.get("metrics", {}))
        for metric_name in (
            "total_events",
            "total_alerts",
            "open_alerts",
            "resolved_alerts",
            "total_decisions",
            "approval_required_decisions",
            "approval_rate",
            "executed_decisions",
            "failed_executions",
            "blocked_executions",
            "execution_success_rate",
            "rollback_count",
            "rollback_failed_count",
            "rollback_rate",
            "blocked_rate",
        ):
            if metric_name not in metrics:
                continue
            self.assistant_db.create_decision_metric_snapshot(
                DecisionMetricSnapshotRecord(
                    snapshot_id=f"metricsnap-{uuid4().hex[:12]}",
                    metric_name=metric_name,
                    metric_value=float(metrics.get(metric_name, 0.0) or 0.0),
                    dimensions={"reason": reason},
                )
            )
        for risk_level, count in dict(metrics.get("risk_counts", {})).items():
            self.assistant_db.create_decision_metric_snapshot(
                DecisionMetricSnapshotRecord(
                    snapshot_id=f"metricsnap-{uuid4().hex[:12]}",
                    metric_name="risk_count",
                    metric_value=float(count),
                    dimensions={"reason": reason, "risk_level": risk_level},
                )
            )
        for blocked_reason, count in dict(metrics.get("blocked_reason_counts", {})).items():
            self.assistant_db.create_decision_metric_snapshot(
                DecisionMetricSnapshotRecord(
                    snapshot_id=f"metricsnap-{uuid4().hex[:12]}",
                    metric_name="blocked_reason_count",
                    metric_value=float(count),
                    dimensions={"reason": reason, "blocked_reason": blocked_reason},
                )
            )
        return self.list_decision_metric_snapshots(limit=30)

    def _require_actor_role(self, actor: str, required_role: str) -> dict[str, Any]:
        actor_role = self._resolve_actor_role(actor)
        if self._role_rank.get(actor_role, 0) < self._role_rank.get(required_role, 0):
            return {
                "ok": False,
                "message": f"Actor '{actor}' requires role>={required_role} but resolved as {actor_role}.",
                "actor": actor,
                "actor_role": actor_role,
                "required_role": required_role,
            }
        return {"ok": True, "actor": actor, "actor_role": actor_role, "required_role": required_role}

    def _prune_agent_memories(self) -> None:
        self.assistant_db.trim_session_memories(self.settings.agent_memory_session_store_limit)
        self.assistant_db.trim_long_term_memories("global", self.settings.agent_memory_long_term_store_limit)

    def _build_session_timeline(
        self,
        action_audits: list[dict[str, Any]],
        tool_calls: list[dict[str, Any]],
    ) -> list[dict[str, Any]]:
        timeline: list[dict[str, Any]] = []
        for item in action_audits:
            timeline.append(
                {
                    "kind": "action_audit",
                    "created_at": item.get("created_at", ""),
                    "status": item.get("status", ""),
                    "name": item.get("action_type", ""),
                    "payload": item,
                }
            )
        for item in tool_calls:
            timeline.append(
                {
                    "kind": "tool_call",
                    "created_at": item.get("created_at", ""),
                    "status": item.get("status", ""),
                    "name": item.get("tool_name", ""),
                    "payload": item,
                }
            )
        timeline.sort(key=lambda entry: (str(entry.get("created_at", "")), str(entry.get("kind", ""))))
        return timeline

    def _run_tool(
        self,
        session_id: str,
        question: str,
        tool_name: str,
        tool_input: dict[str, Any],
        fn: Any,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        try:
            result = fn()
        except Exception as exc:
            result = {
                "ok": False,
                "message": f"Tool {tool_name} execution failed.",
                "error": f"{type(exc).__name__}: {exc}",
            }

        trace_item = {
            "tool_name": tool_name,
            "tool_input": tool_input,
            "tool_status": "ok" if result.get("ok") else "error",
            "tool_output_summary": self._summarize_tool_output(tool_name, result),
            "source": result.get("source", "runtime"),
        }
        self.assistant_db.log_tool_call(
            ToolCallLogRecord(
                session_id=session_id,
                question=question,
                tool_name=tool_name,
                tool_input=tool_input,
                tool_output=result,
                status=trace_item["tool_status"],
            )
        )
        return result, trace_item

    def _run_runtime_tool(
        self,
        session_id: str,
        question: str,
        tool_name: str,
        tool_input: dict[str, Any],
        fn: Any,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        return self._run_tool(session_id, question, tool_name, tool_input, fn)

    def _summarize_tool_output(self, tool_name: str, result: dict[str, Any]) -> str:
        if not result.get("ok"):
            return result.get("message", f"{tool_name} failed.")
        if tool_name == "get_device_status":
            return (
                f"device {result.get('status', 'unknown')}, "
                f"status_source={result.get('status_source', 'unknown')}"
            )
        if tool_name == "get_sensor_history":
            return f"returned {result.get('count', 0)} sensor records"
        if tool_name == "get_system_events":
            return f"returned {result.get('count', 0)} system events"
        if tool_name == "get_ota_task_status":
            task = result.get("task", {})
            return (
                f"ota task {result.get('task_id', task.get('task_uuid', 'unknown'))} "
                f"state={task.get('state', 'unknown')}"
            )
        if tool_name == "get_database_summary":
            summary = result.get("summary", {})
            return (
                f"history_db_exists={summary.get('history_db_exists')}, "
                f"ota_store_format={summary.get('ota_store', {}).get('format', 'unknown')}"
            )
        if tool_name == "get_firmware_manifest":
            manifest = result.get("manifest", {})
            return (
                f"firmware_id={manifest.get('firmware_id', result.get('firmware_id', 'unknown'))}, "
                f"device_type={manifest.get('device_type', 'unknown')}"
            )
        if tool_name == "list_recent_ota_tasks_for_device":
            return f"returned {result.get('count', 0)} OTA tasks"
        if tool_name == "create_ota_task":
            task = result.get("task", {})
            return (
                f"created task_uuid={task.get('task_uuid', 'unknown')}, "
                f"state={task.get('state', 'unknown')}"
            )
        return result.get("message", f"{tool_name} completed")

    def _format_ts_ms(self, value: Any) -> str:
        try:
            ts_ms = int(value)
        except (TypeError, ValueError):
            return str(value)
        dt = datetime.fromtimestamp(ts_ms / 1000, tz=timezone.utc)
        return dt.strftime("%Y-%m-%d %H:%M:%S UTC")
