from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from gateway_ai_assistant.config import Settings, get_settings
from gateway_ai_assistant.service import AssistantService
from gateway_ai_assistant.sql.assistant_db import (
    ActionAuditLogRecord,
    ApprovalRequestRecord,
    EvalCaseResultRecord,
    EvalRunRecord,
    OtaBatchRunItemRecord,
    OtaBatchRunRecord,
    ToolCallLogRecord,
)
from gateway_ai_assistant.sql.query_templates import QueryResult


class ServiceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmpdir = tempfile.TemporaryDirectory()
        base = get_settings()
        data_dir = Path(self.tmpdir.name) / "data"
        reports_dir = Path(self.tmpdir.name) / "reports"
        data_dir.mkdir(parents=True, exist_ok=True)
        reports_dir.mkdir(parents=True, exist_ok=True)
        self.settings = Settings(
            repo_root=base.repo_root,
            app_root=base.app_root,
            data_dir=data_dir,
            reports_dir=reports_dir,
            prompts_dir=base.prompts_dir,
            index_path=data_dir / "rag_index.json",
            gateway_root=base.gateway_root,
            gateway_history_db=base.gateway_history_db,
            gateway_ota_db=base.gateway_ota_db,
            assistant_db=data_dir / "assistant.db",
            llm_api_key="",
            llm_base_url=base.llm_base_url,
            llm_model=base.llm_model,
        )
        self.service = AssistantService(self.settings)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_extract_device_id(self) -> None:
        self.assertEqual(self.service._extract_device_id("查询 device_id=12 最近一次传感器数据"), 12)

    def test_extract_device_name(self) -> None:
        self.assertEqual(self.service._extract_device_name("查询 sensor-01 最近一次传感器数据"), "sensor-01")

    def test_extract_device_ref(self) -> None:
        self.assertEqual(self.service._extract_device_ref("查询 DEVICE_001 当前状态"), "DEVICE_001")

    def test_extract_task_id(self) -> None:
        self.assertEqual(self.service._extract_task_id("查询 task_id=ota-001 的状态"), "ota-001")

    def test_guide_facts_contains_runtime_capability(self) -> None:
        facts = self.service._guide_facts()
        self.assertTrue(any("设备运行状态" in item for item in facts))

    def test_chat_guide_route(self) -> None:
        result = self.service.chat("你会干什么？")
        self.assertEqual(result["route_type"], "Guide")
        self.assertEqual(result["response_source"], "guide")
        self.assertIn("Confirmed:", result["answer"])
        self.assertIn("User-stated:", result["answer"])

    def test_ingest_realtime_event_creates_alert_and_decision(self) -> None:
        result = self.service.ingest_realtime_event(
            "device_offline",
            source="runtime_probe",
            severity="high",
            device_id=7,
            payload={"last_seen_sec": 360},
        )

        self.assertTrue(result["ok"])
        self.assertEqual(result["event"]["event_type"], "device_offline")
        self.assertEqual(result["alert_count"], 1)
        self.assertEqual(result["decision_count"], 1)
        self.assertEqual(result["alerts"][0]["status"], "open")
        self.assertEqual(result["decisions"][0]["action_type"], "create_ticket")

    def test_ingest_realtime_event_dedupes_open_alert(self) -> None:
        first = self.service.ingest_realtime_event(
            "sensor_upload_gap",
            severity="warning",
            device_id=3,
            payload={"window_min": 15, "missing_samples": 8},
        )
        second = self.service.ingest_realtime_event(
            "sensor_upload_gap",
            severity="warning",
            device_id=3,
            payload={"window_min": 15, "missing_samples": 12},
        )

        self.assertTrue(first["ok"])
        self.assertTrue(second["ok"])
        alerts = self.service.list_alert_records(limit=10)["items"]
        self.assertEqual(len(alerts), 1)
        self.assertEqual(alerts[0]["occurrence_count"], 2)

    def test_suppressed_alert_skips_new_decision_generation(self) -> None:
        ingested = self.service.ingest_realtime_event(
            "sensor_upload_gap",
            severity="warning",
            device_id=4,
            payload={"window_min": 15, "missing_samples": 6},
        )
        alert_id = ingested["alerts"][0]["alert_id"]
        suppressed = self.service.suppress_alert(alert_id, actor="operator", minutes=30, reason="maintenance")
        self.assertTrue(suppressed["ok"])

        retrigger = self.service.ingest_realtime_event(
            "sensor_upload_gap",
            severity="warning",
            device_id=4,
            payload={"window_min": 15, "missing_samples": 9},
        )
        self.assertTrue(retrigger["ok"])
        self.assertEqual(retrigger["alerts"][0]["status"], "suppressed")
        self.assertEqual(retrigger["decision_count"], 0)

    def test_alert_escalates_after_repeated_occurrences(self) -> None:
        for _ in range(3):
            self.service.ingest_realtime_event(
                "sensor_upload_gap",
                severity="warning",
                device_id=12,
                payload={"window_min": 20, "missing_samples": 10},
            )
        alert = self.service.list_alert_records(limit=10)["items"][0]
        self.assertEqual(alert["severity"], "high")
        self.assertGreaterEqual(int(alert["escalation_level"]), 1)

    def test_policy_catalog_exposes_versioned_policies(self) -> None:
        policies = self.service.list_decision_policies()
        self.assertTrue(policies["ok"])
        self.assertGreaterEqual(policies["count"], 3)
        self.assertTrue(all(item["version"] == "v1" for item in policies["items"]))

    def test_realtime_alert_creates_notification_record(self) -> None:
        self.service.ingest_realtime_event(
            "device_offline",
            severity="high",
            device_id=21,
            payload={"last_seen_sec": 900},
        )
        notifications = self.service.list_notification_records(limit=10)
        self.assertTrue(notifications["ok"])
        self.assertGreaterEqual(notifications["count"], 1)
        self.assertEqual(notifications["items"][0]["source_type"], "alert")

    def test_reload_decision_policies_reads_external_json(self) -> None:
        policy_path = self.service.settings.decision_policy_path
        policy_path.write_text(
            json.dumps(
                [
                    {
                        "policy_id": "policy_custom_ticket",
                        "version": "v2",
                        "alert_rule_id": "device_offline_alert",
                        "action_type": "create_ticket",
                        "risk_level": "medium",
                        "requires_approval": False,
                        "target_type": "device",
                        "title": "Custom offline ticket",
                        "enabled": True,
                        "notification_target": "custom_oncall",
                        "escalation_target": "custom_manager",
                    }
                ],
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
        reloaded = self.service.reload_decision_policies()
        self.assertTrue(reloaded["ok"])
        self.assertEqual(reloaded["items"][0]["version"], "v2")
        self.assertEqual(reloaded["items"][0]["notification_target"], "custom_oncall")

    def test_export_decision_metrics_csv_returns_csv_payload(self) -> None:
        self.service.ingest_realtime_event(
            "sensor_upload_gap",
            severity="warning",
            device_id=7,
            payload={"window_min": 10, "missing_samples": 5},
        )
        exported = self.service.export_decision_metrics_csv(limit=20)
        self.assertTrue(exported["ok"])
        self.assertIn("metric_name,metric_value", exported["csv"])

    def test_acknowledge_and_resolve_alert(self) -> None:
        ingested = self.service.ingest_realtime_event(
            "ota_failure_spike",
            severity="critical",
            payload={"batch_run_id": "batch-1", "failure_rate": 0.65},
        )
        alert_id = ingested["alerts"][0]["alert_id"]

        acknowledged = self.service.acknowledge_alert(alert_id, actor="operator")
        resolved = self.service.resolve_alert(alert_id, actor="operator")

        self.assertTrue(acknowledged["ok"])
        self.assertEqual(acknowledged["alert"]["status"], "acknowledged")
        self.assertTrue(resolved["ok"])
        self.assertEqual(resolved["alert"]["status"], "resolved")

    def test_decision_metrics_reflect_alert_pipeline(self) -> None:
        self.service.ingest_realtime_event(
            "ota_failure_spike",
            severity="critical",
            payload={"batch_run_id": "batch-9", "failure_rate": 0.72},
        )

        metrics = self.service.decision_metrics()

        self.assertTrue(metrics["ok"])
        self.assertEqual(metrics["metrics"]["total_alerts"], 1)
        self.assertEqual(metrics["metrics"]["total_decisions"], 1)
        self.assertGreater(metrics["metrics"]["approval_rate"], 0.0)
        self.assertIn("history", metrics)

    def test_execute_decision_create_ticket_and_rollback(self) -> None:
        ingested = self.service.ingest_realtime_event(
            "device_offline",
            source="runtime_probe",
            severity="high",
            device_id=9,
            payload={"last_seen_sec": 420},
        )
        decision_id = ingested["decisions"][0]["decision_id"]

        executed = self.service.execute_decision(
            decision_id,
            actor="operator",
            decision_note="auto follow-up",
        )

        self.assertTrue(executed["ok"])
        self.assertEqual(executed["execution"]["status"], "executed")
        self.assertEqual(executed["decision"]["status"], "executed")
        self.assertEqual(executed["result"]["status"], "open")

        rolled_back = self.service.rollback_decision_execution(
            decision_id,
            actor="approver",
            reason="close generated ticket",
        )

        self.assertTrue(rolled_back["ok"])
        self.assertEqual(rolled_back["decision"]["status"], "rolled_back")
        self.assertEqual(rolled_back["rollback"]["status"], "rolled_back")
        self.assertEqual(rolled_back["result"]["ticket"]["status"], "closed")

    def test_execute_decision_blocks_operator_for_critical_pause_batch(self) -> None:
        self.service.assistant_db.create_ota_batch_run(
            OtaBatchRunRecord(
                batch_run_id="batch-run-decision-test",
                approval_id="approval-decision-test",
                firmware_id="fw-1",
                transport="serial",
                target="127.0.0.1:19090",
                batch_size=2,
                batch_count=1,
                total_devices=2,
                status="waiting_next_batch",
                summary_payload={},
            )
        )
        self.service.assistant_db.create_ota_batch_run_items(
            [
                OtaBatchRunItemRecord(batch_run_id="batch-run-decision-test", batch_index=1, device_id=1, status="queued"),
                OtaBatchRunItemRecord(batch_run_id="batch-run-decision-test", batch_index=1, device_id=2, status="waiting_batch"),
            ]
        )
        ingested = self.service.ingest_realtime_event(
            "ota_failure_spike",
            severity="critical",
            payload={"batch_run_id": "batch-run-decision-test", "failure_rate": 0.9},
        )
        decision_id = ingested["decisions"][0]["decision_id"]

        blocked = self.service.execute_decision(
            decision_id,
            actor="operator",
            decision_note="try pause",
        )
        self.assertFalse(blocked["ok"])
        self.assertEqual(blocked["execution"]["status"], "blocked")

        executed = self.service.execute_decision(
            decision_id,
            actor="admin",
            decision_note="admin pause",
        )
        self.assertTrue(executed["ok"])
        self.assertEqual(executed["decision"]["status"], "executed")
        self.assertEqual(executed["result"]["status"], "paused")

    def test_snapshot_decision_metrics_persists_history(self) -> None:
        self.service.ingest_realtime_event(
            "device_offline",
            source="runtime_probe",
            severity="high",
            device_id=5,
            payload={"last_seen_sec": 500},
        )

        snapshot = self.service.snapshot_decision_metrics(reason="test_capture")

        self.assertTrue(snapshot["ok"])
        self.assertGreater(snapshot["count"], 0)
        self.assertTrue(any(item["dimensions"].get("reason") == "test_capture" for item in snapshot["items"]))

    def test_decision_metrics_include_blocked_reason_breakdown(self) -> None:
        self.service.assistant_db.create_ota_batch_run(
            OtaBatchRunRecord(
                batch_run_id="batch-run-blocked-metrics",
                approval_id="approval-blocked-metrics",
                firmware_id="fw-1",
                transport="serial",
                target="127.0.0.1:19090",
                batch_size=1,
                batch_count=1,
                total_devices=1,
                status="waiting_next_batch",
                summary_payload={},
            )
        )
        self.service.assistant_db.create_ota_batch_run_items(
            [OtaBatchRunItemRecord(batch_run_id="batch-run-blocked-metrics", batch_index=1, device_id=1, status="queued")]
        )
        ingested = self.service.ingest_realtime_event(
            "ota_failure_spike",
            severity="critical",
            payload={"batch_run_id": "batch-run-blocked-metrics", "failure_rate": 0.95},
        )
        decision_id = ingested["decisions"][0]["decision_id"]
        blocked = self.service.execute_decision(decision_id, actor="operator", decision_note="blocked")

        self.assertFalse(blocked["ok"])
        metrics = self.service.decision_metrics()
        self.assertGreater(metrics["metrics"]["blocked_executions"], 0)
        self.assertIn("approval_required", metrics["metrics"]["blocked_reason_counts"])

    @patch("gateway_ai_assistant.service.AssistantService._chat_v1")
    @patch("gateway_ai_assistant.service.AssistantService.chat_agent")
    def test_chat_prefers_agent_when_available(self, mock_chat_agent, mock_chat_v1) -> None:
        service = AssistantService(self.settings)
        service.settings = Settings(**{**self.settings.__dict__, "llm_api_key": "test-key"})
        mock_chat_agent.return_value = {"ok": True, "mode": "react_agent", "answer": "agent"}
        result = service.chat("DEVICE_001 当前状态正常吗？")
        self.assertEqual(result["mode"], "react_agent")
        mock_chat_agent.assert_called_once()
        mock_chat_v1.assert_not_called()

    @patch("gateway_ai_assistant.service.AssistantService._chat_v1")
    @patch("gateway_ai_assistant.service.AssistantService.chat_agent")
    def test_chat_falls_back_to_v1_when_agent_fails(self, mock_chat_agent, mock_chat_v1) -> None:
        service = AssistantService(self.settings)
        service.settings = Settings(**{**self.settings.__dict__, "llm_api_key": "test-key"})
        mock_chat_agent.side_effect = RuntimeError("boom")
        mock_chat_v1.return_value = {"route_type": "RAG", "answer": "fallback"}
        result = service.chat("AA55 协议帧格式是什么？")
        self.assertEqual(result["route_type"], "RAG")
        mock_chat_v1.assert_called_once()

    def test_chat_agent_requires_llm_api_key(self) -> None:
        with self.assertRaises(RuntimeError):
            self.service.chat_agent("DEVICE_001 当前状态正常吗？")

    @patch("gateway_ai_assistant.agent.react_agent.ReActAgent.invoke")
    def test_chat_agent_delegates_to_react_agent(self, mock_invoke) -> None:
        service = AssistantService(Settings(**{**self.settings.__dict__, "llm_api_key": "test-key"}))
        mock_invoke.return_value = {
            "ok": True,
            "session_id": "agent-123",
            "mode": "react_agent",
            "answer": "设备在线",
            "tool_trace": [{"tool": "get_device_status", "args": {"device_ref": "DEVICE_001"}}],
        }
        result = service.chat_agent("DEVICE_001 当前状态正常吗？", "agent-123")
        self.assertTrue(result["ok"])
        self.assertIn("Confirmed:", result["answer"])
        self.assertIn("Executed tool get_device_status.", result["answer"])
        mock_invoke.assert_called_once_with("DEVICE_001 当前状态正常吗？", "agent-123")

    @patch("gateway_ai_assistant.agent.react_agent.ReActAgent.astream")
    def test_chat_agent_stream_delegates_to_react_agent(self, mock_astream) -> None:
        service = AssistantService(Settings(**{**self.settings.__dict__, "llm_api_key": "test-key"}))
        mock_astream.return_value = object()
        result = service.chat_agent_stream("DEVICE_001 当前状态正常吗？", "agent-123")
        self.assertIs(result, mock_astream.return_value)
        mock_astream.assert_called_once_with("DEVICE_001 当前状态正常吗？", "agent-123")

    @patch("gateway_ai_assistant.agent.react_agent.ReActAgent.resume")
    def test_chat_agent_resume_delegates_to_react_agent(self, mock_resume) -> None:
        service = AssistantService(Settings(**{**self.settings.__dict__, "llm_api_key": "test-key"}))
        mock_resume.return_value = {"ok": True, "session_id": "agent-123", "mode": "react_agent_resumed"}
        result = service.chat_agent_resume("agent-123", decision="reject", edit_args={"approved_by": "tester"})
        self.assertTrue(result["ok"])
        mock_resume.assert_called_once_with("agent-123", decision="reject", edit_args={"approved_by": "tester"})

    def test_run_agent_live_eval_returns_config_error_without_llm_key(self) -> None:
        result = self.service.run_agent_live_eval()
        self.assertFalse(result["ok"])
        self.assertIn("LLM_API_KEY not configured", result["message"])

    @patch("gateway_ai_assistant.service.AssistantService.export_agent_session_replay")
    @patch("gateway_ai_assistant.service.AssistantService.run_agent_live_eval")
    def test_run_agent_eval_suite_persists_results_and_comparison(
        self,
        mock_run_agent_live_eval,
        mock_export_agent_session_replay,
    ) -> None:
        self.service.assistant_db.create_eval_run(
            EvalRunRecord(
                run_id="eval-prev-1",
                eval_type="agent_live",
                status="completed",
                case_count=1,
                summary_payload={"total": 1, "passed": 1},
            )
        )
        self.service.assistant_db.create_eval_case_results(
            [
                EvalCaseResultRecord(
                    run_id="eval-prev-1",
                    case_index=1,
                    session_id="agent-live-eval-1",
                    question="DEVICE_001 当前状态正常吗？",
                    description="status",
                    ok=True,
                    agent_ok=True,
                    interrupted=False,
                    expected_tools=["get_device_status"],
                    actual_tools=["get_device_status"],
                    missing_tools=[],
                    forbidden_called=[],
                    tool_order_ok=True,
                    answer_present=True,
                    failure_reasons=[],
                    result_payload={"ok": True},
                    replay_payload={"ok": True, "session_id": "agent-live-eval-1"},
                )
            ]
        )
        mock_run_agent_live_eval.return_value = {
            "ok": True,
            "summary": {"total": 1, "passed": 0, "failed": 1, "pass_rate": 0.0},
            "cases": [
                {
                    "question": "DEVICE_001 当前状态正常吗？",
                    "description": "status",
                    "session_id": "agent-live-eval-1",
                    "ok": False,
                    "agent_ok": True,
                    "interrupted": False,
                    "expected_tools": ["get_device_status"],
                    "actual_tools": [],
                    "missing_tools": ["get_device_status"],
                    "forbidden_called": [],
                    "tool_order_ok": False,
                    "answer_present": True,
                    "failure_reasons": ["missing_tools:get_device_status"],
                    "result": {"ok": True, "tool_trace": []},
                }
            ],
        }
        mock_export_agent_session_replay.return_value = {
            "ok": True,
            "session_id": "agent-live-eval-1",
            "timeline": [],
            "timeline_count": 0,
        }

        result = self.service.run_agent_eval_suite()

        self.assertTrue(result["ok"])
        self.assertTrue(result["run_id"].startswith("eval-"))
        self.assertEqual(len(result["comparison"]["regressions"]), 1)
        stored = self.service.get_agent_eval_run(result["run_id"])
        self.assertTrue(stored["ok"])
        self.assertEqual(stored["case_count"], 1)
        self.assertEqual(stored["cases"][0]["session_id"], "agent-live-eval-1")
        self.assertEqual(stored["run"]["baseline_run_id"], "eval-prev-1")

    @patch("gateway_ai_assistant.agent.react_agent.ReActAgent.inspect_session")
    def test_get_agent_session_includes_agent_context_when_llm_enabled(self, mock_inspect_session) -> None:
        service = AssistantService(Settings(**{**self.settings.__dict__, "llm_api_key": "test-key"}))
        mock_inspect_session.return_value = {
            "ok": True,
            "session_id": "agent-ctx-1",
            "message_count": 42,
            "approx_char_count": 13000,
            "context_warning": True,
        }

        result = service.get_agent_session("agent-ctx-1")

        self.assertTrue(result["ok"])
        self.assertTrue(result["context_warning"])
        self.assertEqual(result["agent_context"]["message_count"], 42)
        mock_inspect_session.assert_called_once_with("agent-ctx-1")

    def test_export_agent_session_replay_builds_sorted_timeline(self) -> None:
        self.service.assistant_db.log_action_audit(
            ActionAuditLogRecord(
                session_id="agent-replay-1",
                action_type="agent_execute",
                operator="agent",
                request_payload={"question": "q"},
                result_payload={"ok": True},
                status="ok",
            )
        )
        self.service.assistant_db.log_tool_call(
            ToolCallLogRecord(
                session_id="agent-replay-1",
                question="q",
                tool_name="get_device_status",
                tool_input={"device_ref": "DEVICE_001"},
                tool_output={"ok": True},
                status="ok",
            )
        )

        replay = self.service.export_agent_session_replay("agent-replay-1")

        self.assertTrue(replay["ok"])
        self.assertEqual(replay["timeline_count"], 2)
        self.assertEqual({item["kind"] for item in replay["timeline"]}, {"action_audit", "tool_call"})

    def test_remember_agent_interaction_persists_session_and_long_term_memory(self) -> None:
        self.service.remember_agent_interaction(
            "agent-memory-1",
            "以后默认用 tcp_binary，回答简洁一点，device_id=7",
            "后续会优先使用 tcp_binary，并尽量简洁回答。",
            [{"tool": "ota_create_request", "args": {"device_id": 7}}],
        )

        snapshot = self.service.get_agent_memory_snapshot("agent-memory-1")

        self.assertTrue(snapshot["ok"])
        self.assertEqual(snapshot["session_memory"]["key_facts"]["device_id"], 7)
        self.assertGreaterEqual(snapshot["long_term_count"], 2)
        keys = {item["memory_key"] for item in snapshot["long_term_memories"]}
        self.assertIn("preferred_transport", keys)
        self.assertIn("answer_style", keys)

    def test_build_agent_memory_context_contains_session_and_long_term_memory(self) -> None:
        self.service.remember_agent_interaction(
            "agent-memory-ctx-1",
            "以后默认用 serial，中文回答，device_id=3",
            "将优先使用 serial，并使用中文回答。",
            [{"tool": "ota_risk_check", "args": {"device_id": 3}}],
        )

        context = self.service.build_agent_memory_context("agent-memory-ctx-1")

        self.assertTrue(context["ok"])
        self.assertIn("Session summary:", context["memory_prompt"])
        self.assertIn("Long-term operator preferences:", context["memory_prompt"])

    def test_get_agent_memory_snapshot_includes_related_sessions(self) -> None:
        self.service.remember_agent_interaction(
            "agent-related-old",
            "查询 device_id=8 最近状态",
            "设备 8 在线。",
            [{"tool": "get_device_status", "args": {"device_ref": "8"}}],
        )
        self.service.remember_agent_interaction(
            "agent-related-new",
            "继续看 device_id=8 的系统事件",
            "最近有 2 条系统事件。",
            [{"tool": "get_system_events", "args": {"limit": 5}}],
        )

        snapshot = self.service.get_agent_memory_snapshot("agent-current", question="请诊断 device_id=8")

        self.assertTrue(snapshot["ok"])
        self.assertGreaterEqual(snapshot["related_session_count"], 1)
        self.assertEqual(snapshot["related_session_memories"][0]["key_facts"]["device_id"], 8)

    def test_remember_agent_interaction_prunes_excess_session_memories(self) -> None:
        self.service.settings = Settings(**{**self.settings.__dict__, "agent_memory_session_store_limit": 2})
        self.service.remember_agent_interaction("agent-prune-1", "device_id=1", "a", [])
        self.service.remember_agent_interaction("agent-prune-2", "device_id=2", "b", [])
        self.service.remember_agent_interaction("agent-prune-3", "device_id=3", "c", [])

        memories = self.service.list_agent_memories(limit=10)

        self.assertEqual(memories["session_count"], 2)
        session_ids = {item["session_id"] for item in memories["session_memories"]}
        self.assertNotIn("agent-prune-1", session_ids)

    def test_create_update_and_delete_long_term_memory(self) -> None:
        created = self.service.create_or_update_long_term_memory(
            memory_key="preferred_transport",
            content="Prefer tcp_binary transport.",
            memory_type="manual",
            provenance="tool_verified",
            status="active",
            is_pinned=True,
            actor="approver",
        )
        self.assertTrue(created["ok"])
        memory_id = int(created["memory"]["id"])
        self.assertEqual(created["memory"]["provenance"], "tool_verified")

        updated = self.service.update_long_term_memory(
            memory_id,
            content="Prefer serial transport.",
            provenance="inferred",
            status="inactive",
            is_pinned=False,
            actor="approver",
        )
        self.assertTrue(updated["ok"])
        self.assertEqual(updated["memory"]["status"], "inactive")
        self.assertEqual(updated["memory"]["content"], "Prefer serial transport.")
        self.assertEqual(updated["memory"]["provenance"], "inferred")

        snapshot = self.service.get_agent_memory_snapshot("agent-noop", question="")
        keys = {item["memory_key"] for item in snapshot["long_term_memories"]}
        self.assertNotIn("preferred_transport", keys)

        deleted = self.service.delete_long_term_memory(memory_id, actor="admin")
        self.assertTrue(deleted["ok"])

        audits = self.service.list_memory_audits(limit=10, memory_key="preferred_transport")
        action_types = [item["action_type"] for item in audits["items"]]
        self.assertIn("long_term_upserted", action_types)
        self.assertIn("long_term_updated", action_types)
        self.assertIn("long_term_deleted", action_types)

    def test_upsert_and_delete_session_memory(self) -> None:
        upserted = self.service.upsert_agent_session_memory(
            "agent-session-edit-1",
            actor="operator",
            summary="manual summary",
            key_facts={"device_id": 11},
            last_question="q",
            last_answer="a",
        )
        self.assertTrue(upserted["ok"])
        self.assertEqual(upserted["session_memory"]["key_facts"]["device_id"], 11)

        deleted = self.service.delete_agent_session_memory("agent-session-edit-1", actor="operator")
        self.assertTrue(deleted["ok"])

    def test_remember_agent_interaction_creates_memory_candidate_when_user_requests_memory(self) -> None:
        self.service.remember_agent_interaction(
            "agent-candidate-1",
            "记住 approval_id=approval-77，后续我要继续跟进",
            "已记录后续跟进意图。",
            [],
        )

        candidates = self.service.list_memory_candidates(limit=10)

        self.assertEqual(candidates["count"], 1)
        self.assertEqual(candidates["items"][0]["memory_key"], "tracked_approval_approval-77")

    def test_approve_memory_candidate_promotes_to_long_term_memory(self) -> None:
        self.service.remember_agent_interaction(
            "agent-candidate-2",
            "记住 task_id=ota-99，后续继续看这个任务",
            "已记录任务。",
            [],
        )
        candidate_id = self.service.list_memory_candidates(limit=10)["items"][0]["candidate_id"]

        approved = self.service.approve_memory_candidate(candidate_id, reviewed_by="approver", decision_note="keep it")

        self.assertTrue(approved["ok"])
        self.assertEqual(approved["candidate"]["status"], "approved")
        self.assertEqual(approved["memory"]["provenance"], "user_stated")
        memories = self.service.list_agent_memories(limit=20)
        keys = {item["memory_key"] for item in memories["long_term_memories"]}
        self.assertIn("tracked_task_ota-99", keys)
        audits = self.service.list_memory_audits(limit=10, memory_key="tracked_task_ota-99")
        self.assertTrue(any(item["action_type"] == "candidate_approved" for item in audits["items"]))

    def test_reject_memory_candidate_marks_candidate_rejected(self) -> None:
        self.service.remember_agent_interaction(
            "agent-candidate-3",
            "记住 device_id=5，后续跟进这台设备",
            "已记录设备。",
            [],
        )
        candidate_id = self.service.list_memory_candidates(limit=10)["items"][0]["candidate_id"]

        rejected = self.service.reject_memory_candidate(candidate_id, reviewed_by="approver", decision_note="noise")

        self.assertTrue(rejected["ok"])
        self.assertEqual(rejected["candidate"]["status"], "rejected")

    def test_high_risk_memory_candidate_requires_two_stage_approval(self) -> None:
        self.service.remember_agent_interaction(
            "agent-candidate-stage-1",
            "记住这个结论，后续提醒我",
            "可能是网络抖动导致，需要后续人工确认。",
            [],
        )
        candidate = self.service.list_memory_candidates(limit=10, status="review_queue")["items"][0]

        self.assertGreaterEqual(float(candidate["risk_score"]), 0.4)

        stage1 = self.service.approve_memory_candidate(
            candidate["candidate_id"],
            reviewed_by="approver",
            decision_note="先做第一审",
        )
        self.assertTrue(stage1["ok"])
        self.assertEqual(stage1["stage"], "stage1")
        self.assertEqual(stage1["candidate"]["status"], "stage1_approved")

        same_reviewer = self.service.approve_memory_candidate(
            candidate["candidate_id"],
            reviewed_by="approver",
            decision_note="同一人二审",
        )
        self.assertFalse(same_reviewer["ok"])

        final = self.service.approve_memory_candidate(
            candidate["candidate_id"],
            reviewed_by="admin",
            decision_note="二审通过",
        )
        self.assertTrue(final["ok"])
        self.assertEqual(final["stage"], "final")
        self.assertEqual(final["candidate"]["status"], "approved")

    def test_memory_candidate_merge_reuses_existing_pending_candidate(self) -> None:
        self.service.remember_agent_interaction(
            "agent-candidate-merge-1",
            "记住 approval_id=approval-88，后续跟进",
            "已记录审批。",
            [],
        )
        first = self.service.list_memory_candidates(limit=10)["items"][0]
        self.service.remember_agent_interaction(
            "agent-candidate-merge-2",
            "记住 approval_id=approval-88，后续继续跟进这条审批",
            "继续记录审批。",
            [],
        )

        candidates = self.service.list_memory_candidates(limit=10)["items"]

        self.assertEqual(len(candidates), 1)
        self.assertEqual(candidates[0]["candidate_id"], first["candidate_id"])
        self.assertIn("继续跟进", candidates[0]["rationale"])
        self.assertEqual(candidates[0]["provenance"], "user_stated")
        audits = self.service.list_memory_audits(limit=10, memory_key="tracked_approval_approval-88")
        self.assertTrue(any(item["action_type"] == "candidate_merged" for item in audits["items"]))

    def test_memory_permission_blocks_operator_from_creating_long_term_memory(self) -> None:
        result = self.service.create_or_update_long_term_memory(
            memory_key="preferred_transport",
            content="Prefer serial.",
            actor="operator",
        )
        self.assertFalse(result["ok"])
        self.assertEqual(result["required_role"], "approver")

    def test_memory_permission_blocks_viewer_from_approving_candidate(self) -> None:
        self.service.remember_agent_interaction(
            "agent-candidate-role-1",
            "记住 task_id=ota-11，后续跟进",
            "已记录任务。",
            [],
        )
        candidate_id = self.service.list_memory_candidates(limit=10)["items"][0]["candidate_id"]
        result = self.service.approve_memory_candidate(candidate_id, reviewed_by="viewer", decision_note="try")
        self.assertFalse(result["ok"])
        self.assertEqual(result["required_role"], "approver")

    def test_route_sql_ota_state_count(self) -> None:
        result = self.service.route_sql("统计 OTA 任务状态数量")
        self.assertTrue("GROUP BY state" in result.sql or "database not found" in result.note)

    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_route_runtime_status(self, mock_get_device_status) -> None:
        mock_get_device_status.return_value = {"ok": True, "status": "online", "status_source": "http_api:/api/devices"}
        result = self.service.route_runtime("DEVICE_001 当前状态正常吗？")
        self.assertTrue(result["ok"])
        mock_get_device_status.assert_called_once_with("DEVICE_001")

    def test_route_hybrid_sensor_activity(self) -> None:
        result = self.service.route_hybrid_sql("最近传感器上报减少，可能是什么原因？")
        self.assertEqual(result["summary_type"], "sensor_activity")

    def test_route_hybrid_ota_failure(self) -> None:
        result = self.service.route_hybrid_sql("最近 OTA 失败任务变多，结合文档分析可能原因")
        self.assertEqual(result["summary_type"], "ota_failure")

    def test_summarize_query_result_sensor_latest(self) -> None:
        result = QueryResult(
            sql="",
            columns=["id", "ts_unix_ms", "device_id", "device_name", "frame_type", "temperature", "humidity", "light", "last_error"],
            rows=[[1, 1779679389402, 1, "sensor-01", "sensor_data", 25.1, 52.6, 12.0, ""]],
            note="",
        )
        facts = self.service._summarize_query_result(result)
        self.assertTrue(any("sensor-01" in item for item in facts))

    def test_summarize_hybrid_result_ota(self) -> None:
        data = {
            "summary_type": "ota_failure",
            "failure_summary": {"rows": [[11, 0, "tcp_binary send failed"]]},
            "failure_reasons": {"rows": [["tcp_binary send failed", 6]]},
            "recent_failed_tasks": {"rows": [[1], [2]]},
        }
        facts = self.service._summarize_hybrid_result(data)
        self.assertTrue(any("tcp_binary send failed" in item for item in facts))

    def test_build_rag_prompt_contains_citation(self) -> None:
        prompt = self.service._build_rag_prompt(
            "AA55 协议帧格式是什么？",
            [
                {
                    "source_path": "serial-gateway/docs/protocol.md",
                    "section_title": "协议帧",
                    "citation": "protocol.md / 协议帧 / chunk-0",
                    "score": 2.1,
                    "chunk_text": "AA 55 | LEN | TYPE | PAYLOAD | CRC16",
                }
            ],
        )
        self.assertIn("[引用] protocol.md / 协议帧 / chunk-0", prompt)

    def test_health_contains_index_status(self) -> None:
        health = self.service.health()
        self.assertIn("index_status", health)
        self.assertIn("is_stale", health["index_status"])

    def test_agent_plan_for_diagnosis(self) -> None:
        result = self.service.agent_plan("请诊断 device_id=1 最近 5 条数据")
        self.assertEqual(result["goal"], "diagnose_fault")
        self.assertEqual(result["steps"][0]["action"], "workflow_diagnose_fault")

    def test_agent_plan_for_diagnosis_and_report(self) -> None:
        result = self.service.agent_plan("请先诊断 device_id=1 再生成故障报告")
        self.assertEqual(result["goal"], "diagnose_and_generate_report")
        self.assertEqual(result["step_count"], 2)
        self.assertEqual(result["steps"][0]["action"], "workflow_diagnose_fault")
        self.assertEqual(result["steps"][1]["action"], "workflow_generate_report")
        self.assertIn("reason", result["steps"][0])
        self.assertIn("description", result["steps"][1])

    def test_agent_plan_for_ota_confirm(self) -> None:
        result = self.service.agent_plan("请确认 OTA 审批 approval_id=approval-123 approved_by=tester")
        self.assertEqual(result["goal"], "ota_confirm_request")
        self.assertEqual(result["step_count"], 1)
        self.assertEqual(result["steps"][0]["action"], "workflow_ota_request_confirm")
        self.assertEqual(result["steps"][0]["args"]["approval_id"], "approval-123")
        self.assertEqual(result["steps"][0]["args"]["approved_by"], "tester")

    def test_agent_plan_for_ota_batch(self) -> None:
        result = self.service.agent_plan("请对 device_ids=1,2,3 做批量 OTA 风险检查并创建审批 firmware_id=fw-1")
        self.assertEqual(result["goal"], "ota_batch_risk_then_request_create")
        self.assertEqual(result["step_count"], 2)
        self.assertEqual(result["steps"][0]["action"], "workflow_ota_batch_plan")
        self.assertEqual(result["steps"][1]["action"], "workflow_ota_batch_request_create")

    @patch("gateway_ai_assistant.service.AssistantService.workflow_diagnose_fault")
    def test_agent_execute_runs_diagnosis_workflow(self, mock_workflow_diagnose_fault) -> None:
        mock_workflow_diagnose_fault.return_value = {"ok": True, "overall_severity": "medium"}
        result = self.service.agent_execute("请诊断 device_id=1")
        self.assertTrue(result["ok"])
        self.assertTrue(result["session_id"].startswith("agent-"))
        self.assertEqual(result["plan"]["goal"], "diagnose_fault")
        self.assertEqual(result["execution"]["final_output"]["overall_severity"], "medium")
        self.assertEqual(result["execution_summary"]["final_status"], "ok")

    @patch("gateway_ai_assistant.service.AssistantService.workflow_generate_report")
    @patch("gateway_ai_assistant.service.AssistantService.workflow_diagnose_fault")
    def test_agent_execute_runs_multi_step_report_flow(
        self,
        mock_workflow_diagnose_fault,
        mock_workflow_generate_report,
    ) -> None:
        mock_workflow_diagnose_fault.return_value = {
            "ok": True,
            "overall_severity": "high",
            "finding_count": 2,
            "findings": [{"code": "device_offline"}],
            "tool_trace": [{"tool_name": "get_device_status"}],
        }
        mock_workflow_generate_report.return_value = {
            "ok": True,
            "report_id": "report-1",
            "report_type": "fault_ticket",
            "tool_trace": [{"tool_name": "get_system_events"}],
        }
        result = self.service.agent_execute("请先诊断 device_id=1 再生成故障报告")
        self.assertTrue(result["ok"])
        self.assertEqual(result["plan"]["goal"], "diagnose_and_generate_report")
        self.assertEqual(result["execution"]["step_count"], 2)
        self.assertEqual(result["execution"]["final_output"]["report_id"], "report-1")
        self.assertEqual(len(result["execution"]["tool_trace"]), 2)
        self.assertEqual(result["execution_summary"]["tool_call_count"], 2)
        kwargs = mock_workflow_generate_report.call_args.kwargs
        self.assertIn("diagnosis_result_override", kwargs)
        self.assertEqual(kwargs["diagnosis_result_override"]["overall_severity"], "high")

    @patch("gateway_ai_assistant.service.AssistantService.workflow_ota_request_confirm")
    def test_agent_execute_runs_ota_confirm_flow(self, mock_workflow_ota_request_confirm) -> None:
        mock_workflow_ota_request_confirm.return_value = {
            "ok": True,
            "approval_id": "approval-123",
            "status": "approved",
            "tool_trace": [{"tool_name": "create_ota_task"}],
        }
        result = self.service.agent_execute("请确认 OTA 审批 approval_id=approval-123 approved_by=tester")
        self.assertTrue(result["ok"])
        self.assertEqual(result["plan"]["goal"], "ota_confirm_request")
        self.assertEqual(result["execution"]["final_output"]["status"], "approved")
        kwargs = mock_workflow_ota_request_confirm.call_args.kwargs
        self.assertEqual(kwargs["approval_id"], "approval-123")
        self.assertEqual(kwargs["approved_by"], "tester")

    @patch("gateway_ai_assistant.service.AssistantService.workflow_ota_batch_request_create")
    @patch("gateway_ai_assistant.service.AssistantService.workflow_ota_batch_plan")
    def test_agent_execute_runs_ota_batch_flow(
        self,
        mock_workflow_ota_batch_plan,
        mock_workflow_ota_batch_request_create,
    ) -> None:
        mock_workflow_ota_batch_plan.return_value = {
            "ok": True,
            "batch_count": 2,
            "ready_device_ids": [1, 2],
            "tool_trace": [{"tool_name": "get_device_status"}],
        }
        mock_workflow_ota_batch_request_create.return_value = {
            "ok": True,
            "approval_id": "approval-batch-1",
            "status": "pending",
            "tool_trace": [{"tool_name": "create_batch_approval"}],
        }
        result = self.service.agent_execute("请对 device_ids=1,2,3 做批量 OTA 风险检查并创建审批 firmware_id=fw-1")
        self.assertTrue(result["ok"])
        self.assertEqual(result["plan"]["goal"], "ota_batch_risk_then_request_create")
        self.assertEqual(result["execution"]["step_count"], 2)
        self.assertEqual(result["execution"]["final_output"]["approval_id"], "approval-batch-1")

    @patch("gateway_ai_assistant.service.AssistantService.workflow_ota_batch_request_confirm")
    def test_approve_request_dispatches_batch_approval(self, mock_workflow_ota_batch_request_confirm) -> None:
        self.service.assistant_db.create_approval_request(
            ApprovalRequestRecord(
                approval_id="approval-batch-test",
                request_type="ota_batch_create_task",
                request_payload={"device_ids": [1, 2], "ready_device_ids": [1], "blocked_device_ids": [2], "firmware_id": "fw-1"},
                risk_summary="test",
                status="pending",
            )
        )
        mock_workflow_ota_batch_request_confirm.return_value = {"ok": True, "status": "approved"}
        result = self.service.approve_request("approval-batch-test", "tester")
        self.assertTrue(result["ok"])
        mock_workflow_ota_batch_request_confirm.assert_called_once()

    def test_cancel_approval_request_updates_status_and_note(self) -> None:
        self.service.assistant_db.create_approval_request(
            ApprovalRequestRecord(
                approval_id="approval-cancel-test",
                request_type="ota_create_task",
                request_payload={"device_id": 1, "firmware_id": "fw-1"},
                risk_summary="test",
                status="pending",
            )
        )
        result = self.service.cancel_approval_request("approval-cancel-test", "tester", "operator cancelled")
        self.assertTrue(result["ok"])
        self.assertEqual(result["status"], "cancelled")
        approval = self.service.get_approval("approval-cancel-test")
        self.assertTrue(approval["ok"])
        self.assertEqual(approval["approval"]["decision_note"], "operator cancelled")

    @patch("gateway_ai_assistant.service.RuntimeGateway.get_gateway_status")
    @patch("gateway_ai_assistant.service.OtaTools.create_ota_task")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    def test_retry_batch_failed_devices(self, mock_get_firmware_manifest, mock_create_ota_task, mock_get_gateway_status) -> None:
        mock_get_gateway_status.return_value = {"ok": True, "serial_available": True}
        self.service.assistant_db.create_approval_request(
            ApprovalRequestRecord(
                approval_id="approval-batch-retry-test",
                request_type="ota_batch_create_task",
                request_payload={
                    "device_ids": [1, 2],
                    "ready_device_ids": [1, 2],
                    "blocked_device_ids": [],
                    "firmware_id": "fw-1",
                    "transport": "serial",
                    "target": "127.0.0.1:19090",
                    "batch_count": 1,
                },
                risk_summary="test",
                status="partial_failed",
                result_payload={
                    "status": "partial_failed",
                    "created_count": 1,
                    "failed_count": 1,
                    "failed_device_ids": [2],
                    "per_device_results": [
                        {"device_id": 1, "result": {"ok": True}},
                        {"device_id": 2, "result": {"ok": False, "message": "send failed"}},
                    ],
                    "retry_history": [],
                },
            )
        )
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
        }
        mock_create_ota_task.return_value = {"ok": True, "task": {"task_uuid": "ota-retry-2"}}
        result = self.service.retry_batch_failed_devices("approval-batch-retry-test", "tester", "retry once")
        self.assertTrue(result["ok"])
        self.assertEqual(result["status"], "approved")
        self.assertEqual(result["remaining_failed_device_ids"], [])
        approval = self.service.get_approval("approval-batch-retry-test")
        self.assertTrue(approval["ok"])
        self.assertEqual(approval["approval"]["status"], "approved")
        self.assertEqual(approval["approval"]["result_payload"]["failed_device_ids"], [])
        self.assertEqual(len(approval["approval"]["result_payload"]["retry_history"]), 1)

    @patch("gateway_ai_assistant.service.RuntimeGateway.get_gateway_status")
    @patch("gateway_ai_assistant.service.OtaTools.create_ota_task")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    def test_batch_confirm_creates_batch_run_records(self, mock_get_firmware_manifest, mock_create_ota_task, mock_get_gateway_status) -> None:
        mock_get_gateway_status.return_value = {"ok": True, "serial_available": True}
        self.service.assistant_db.create_approval_request(
            ApprovalRequestRecord(
                approval_id="approval-batch-run-test",
                request_type="ota_batch_create_task",
                request_payload={
                    "device_ids": [1, 2, 3],
                    "ready_device_ids": [1, 2],
                    "blocked_device_ids": [3],
                    "firmware_id": "fw-1",
                    "transport": "serial",
                    "target": "127.0.0.1:19090",
                    "batch_size": 2,
                    "batch_count": 2,
                    "rollout_batches": [[1, 2], [3]],
                    "device_transport_overrides": {
                        "1": {"transport": "tcp_binary", "target": "127.0.0.1:19090"},
                        "2": {"transport": "serial", "target": "127.0.0.1:19090"},
                    },
                },
                risk_summary="test",
                status="pending",
            )
        )
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
        }
        mock_create_ota_task.side_effect = [
            {"ok": True, "task": {"task_uuid": "ota-b1", "state": "CREATED"}},
            {"ok": False, "message": "send failed"},
        ]
        result = self.service.workflow_ota_batch_request_confirm("approval-batch-run-test", "tester", "batch confirm")
        self.assertFalse(result["ok"])
        self.assertTrue(result["batch_run_id"].startswith("batch-run-"))
        run = self.service.get_batch_run(result["batch_run_id"])
        self.assertTrue(run["ok"])
        self.assertEqual(run["batch_run"]["status"], "partial_failed")
        self.assertEqual(len(run["items"]), 2)
        self.assertEqual(run["batch_run"]["summary_payload"]["blocked_device_ids"], [3])
        first_call = mock_create_ota_task.call_args_list[0]
        self.assertEqual(first_call.kwargs["transport"], "tcp_binary")

    @patch("gateway_ai_assistant.service.RuntimeGateway.get_ota_task_status")
    def test_refresh_batch_run_updates_item_statuses(self, mock_get_ota_task_status) -> None:
        self.service.assistant_db.create_ota_batch_run(
            OtaBatchRunRecord(
                batch_run_id="batch-run-refresh-test",
                approval_id="approval-batch-refresh-test",
                firmware_id="fw-1",
                transport="serial",
                target="127.0.0.1:19090",
                batch_size=2,
                batch_count=1,
                total_devices=2,
                status="approved",
                summary_payload={},
            )
        )
        self.service.assistant_db.create_ota_batch_run_items(
            [
                OtaBatchRunItemRecord(
                    batch_run_id="batch-run-refresh-test",
                    batch_index=1,
                    device_id=1,
                    status="task_created",
                    task_uuid="ota-1",
                ),
                OtaBatchRunItemRecord(
                    batch_run_id="batch-run-refresh-test",
                    batch_index=1,
                    device_id=2,
                    status="task_created",
                    task_uuid="ota-2",
                ),
            ]
        )
        mock_get_ota_task_status.side_effect = [
            {"ok": True, "task": {"task_uuid": "ota-1", "state": "SUCCESS", "last_error": ""}},
            {"ok": True, "task": {"task_uuid": "ota-2", "state": "FAILED", "last_error": "crc"}},
        ]
        result = self.service.refresh_batch_run("batch-run-refresh-test")
        self.assertTrue(result["ok"])
        self.assertEqual(result["batch_run"]["status"], "partial_failed")
        item_statuses = {int(item["device_id"]): item["status"] for item in result["items"]}
        self.assertEqual(item_statuses[1], "success")
        self.assertEqual(item_statuses[2], "failed")
        self.assertIn("sla", result["batch_run"]["summary_payload"])

    def test_pause_and_terminate_batch_run(self) -> None:
        self.service.assistant_db.create_ota_batch_run(
            OtaBatchRunRecord(
                batch_run_id="batch-run-control-test",
                approval_id="approval-batch-control-test",
                firmware_id="fw-1",
                transport="serial",
                target="127.0.0.1:19090",
                batch_size=2,
                batch_count=2,
                total_devices=3,
                status="waiting_next_batch",
                summary_payload={},
            )
        )
        self.service.assistant_db.create_ota_batch_run_items(
            [
                OtaBatchRunItemRecord(batch_run_id="batch-run-control-test", batch_index=1, device_id=1, status="success"),
                OtaBatchRunItemRecord(batch_run_id="batch-run-control-test", batch_index=2, device_id=2, status="queued"),
                OtaBatchRunItemRecord(batch_run_id="batch-run-control-test", batch_index=2, device_id=3, status="waiting_batch"),
            ]
        )
        pause_result = self.service.pause_batch_run("batch-run-control-test", "tester", "pause rollout")
        self.assertTrue(pause_result["ok"])
        run = self.service.get_batch_run("batch-run-control-test")
        self.assertEqual(run["batch_run"]["status"], "paused")

        terminate_result = self.service.terminate_batch_run("batch-run-control-test", "tester", "stop rollout")
        self.assertTrue(terminate_result["ok"])
        run = self.service.get_batch_run("batch-run-control-test")
        self.assertEqual(run["batch_run"]["status"], "terminated")
        self.assertTrue(all(str(item["status"]) == "cancelled" for item in run["items"] if int(item["batch_index"]) == 2))

    def test_ticket_lifecycle(self) -> None:
        create_result = self.service.create_ticket(title="T1", severity="high", device_id=1, description="desc")
        self.assertTrue(create_result["ok"])
        ticket_id = create_result["ticket_id"]
        list_result = self.service.list_tickets(limit=10, status="open", severity="high")
        self.assertEqual(list_result["count"], 1)
        assign_result = self.service.assign_ticket(ticket_id, "alice")
        self.assertEqual(assign_result["ticket"]["assignee"], "alice")
        close_result = self.service.close_ticket(ticket_id)
        self.assertEqual(close_result["ticket"]["status"], "closed")
        reopen_result = self.service.reopen_ticket(ticket_id)
        self.assertEqual(reopen_result["ticket"]["status"], "open")

    @patch("gateway_ai_assistant.service.RuntimeGateway.get_gateway_status")
    @patch("gateway_ai_assistant.service.OtaTools.create_ota_task")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    def test_start_next_batch_runs_only_pending_batch(self, mock_get_firmware_manifest, mock_create_ota_task, mock_get_gateway_status) -> None:
        mock_get_gateway_status.return_value = {"ok": True, "serial_available": True}
        self.service.assistant_db.create_approval_request(
            ApprovalRequestRecord(
                approval_id="approval-batch-next-test",
                request_type="ota_batch_create_task",
                request_payload={
                    "device_ids": [1, 2, 3],
                    "ready_device_ids": [1, 2, 3],
                    "blocked_device_ids": [],
                    "firmware_id": "fw-1",
                    "transport": "serial",
                    "target": "127.0.0.1:19090",
                    "batch_size": 2,
                    "batch_count": 2,
                    "rollout_batches": [[1, 2], [3]],
                },
                risk_summary="test",
                status="pending",
            )
        )
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
        }
        mock_create_ota_task.side_effect = [
            {"ok": True, "task": {"task_uuid": "ota-b1-1", "state": "CREATED"}},
            {"ok": True, "task": {"task_uuid": "ota-b1-2", "state": "CREATED"}},
            {"ok": True, "task": {"task_uuid": "ota-b2-1", "state": "CREATED"}},
        ]
        confirm_result = self.service.workflow_ota_batch_request_confirm("approval-batch-next-test", "tester", "start batch 1")
        self.assertTrue(confirm_result["ok"])
        self.assertEqual(confirm_result["started_batch_index"], 1)
        self.assertEqual(mock_create_ota_task.call_count, 2)
        batch_run_id = confirm_result["batch_run_id"]

        run_items = self.service.assistant_db.list_ota_batch_run_items(batch_run_id)
        for item in run_items:
            if int(item["device_id"]) in {1, 2}:
                self.service.assistant_db.update_ota_batch_run_item(
                    int(item["id"]),
                    "success",
                    task_uuid=str(item["task_uuid"]),
                    last_checked_at="done",
                )
        next_result = self.service.start_next_batch(batch_run_id, "tester", "start batch 2")
        self.assertTrue(next_result["ok"])
        self.assertEqual(next_result["started_batch_index"], 2)
        self.assertEqual(mock_create_ota_task.call_count, 3)

    @patch("gateway_ai_assistant.service.RuntimeGateway.get_gateway_status")
    @patch("gateway_ai_assistant.service.OtaTools.create_ota_task")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    def test_start_next_batch_blocks_when_serial_transport_unavailable(
        self,
        mock_get_firmware_manifest,
        mock_create_ota_task,
        mock_get_gateway_status,
    ) -> None:
        mock_get_gateway_status.return_value = {"ok": True, "serial_available": False}
        self.service.assistant_db.create_ota_batch_run(
            OtaBatchRunRecord(
                batch_run_id="batch-run-serial-guard-test",
                approval_id="approval-batch-serial-guard-test",
                firmware_id="fw-1",
                transport="serial",
                target="127.0.0.1:19090",
                batch_size=1,
                batch_count=1,
                total_devices=1,
                status="pending_batch_start",
                summary_payload={},
            )
        )
        self.service.assistant_db.create_ota_batch_run_items(
            [
                OtaBatchRunItemRecord(
                    batch_run_id="batch-run-serial-guard-test",
                    batch_index=1,
                    device_id=1,
                    status="queued",
                )
            ]
        )
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
        }

        result = self.service.start_next_batch("batch-run-serial-guard-test", "tester", "guard check")

        self.assertFalse(result["ok"])
        self.assertEqual(result["per_device_results"][0]["result"]["reason"], "gateway_transport_unavailable")
        mock_create_ota_task.assert_not_called()

    @patch("gateway_ai_assistant.service.AssistantService.workflow_ota_request_confirm")
    def test_agent_execute_failure_contains_step_status_and_reason(self, mock_workflow_ota_request_confirm) -> None:
        mock_workflow_ota_request_confirm.return_value = {
            "ok": False,
            "message": "Approval request was not found.",
            "tool_trace": [],
        }
        result = self.service.agent_execute("请确认 OTA 审批 approval_id=approval-404 approved_by=tester")
        self.assertFalse(result["ok"])
        self.assertEqual(result["execution_summary"]["final_status"], "error")
        self.assertIn("Approval request was not found.", result["execution_summary"]["failure_reason"])
        step = result["execution"]["step_results"][0]
        self.assertEqual(step["status"], "error")
        self.assertIn("Approval request was not found.", step["failure_reason"])
        self.assertTrue(step["started_at"])
        self.assertTrue(step["finished_at"])

    @patch("gateway_ai_assistant.service.AssistantService.workflow_diagnose_fault")
    def test_agent_session_contains_action_audit(self, mock_workflow_diagnose_fault) -> None:
        mock_workflow_diagnose_fault.return_value = {"ok": True, "overall_severity": "low", "tool_trace": []}
        result = self.service.agent_execute("请诊断 device_id=1")
        session = self.service.get_agent_session(result["session_id"])
        self.assertTrue(session["ok"])
        self.assertEqual(session["action_count"], 1)
        self.assertEqual(session["action_audits"][0]["action_type"], "agent_execute")

    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_system_events")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_sensor_history")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_agent_session_includes_workflow_tool_calls(
        self,
        mock_get_device_status,
        mock_get_sensor_history,
        mock_get_system_events,
        mock_list_recent_ota_tasks,
    ) -> None:
        mock_get_device_status.return_value = {"ok": True, "status": "online", "tool_trace": []}
        mock_get_sensor_history.return_value = {"ok": True, "count": 1, "records": [{"device_id": 1}]}
        mock_get_system_events.return_value = {"ok": True, "count": 1, "records": [{"event_type": "device_online", "device_id": 1}]}
        mock_list_recent_ota_tasks.return_value = {"ok": True, "count": 0, "tasks": []}
        result = self.service.agent_execute("请诊断 device_id=1")
        session = self.service.get_agent_session(result["session_id"])
        self.assertTrue(session["ok"])
        self.assertGreaterEqual(session["tool_call_count"], 4)
        self.assertTrue(all(item["session_id"] == result["session_id"] for item in session["tool_calls"]))

    @patch("gateway_ai_assistant.service.AssistantService.start_next_batch")
    def test_agent_execute_runs_start_next_batch(self, mock_start_next_batch) -> None:
        mock_start_next_batch.return_value = {
            "ok": True,
            "batch_run_id": "batch-run-1",
            "started_batch_index": 2,
            "tool_trace": [{"tool_name": "create_ota_task"}],
        }
        result = self.service.agent_execute("请启动下一批 OTA batch_run_id=batch-run-1 operator=tester")
        self.assertTrue(result["ok"])
        self.assertEqual(result["plan"]["goal"], "ota_batch_start_next")
        mock_start_next_batch.assert_called_once()

    @patch("gateway_ai_assistant.service.AssistantService.pause_batch_run")
    def test_agent_execute_runs_pause_batch(self, mock_pause_batch_run) -> None:
        mock_pause_batch_run.return_value = {"ok": True, "batch_run_id": "batch-run-1", "status": "paused"}
        result = self.service.agent_execute("请暂停批量 OTA batch_run_id=batch-run-1 operator=tester")
        self.assertTrue(result["ok"])
        self.assertEqual(result["plan"]["goal"], "ota_batch_pause")
        mock_pause_batch_run.assert_called_once()

    def test_summarize_runtime_result_inferred(self) -> None:
        facts = self.service._summarize_runtime_result(
            {
                "ok": True,
                "status": "offline",
                "status_source": "inferred_from_recent_events",
                "last_seen_readable_time": "2026-01-01T00:00:00+00:00",
            }
        )
        self.assertTrue(any("inferred_from_recent_events" in item for item in facts))


if __name__ == "__main__":
    unittest.main()
