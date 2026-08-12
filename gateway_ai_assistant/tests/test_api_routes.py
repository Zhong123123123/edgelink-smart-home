from __future__ import annotations

import json
import unittest
from unittest.mock import patch

from fastapi.testclient import TestClient

from gateway_ai_assistant.app import app


class ApiRouteTests(unittest.TestCase):
    def setUp(self) -> None:
        self.client = TestClient(app)

    def test_health_endpoint_still_available(self) -> None:
        resp = self.client.get("/assistant/health")
        self.assertEqual(resp.status_code, 200)
        self.assertIn("status", resp.json())

    @patch("gateway_ai_assistant.app.service.chat")
    def test_chat_endpoint_uses_router(self, mock_chat) -> None:
        mock_chat.return_value = {
            "route_type": "Runtime",
            "route_reason": "matched runtime query keywords",
            "answer": "ok",
            "llm_mode": "mock",
            "response_source": "runtime:http",
            "tool_trace": [],
        }
        resp = self.client.post("/assistant/chat", json={"question": "sensor-01 当前状态正常吗？"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["route_type"], "Runtime")

    @patch("gateway_ai_assistant.app.service.agent_plan")
    def test_agent_plan_endpoint(self, mock_agent_plan) -> None:
        mock_agent_plan.return_value = {"ok": True, "goal": "diagnose_and_generate_report", "steps": [{}, {}], "plan_summary": "1.a -> 2.b"}
        resp = self.client.post("/assistant/agent/plan", json={"question": "请诊断 device_id=1"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["goal"], "diagnose_and_generate_report")
        self.assertIn("plan_summary", resp.json())

    @patch("gateway_ai_assistant.app.service.agent_execute")
    def test_agent_execute_endpoint(self, mock_agent_execute) -> None:
        mock_agent_execute.return_value = {
            "ok": True,
            "session_id": "agent-1",
            "plan": {"goal": "chat"},
            "execution": {"ok": True},
            "execution_summary": {"final_status": "ok"},
        }
        resp = self.client.post("/assistant/agent/execute", json={"question": "DEVICE_001 当前状态正常吗？"})
        self.assertEqual(resp.status_code, 200)
        self.assertTrue(resp.json()["ok"])

    @patch("gateway_ai_assistant.app.service.chat_agent")
    def test_agent_chat_endpoint(self, mock_chat_agent) -> None:
        mock_chat_agent.return_value = {
            "ok": True,
            "session_id": "agent-chat-1",
            "answer": "设备在线",
            "tool_trace": [{"tool": "get_device_status", "args": {"device_ref": "DEVICE_001"}, "step": 1}],
            "tool_call_count": 1,
            "interrupted": False,
            "mode": "react_agent",
        }
        resp = self.client.post(
            "/assistant/agent/chat",
            json={"question": "DEVICE_001 当前状态正常吗？", "session_id": "agent-chat-1"},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["mode"], "react_agent")
        mock_chat_agent.assert_called_once_with("DEVICE_001 当前状态正常吗？", "agent-chat-1")

    @patch("gateway_ai_assistant.app.service.chat_agent")
    def test_agent_chat_endpoint_returns_503_when_agent_unavailable(self, mock_chat_agent) -> None:
        mock_chat_agent.side_effect = RuntimeError("LLM_API_KEY not configured. Agent mode requires a valid API key.")
        resp = self.client.post(
            "/assistant/agent/chat",
            json={"question": "DEVICE_001 当前状态正常吗？", "session_id": ""},
        )
        self.assertEqual(resp.status_code, 503)
        self.assertIn("LLM_API_KEY not configured", resp.json()["detail"])

    @patch("gateway_ai_assistant.app.service.chat_agent_stream")
    def test_agent_stream_endpoint(self, mock_chat_agent_stream) -> None:
        async def fake_stream():
            yield {"type": "token", "content": "设备"}
            yield {"type": "final_answer", "content": "设备在线"}
            yield {"type": "done", "session_id": "agent-stream-1"}

        mock_chat_agent_stream.return_value = fake_stream()
        with self.client.stream(
            "POST",
            "/assistant/agent/stream",
            json={"question": "DEVICE_001 当前状态正常吗？", "session_id": "agent-stream-1"},
        ) as resp:
            body = "".join(chunk.decode("utf-8") if isinstance(chunk, bytes) else chunk for chunk in resp.iter_text())
        self.assertEqual(resp.status_code, 200)
        self.assertIn("text/event-stream", resp.headers["content-type"])
        events = [line[len("data: ") :] for line in body.splitlines() if line.startswith("data: ")]
        parsed = [json.loads(item) for item in events]
        self.assertEqual(parsed[0]["type"], "token")
        self.assertEqual(parsed[1]["type"], "final_answer")
        self.assertEqual(parsed[2]["type"], "done")
        mock_chat_agent_stream.assert_called_once_with("DEVICE_001 当前状态正常吗？", "agent-stream-1")

    @patch("gateway_ai_assistant.app.service.chat_agent_stream")
    def test_agent_stream_endpoint_returns_503_when_agent_unavailable(self, mock_chat_agent_stream) -> None:
        mock_chat_agent_stream.side_effect = RuntimeError("LLM_API_KEY not configured. Agent mode requires a valid API key.")
        resp = self.client.post(
            "/assistant/agent/stream",
            json={"question": "DEVICE_001 当前状态正常吗？", "session_id": ""},
        )
        self.assertEqual(resp.status_code, 503)
        self.assertIn("LLM_API_KEY not configured", resp.json()["detail"])

    @patch("gateway_ai_assistant.app.service.chat_agent_resume")
    def test_agent_resume_endpoint(self, mock_chat_agent_resume) -> None:
        mock_chat_agent_resume.return_value = {
            "ok": True,
            "session_id": "agent-chat-1",
            "answer": "已继续执行",
            "mode": "react_agent_resumed",
        }
        resp = self.client.post(
            "/assistant/agent/resume",
            json={"session_id": "agent-chat-1", "decision": "reject", "edit_args": {"approved_by": "tester"}},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["mode"], "react_agent_resumed")
        mock_chat_agent_resume.assert_called_once_with(
            "agent-chat-1",
            decision="reject",
            edit_args={"approved_by": "tester"},
        )

    @patch("gateway_ai_assistant.app.service.chat_agent_resume")
    def test_agent_resume_endpoint_returns_409_without_pending_interrupt(self, mock_chat_agent_resume) -> None:
        mock_chat_agent_resume.return_value = {
            "ok": False,
            "session_id": "agent-chat-1",
            "error": "No pending approval interrupt for this session.",
        }
        resp = self.client.post(
            "/assistant/agent/resume",
            json={"session_id": "agent-chat-1", "decision": "approve"},
        )
        self.assertEqual(resp.status_code, 409)
        self.assertIn("No pending approval interrupt", resp.json()["detail"])

    def test_agent_resume_endpoint_requires_session_id(self) -> None:
        resp = self.client.post(
            "/assistant/agent/resume",
            json={"session_id": "", "decision": "approve"},
        )
        self.assertEqual(resp.status_code, 400)
        self.assertIn("session_id is required", resp.json()["detail"])

    @patch("gateway_ai_assistant.app.service.get_agent_session")
    def test_agent_session_endpoint(self, mock_get_agent_session) -> None:
        mock_get_agent_session.return_value = {"ok": True, "session_id": "agent-1", "action_count": 1, "tool_call_count": 0}
        resp = self.client.get("/assistant/agent/sessions/agent-1")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["session_id"], "agent-1")

    @patch("gateway_ai_assistant.app.service.export_agent_session_replay")
    def test_agent_session_replay_endpoint(self, mock_export_agent_session_replay) -> None:
        mock_export_agent_session_replay.return_value = {"ok": True, "session_id": "agent-1", "timeline_count": 2}
        resp = self.client.get("/assistant/agent/sessions/agent-1/replay")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["timeline_count"], 2)

    @patch("gateway_ai_assistant.app.service.get_agent_memory_snapshot")
    def test_agent_session_memory_endpoint(self, mock_get_agent_memory_snapshot) -> None:
        mock_get_agent_memory_snapshot.return_value = {"ok": True, "session_id": "agent-1", "long_term_count": 1}
        resp = self.client.get("/assistant/agent/sessions/agent-1/memory")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["long_term_count"], 1)

    @patch("gateway_ai_assistant.app.service.list_agent_memories")
    def test_agent_memories_endpoint(self, mock_list_agent_memories) -> None:
        mock_list_agent_memories.return_value = {"ok": True, "session_count": 1, "long_term_count": 2}
        resp = self.client.get("/assistant/agent/memories")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["long_term_count"], 2)

    @patch("gateway_ai_assistant.app.service.list_memory_candidates")
    def test_agent_memory_candidates_endpoint(self, mock_list_memory_candidates) -> None:
        mock_list_memory_candidates.return_value = {"ok": True, "items": [{"candidate_id": "memcand-1"}], "count": 1}
        resp = self.client.get("/assistant/agent/memory_candidates")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.list_memory_audits")
    def test_agent_memory_audits_endpoint(self, mock_list_memory_audits) -> None:
        mock_list_memory_audits.return_value = {"ok": True, "items": [{"action_type": "candidate_created"}], "count": 1}
        resp = self.client.get("/assistant/agent/memory_audits")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.ingest_realtime_event")
    def test_ingest_realtime_event_endpoint(self, mock_ingest_realtime_event) -> None:
        mock_ingest_realtime_event.return_value = {"ok": True, "event": {"event_id": "evt-1"}, "alert_count": 1, "decision_count": 1}
        resp = self.client.post(
            "/assistant/realtime/events",
            json={"event_type": "device_offline", "source": "api", "severity": "high", "device_id": 7, "payload": {"last_seen_sec": 100}},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["alert_count"], 1)

    @patch("gateway_ai_assistant.app.service.list_realtime_events")
    def test_list_realtime_events_endpoint(self, mock_list_realtime_events) -> None:
        mock_list_realtime_events.return_value = {"ok": True, "items": [{"event_id": "evt-1"}], "count": 1}
        resp = self.client.get("/assistant/realtime/events")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.list_alert_records")
    def test_list_alerts_endpoint(self, mock_list_alert_records) -> None:
        mock_list_alert_records.return_value = {"ok": True, "items": [{"alert_id": "alert-1"}], "count": 1}
        resp = self.client.get("/assistant/alerts")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.suppress_alert")
    def test_suppress_alert_endpoint(self, mock_suppress_alert) -> None:
        mock_suppress_alert.return_value = {"ok": True, "alert": {"alert_id": "alert-1", "status": "suppressed"}}
        resp = self.client.post("/assistant/alerts/alert-1/suppress", json={"actor": "operator", "suppress_minutes": 30})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["alert"]["status"], "suppressed")

    @patch("gateway_ai_assistant.app.service.list_notification_records")
    def test_list_notifications_endpoint(self, mock_list_notification_records) -> None:
        mock_list_notification_records.return_value = {"ok": True, "items": [{"notification_id": "notif-1"}], "count": 1}
        resp = self.client.get("/assistant/alerts/notifications")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.acknowledge_alert")
    def test_acknowledge_alert_endpoint(self, mock_acknowledge_alert) -> None:
        mock_acknowledge_alert.return_value = {"ok": True, "alert": {"alert_id": "alert-1", "status": "acknowledged"}}
        resp = self.client.post("/assistant/alerts/alert-1/ack", json={"actor": "operator"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["alert"]["status"], "acknowledged")

    @patch("gateway_ai_assistant.app.service.resolve_alert")
    def test_resolve_alert_endpoint(self, mock_resolve_alert) -> None:
        mock_resolve_alert.return_value = {"ok": True, "alert": {"alert_id": "alert-1", "status": "resolved"}}
        resp = self.client.post("/assistant/alerts/alert-1/resolve", json={"actor": "operator"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["alert"]["status"], "resolved")

    @patch("gateway_ai_assistant.app.service.list_decision_records")
    def test_list_decisions_endpoint(self, mock_list_decision_records) -> None:
        mock_list_decision_records.return_value = {"ok": True, "items": [{"decision_id": "decision-1"}], "count": 1}
        resp = self.client.get("/assistant/decisions")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.list_decision_policies")
    def test_list_decision_policies_endpoint(self, mock_list_decision_policies) -> None:
        mock_list_decision_policies.return_value = {"ok": True, "items": [{"policy_id": "policy-1", "version": "v1"}], "count": 1}
        resp = self.client.get("/assistant/decisions/policies")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["items"][0]["version"], "v1")

    @patch("gateway_ai_assistant.app.service.reload_decision_policies")
    def test_reload_decision_policies_endpoint(self, mock_reload_decision_policies) -> None:
        mock_reload_decision_policies.return_value = {"ok": True, "items": [{"policy_id": "policy-1", "version": "v2"}], "count": 1}
        resp = self.client.post("/assistant/decisions/policies/reload")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["items"][0]["version"], "v2")

    @patch("gateway_ai_assistant.app.service.list_execution_records")
    def test_list_executions_endpoint(self, mock_list_execution_records) -> None:
        mock_list_execution_records.return_value = {"ok": True, "items": [{"execution_id": "exec-1"}], "count": 1}
        resp = self.client.get("/assistant/decisions/executions")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.list_rollback_records")
    def test_list_rollbacks_endpoint(self, mock_list_rollback_records) -> None:
        mock_list_rollback_records.return_value = {"ok": True, "items": [{"rollback_id": "rollback-1"}], "count": 1}
        resp = self.client.get("/assistant/decisions/rollbacks")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.execute_decision")
    def test_execute_decision_endpoint(self, mock_execute_decision) -> None:
        mock_execute_decision.return_value = {"ok": True, "decision": {"decision_id": "decision-1", "status": "executed"}}
        resp = self.client.post("/assistant/decisions/decision-1/execute", json={"actor": "admin", "decision_note": "go"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["decision"]["status"], "executed")

    @patch("gateway_ai_assistant.app.service.rollback_decision_execution")
    def test_rollback_decision_endpoint(self, mock_rollback_decision_execution) -> None:
        mock_rollback_decision_execution.return_value = {"ok": True, "decision": {"decision_id": "decision-1", "status": "rolled_back"}}
        resp = self.client.post("/assistant/decisions/decision-1/rollback", json={"actor": "approver", "decision_note": "revert"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["decision"]["status"], "rolled_back")

    @patch("gateway_ai_assistant.app.service.decision_metrics")
    def test_decision_metrics_endpoint(self, mock_decision_metrics) -> None:
        mock_decision_metrics.return_value = {"ok": True, "metrics": {"total_alerts": 1, "total_decisions": 1}}
        resp = self.client.get("/assistant/decisions/metrics")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["metrics"]["total_alerts"], 1)

    @patch("gateway_ai_assistant.app.service.snapshot_decision_metrics")
    def test_snapshot_decision_metrics_endpoint(self, mock_snapshot_decision_metrics) -> None:
        mock_snapshot_decision_metrics.return_value = {"ok": True, "count": 2, "items": [{"metric_name": "total_alerts"}]}
        resp = self.client.post("/assistant/decisions/metrics/snapshot")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 2)

    @patch("gateway_ai_assistant.app.service.list_decision_metric_snapshots")
    def test_decision_metric_history_endpoint(self, mock_list_decision_metric_snapshots) -> None:
        mock_list_decision_metric_snapshots.return_value = {"ok": True, "count": 1, "items": [{"metric_name": "blocked_rate"}]}
        resp = self.client.get("/assistant/decisions/metrics/history")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.export_decision_metrics_csv")
    def test_export_decision_metrics_endpoint(self, mock_export_decision_metrics_csv) -> None:
        mock_export_decision_metrics_csv.return_value = {"ok": True, "csv": "metric_name,metric_value\nx,1\n", "count": 1}
        resp = self.client.get("/assistant/decisions/metrics/export")
        self.assertEqual(resp.status_code, 200)
        self.assertIn("metric_name,metric_value", resp.json()["csv"])

    @patch("gateway_ai_assistant.app.service.upsert_agent_session_memory")
    def test_upsert_agent_session_memory_endpoint(self, mock_upsert_agent_session_memory) -> None:
        mock_upsert_agent_session_memory.return_value = {"ok": True, "session_memory": {"session_id": "agent-1"}}
        resp = self.client.post(
            "/assistant/agent/sessions/agent-1/memory",
            json={"summary": "manual", "key_facts": {"device_id": 1}, "last_question": "q", "last_answer": "a"},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["session_memory"]["session_id"], "agent-1")

    @patch("gateway_ai_assistant.app.service.delete_agent_session_memory")
    def test_delete_agent_session_memory_endpoint(self, mock_delete_agent_session_memory) -> None:
        mock_delete_agent_session_memory.return_value = {"ok": True, "deleted": True}
        resp = self.client.delete("/assistant/agent/sessions/agent-1/memory")
        self.assertEqual(resp.status_code, 200)
        self.assertTrue(resp.json()["deleted"])

    @patch("gateway_ai_assistant.app.service.create_or_update_long_term_memory")
    def test_create_long_term_memory_endpoint(self, mock_create_or_update_long_term_memory) -> None:
        mock_create_or_update_long_term_memory.return_value = {"ok": True, "memory": {"id": 1, "memory_key": "preferred_transport"}}
        resp = self.client.post(
            "/assistant/agent/memories/long_term",
            json={"memory_key": "preferred_transport", "content": "Prefer serial.", "status": "active", "actor": "approver"},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["memory"]["memory_key"], "preferred_transport")

    @patch("gateway_ai_assistant.app.service.create_or_update_long_term_memory")
    def test_create_long_term_memory_endpoint_returns_403_for_permission_error(self, mock_create_or_update_long_term_memory) -> None:
        mock_create_or_update_long_term_memory.return_value = {"ok": False, "message": "Actor 'operator' requires role>=approver but resolved as operator."}
        resp = self.client.post(
            "/assistant/agent/memories/long_term",
            json={"memory_key": "preferred_transport", "content": "Prefer serial.", "status": "active", "actor": "operator"},
        )
        self.assertEqual(resp.status_code, 403)

    @patch("gateway_ai_assistant.app.service.update_long_term_memory")
    def test_update_long_term_memory_endpoint(self, mock_update_long_term_memory) -> None:
        mock_update_long_term_memory.return_value = {"ok": True, "memory": {"id": 1, "status": "inactive"}}
        resp = self.client.post(
            "/assistant/agent/memories/long_term/1",
            json={"status": "inactive", "actor": "approver"},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["memory"]["status"], "inactive")

    @patch("gateway_ai_assistant.app.service.delete_long_term_memory")
    def test_delete_long_term_memory_endpoint(self, mock_delete_long_term_memory) -> None:
        mock_delete_long_term_memory.return_value = {"ok": True, "deleted": True}
        resp = self.client.delete("/assistant/agent/memories/long_term/1?actor=admin")
        self.assertEqual(resp.status_code, 200)
        self.assertTrue(resp.json()["deleted"])

    @patch("gateway_ai_assistant.app.service.approve_memory_candidate")
    def test_approve_memory_candidate_endpoint(self, mock_approve_memory_candidate) -> None:
        mock_approve_memory_candidate.return_value = {"ok": True, "candidate": {"candidate_id": "memcand-1", "status": "approved"}}
        resp = self.client.post(
            "/assistant/agent/memory_candidates/memcand-1/approve",
            json={"reviewed_by": "tester", "decision_note": "ok"},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["candidate"]["status"], "approved")

    @patch("gateway_ai_assistant.app.service.reject_memory_candidate")
    def test_reject_memory_candidate_endpoint(self, mock_reject_memory_candidate) -> None:
        mock_reject_memory_candidate.return_value = {"ok": True, "candidate": {"candidate_id": "memcand-1", "status": "rejected"}}
        resp = self.client.post(
            "/assistant/agent/memory_candidates/memcand-1/reject",
            json={"reviewed_by": "tester", "decision_note": "noise"},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["candidate"]["status"], "rejected")

    @patch("gateway_ai_assistant.app.service.run_agent_eval_suite")
    def test_agent_eval_run_endpoint(self, mock_run_agent_eval_suite) -> None:
        mock_run_agent_eval_suite.return_value = {
            "ok": True,
            "run_id": "eval-1",
            "summary": {"total": 1, "passed": 1},
            "comparison": {"has_baseline": False, "regressions": [], "improvements": [], "unchanged": 1},
        }
        resp = self.client.post("/assistant/evals/agent/run", json={"cases": []})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["run_id"], "eval-1")

    @patch("gateway_ai_assistant.app.service.list_agent_eval_runs")
    def test_agent_eval_runs_endpoint(self, mock_list_agent_eval_runs) -> None:
        mock_list_agent_eval_runs.return_value = {"ok": True, "runs": [{"run_id": "eval-1"}], "count": 1}
        resp = self.client.get("/assistant/evals/agent/runs")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.get_agent_eval_run")
    def test_agent_eval_run_detail_endpoint(self, mock_get_agent_eval_run) -> None:
        mock_get_agent_eval_run.return_value = {"ok": True, "run": {"run_id": "eval-1"}, "cases": [], "case_count": 0}
        resp = self.client.get("/assistant/evals/agent/runs/eval-1")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["run"]["run_id"], "eval-1")

    @patch("gateway_ai_assistant.app.service.get_agent_eval_run")
    def test_agent_eval_run_detail_endpoint_returns_404_when_missing(self, mock_get_agent_eval_run) -> None:
        mock_get_agent_eval_run.return_value = {"ok": False, "message": "Eval run was not found."}
        resp = self.client.get("/assistant/evals/agent/runs/eval-missing")
        self.assertEqual(resp.status_code, 404)
        self.assertIn("Eval run was not found", resp.json()["detail"])

    @patch("gateway_ai_assistant.app.service.workflow_ota_risk_check")
    def test_workflow_ota_risk_check_endpoint(self, mock_workflow_ota_risk_check) -> None:
        mock_workflow_ota_risk_check.return_value = {
            "ok": True,
            "risk_level": "low",
            "checks": [],
            "tool_trace": [],
        }
        resp = self.client.post(
            "/assistant/workflow/ota_risk_check",
            json={"device_id": 1, "firmware_id": "fw-1"},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["risk_level"], "low")

    @patch("gateway_ai_assistant.app.service.workflow_ota_request_create")
    def test_workflow_ota_request_create_endpoint(self, mock_workflow_ota_request_create) -> None:
        mock_workflow_ota_request_create.return_value = {
            "ok": True,
            "approval_id": "approval-1",
            "status": "pending",
        }
        resp = self.client.post(
            "/assistant/workflow/ota_request_create",
            json={"device_id": 1, "firmware_id": "fw-1", "transport": "serial", "target": "127.0.0.1:19090"},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"], "pending")

    @patch("gateway_ai_assistant.app.service.workflow_ota_batch_plan")
    def test_workflow_ota_batch_plan_endpoint(self, mock_workflow_ota_batch_plan) -> None:
        mock_workflow_ota_batch_plan.return_value = {
            "ok": True,
            "batch_count": 2,
            "ready_device_ids": [1, 2],
        }
        resp = self.client.post(
            "/assistant/workflow/ota_batch_plan",
            json={"device_ids": [1, 2, 3], "firmware_id": "fw-1", "transport": "serial", "target": "127.0.0.1:19090", "batch_size": 2},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["batch_count"], 2)

    @patch("gateway_ai_assistant.app.service.workflow_ota_batch_request_create")
    def test_workflow_ota_batch_request_create_endpoint(self, mock_workflow_ota_batch_request_create) -> None:
        mock_workflow_ota_batch_request_create.return_value = {
            "ok": True,
            "approval_id": "approval-batch-1",
            "status": "pending",
        }
        resp = self.client.post(
            "/assistant/workflow/ota_batch_request_create",
            json={"device_ids": [1, 2, 3], "firmware_id": "fw-1", "transport": "serial", "target": "127.0.0.1:19090", "batch_size": 2},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"], "pending")

    @patch("gateway_ai_assistant.app.service.workflow_ota_request_confirm")
    def test_workflow_ota_request_confirm_endpoint(self, mock_workflow_ota_request_confirm) -> None:
        mock_workflow_ota_request_confirm.return_value = {
            "ok": True,
            "approval_id": "approval-1",
            "status": "approved",
        }
        resp = self.client.post(
            "/assistant/workflow/ota_request_confirm",
            json={"approval_id": "approval-1", "approved_by": "tester"},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"], "approved")

    @patch("gateway_ai_assistant.app.service.workflow_generate_report")
    def test_workflow_report_generate_endpoint(self, mock_workflow_generate_report) -> None:
        mock_workflow_generate_report.return_value = {
            "ok": True,
            "report_type": "fault_ticket",
            "title": "DEVICE_001 Fault Ticket",
            "markdown": "# DEVICE_001 Fault Ticket",
        }
        resp = self.client.post(
            "/assistant/workflow/report_generate",
            json={"report_type": "fault_ticket", "device_id": 1, "task_id": "", "limit": 5},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["report_type"], "fault_ticket")

    @patch("gateway_ai_assistant.app.service.workflow_diagnose_fault")
    def test_workflow_diagnose_fault_endpoint(self, mock_workflow_diagnose_fault) -> None:
        mock_workflow_diagnose_fault.return_value = {
            "ok": True,
            "overall_severity": "high",
            "finding_count": 2,
            "findings": [],
        }
        resp = self.client.post(
            "/assistant/workflow/diagnose_fault",
            json={"device_id": 1, "limit": 10},
        )
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["overall_severity"], "high")

    @patch("gateway_ai_assistant.app.service.list_approval_requests")
    def test_approval_list_endpoint(self, mock_list_approval_requests) -> None:
        mock_list_approval_requests.return_value = {"ok": True, "count": 1, "items": [{"approval_id": "approval-1"}]}
        resp = self.client.get("/assistant/approvals", params={"limit": 5, "status": "pending"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.approve_request")
    def test_approval_approve_endpoint(self, mock_workflow_ota_request_confirm) -> None:
        mock_workflow_ota_request_confirm.return_value = {"ok": True, "approval_id": "approval-1", "status": "approved"}
        resp = self.client.post("/assistant/approvals/approval-1/approve", json={"operator": "tester", "note": "approved"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"], "approved")

    @patch("gateway_ai_assistant.app.service.reject_approval_request")
    def test_approval_reject_endpoint(self, mock_reject_approval_request) -> None:
        mock_reject_approval_request.return_value = {"ok": True, "approval_id": "approval-1", "status": "rejected"}
        resp = self.client.post("/assistant/approvals/approval-1/reject", json={"operator": "tester", "note": "rejected"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"], "rejected")

    @patch("gateway_ai_assistant.app.service.cancel_approval_request")
    def test_approval_cancel_endpoint(self, mock_cancel_approval_request) -> None:
        mock_cancel_approval_request.return_value = {"ok": True, "approval_id": "approval-1", "status": "cancelled"}
        resp = self.client.post("/assistant/approvals/approval-1/cancel", json={"operator": "tester", "note": "cancelled"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"], "cancelled")

    @patch("gateway_ai_assistant.app.service.retry_batch_failed_devices")
    def test_approval_retry_failed_endpoint(self, mock_retry_batch_failed_devices) -> None:
        mock_retry_batch_failed_devices.return_value = {"ok": True, "approval_id": "approval-1", "status": "approved"}
        resp = self.client.post("/assistant/approvals/approval-1/retry_failed", json={"operator": "tester", "note": "retry"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"], "approved")

    @patch("gateway_ai_assistant.app.service.list_tool_call_logs")
    def test_audit_tool_calls_endpoint(self, mock_list_tool_call_logs) -> None:
        mock_list_tool_call_logs.return_value = {"ok": True, "count": 1, "items": [{"tool_name": "get_device_status"}]}
        resp = self.client.get("/assistant/audit/tool_calls", params={"limit": 10})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.list_reports")
    def test_reports_endpoint(self, mock_list_reports) -> None:
        mock_list_reports.return_value = {"ok": True, "count": 1, "items": [{"report_id": "report-1"}]}
        resp = self.client.get("/assistant/reports", params={"limit": 10})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.list_batch_runs")
    def test_batch_runs_endpoint(self, mock_list_batch_runs) -> None:
        mock_list_batch_runs.return_value = {"ok": True, "count": 1, "items": [{"batch_run_id": "batch-run-1"}]}
        resp = self.client.get("/assistant/ota/batch_runs", params={"limit": 10})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.get_batch_run")
    def test_batch_run_detail_endpoint(self, mock_get_batch_run) -> None:
        mock_get_batch_run.return_value = {"ok": True, "batch_run": {"batch_run_id": "batch-run-1"}, "items": []}
        resp = self.client.get("/assistant/ota/batch_runs/batch-run-1")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["batch_run"]["batch_run_id"], "batch-run-1")

    @patch("gateway_ai_assistant.app.service.refresh_batch_run")
    def test_batch_run_refresh_endpoint(self, mock_refresh_batch_run) -> None:
        mock_refresh_batch_run.return_value = {"ok": True, "batch_run": {"batch_run_id": "batch-run-1"}, "items": []}
        resp = self.client.post("/assistant/ota/batch_runs/batch-run-1/refresh")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["batch_run"]["batch_run_id"], "batch-run-1")

    @patch("gateway_ai_assistant.app.service.start_next_batch")
    def test_batch_run_start_next_endpoint(self, mock_start_next_batch) -> None:
        mock_start_next_batch.return_value = {"ok": True, "batch_run_id": "batch-run-1", "started_batch_index": 2}
        resp = self.client.post("/assistant/ota/batch_runs/batch-run-1/start_next_batch", json={"operator": "tester", "note": "next"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["started_batch_index"], 2)

    @patch("gateway_ai_assistant.app.service.pause_batch_run")
    def test_batch_run_pause_endpoint(self, mock_pause_batch_run) -> None:
        mock_pause_batch_run.return_value = {"ok": True, "batch_run_id": "batch-run-1", "status": "paused"}
        resp = self.client.post("/assistant/ota/batch_runs/batch-run-1/pause", json={"operator": "tester", "note": "pause"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"], "paused")

    @patch("gateway_ai_assistant.app.service.terminate_batch_run")
    def test_batch_run_terminate_endpoint(self, mock_terminate_batch_run) -> None:
        mock_terminate_batch_run.return_value = {"ok": True, "batch_run_id": "batch-run-1", "status": "terminated"}
        resp = self.client.post("/assistant/ota/batch_runs/batch-run-1/terminate", json={"operator": "tester", "note": "stop"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"], "terminated")

    @patch("gateway_ai_assistant.app.service.create_ticket")
    def test_ticket_create_endpoint(self, mock_create_ticket) -> None:
        mock_create_ticket.return_value = {"ok": True, "ticket_id": "ticket-1", "status": "open"}
        resp = self.client.post("/assistant/tickets", json={"report_id": "report-1", "severity": "high"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["ticket_id"], "ticket-1")

    @patch("gateway_ai_assistant.app.service.list_tickets")
    def test_ticket_list_endpoint(self, mock_list_tickets) -> None:
        mock_list_tickets.return_value = {"ok": True, "count": 1, "items": [{"ticket_id": "ticket-1"}]}
        resp = self.client.get("/assistant/tickets", params={"limit": 10})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)

    @patch("gateway_ai_assistant.app.service.reopen_ticket")
    def test_ticket_reopen_endpoint(self, mock_reopen_ticket) -> None:
        mock_reopen_ticket.return_value = {"ok": True, "ticket": {"ticket_id": "ticket-1", "status": "open"}}
        resp = self.client.post("/assistant/tickets/ticket-1/reopen")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["ticket"]["status"], "open")
