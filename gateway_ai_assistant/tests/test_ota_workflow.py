from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from gateway_ai_assistant.config import Settings, get_settings
from gateway_ai_assistant.service import AssistantService
from gateway_ai_assistant.sql.assistant_db import ApprovalRequestRecord


class OtaWorkflowTests(unittest.TestCase):
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

    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_workflow_ota_risk_check(
        self,
        mock_get_device_status,
        mock_get_firmware_manifest,
        mock_list_recent_ota_tasks,
    ) -> None:
        mock_get_device_status.return_value = {
            "ok": True,
            "source": "http",
            "status": "online",
            "status_source": "http_api:/api/devices",
            "message": "device online",
        }
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "source": "filesystem",
            "firmware_id": "fw-1",
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
            "message": "manifest loaded",
        }
        mock_list_recent_ota_tasks.return_value = {
            "ok": True,
            "source": "sqlite",
            "device_id": 1,
            "count": 1,
            "tasks": [
                {
                    "task_uuid": "ota-1",
                    "device_id": 1,
                    "device_type": "stm32",
                    "firmware_id": "fw-old",
                    "state": "SUCCESS",
                    "last_error": "",
                }
            ],
            "message": "recent tasks loaded",
        }

        result = self.service.workflow_ota_risk_check(1, "fw-1")

        self.assertTrue(result["ok"])
        self.assertIn("risk_level", result)
        self.assertEqual(len(result["tool_trace"]), 3)
        self.assertEqual(result["tool_trace"][0]["tool_name"], "get_device_status")

    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_workflow_ota_plan_marks_blocking_items(
        self,
        mock_get_device_status,
        mock_get_firmware_manifest,
        mock_list_recent_ota_tasks,
    ) -> None:
        mock_get_device_status.return_value = {
            "ok": True,
            "source": "http",
            "status": "offline",
            "status_source": "http_api:/api/devices",
            "message": "device offline",
        }
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "source": "filesystem",
            "firmware_id": "fw-1",
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
            "message": "manifest loaded",
        }
        mock_list_recent_ota_tasks.return_value = {
            "ok": True,
            "source": "sqlite",
            "device_id": 1,
            "count": 1,
            "tasks": [],
            "message": "recent tasks loaded",
        }

        result = self.service.workflow_ota_plan(1, "fw-1")

        self.assertTrue(result["ok"])
        self.assertFalse(result["ready_to_create"])
        self.assertGreaterEqual(len(result["blocking_items"]), 1)

    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_workflow_ota_request_create(
        self,
        mock_get_device_status,
        mock_get_firmware_manifest,
        mock_list_recent_ota_tasks,
    ) -> None:
        mock_get_device_status.return_value = {
            "ok": True,
            "source": "http",
            "status": "online",
            "status_source": "http_api:/api/devices",
            "message": "device online",
        }
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "source": "filesystem",
            "firmware_id": "fw-1",
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
            "message": "manifest loaded",
        }
        mock_list_recent_ota_tasks.return_value = {
            "ok": True,
            "source": "sqlite",
            "device_id": 1,
            "count": 0,
            "tasks": [],
            "message": "recent tasks loaded",
        }

        result = self.service.workflow_ota_request_create(1, "fw-1")

        self.assertTrue(result["ok"])
        self.assertEqual(result["status"], "pending")
        self.assertIn("approval_id", result)
        self.assertEqual(result["request_payload"]["transport"], "serial")

    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_workflow_ota_request_create_auto_selects_tcp_binary_transport(
        self,
        mock_get_device_status,
        mock_get_firmware_manifest,
        mock_list_recent_ota_tasks,
    ) -> None:
        mock_get_device_status.return_value = {
            "ok": True,
            "source": "http",
            "status": "online",
            "status_source": "http_api:/api/devices",
            "device": {"device_id": 1, "active_transport": "tcp_binary", "link_type": "tcp_binary"},
            "message": "device online",
        }
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "source": "filesystem",
            "firmware_id": "fw-1",
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
            "message": "manifest loaded",
        }
        mock_list_recent_ota_tasks.return_value = {
            "ok": True,
            "source": "sqlite",
            "device_id": 1,
            "count": 0,
            "tasks": [],
            "message": "recent tasks loaded",
        }

        result = self.service.workflow_ota_request_create(1, "fw-1")

        self.assertTrue(result["ok"])
        self.assertEqual(result["request_payload"]["transport"], "tcp_binary")
        self.assertEqual(result["request_payload"]["transport_resolution"]["mode"], "auto_active_transport")

    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_workflow_ota_request_create_respects_explicit_transport(
        self,
        mock_get_device_status,
        mock_get_firmware_manifest,
        mock_list_recent_ota_tasks,
    ) -> None:
        mock_get_device_status.return_value = {
            "ok": True,
            "source": "http",
            "status": "online",
            "status_source": "http_api:/api/devices",
            "device": {"device_id": 1, "active_transport": "tcp_binary"},
            "message": "device online",
        }
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "source": "filesystem",
            "firmware_id": "fw-1",
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
            "message": "manifest loaded",
        }
        mock_list_recent_ota_tasks.return_value = {
            "ok": True,
            "source": "sqlite",
            "device_id": 1,
            "count": 0,
            "tasks": [],
            "message": "recent tasks loaded",
        }

        result = self.service.workflow_ota_request_create(1, "fw-1", transport="serial", target="192.168.10.2:19090")

        self.assertTrue(result["ok"])
        self.assertEqual(result["request_payload"]["transport"], "serial")
        self.assertEqual(result["request_payload"]["target"], "192.168.10.2:19090")
        self.assertEqual(result["request_payload"]["transport_resolution"]["mode"], "explicit")

    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_gateway_status")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_workflow_ota_batch_plan(
        self,
        mock_get_device_status,
        mock_get_gateway_status,
        mock_get_firmware_manifest,
        mock_list_recent_ota_tasks,
    ) -> None:
        mock_get_device_status.return_value = {"ok": True, "status": "online", "status_source": "http_api:/api/devices", "message": "ok"}
        mock_get_gateway_status.return_value = {"ok": True, "serial_available": True}
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
            "message": "manifest loaded",
        }
        mock_list_recent_ota_tasks.return_value = {"ok": True, "count": 0, "tasks": [], "message": "recent tasks loaded"}

        result = self.service.workflow_ota_batch_plan([1, 2, 3], "fw-1", batch_size=2)

        self.assertTrue(result["ok"])
        self.assertEqual(result["batch_count"], 2)
        self.assertEqual(result["rollout_batches"], [[1, 2], [3]])
        self.assertEqual(result["ready_device_ids"], [1, 2, 3])
        self.assertIn("device_transport_overrides", result)

    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_gateway_status")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_workflow_ota_batch_request_create(
        self,
        mock_get_device_status,
        mock_get_gateway_status,
        mock_get_firmware_manifest,
        mock_list_recent_ota_tasks,
    ) -> None:
        mock_get_device_status.return_value = {"ok": True, "status": "online", "status_source": "http_api:/api/devices", "message": "ok"}
        mock_get_gateway_status.return_value = {"ok": True, "serial_available": True}
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
            "message": "manifest loaded",
        }
        mock_list_recent_ota_tasks.return_value = {"ok": True, "count": 0, "tasks": [], "message": "recent tasks loaded"}

        result = self.service.workflow_ota_batch_request_create([1, 2, 3], "fw-1", batch_size=2)

        self.assertTrue(result["ok"])
        self.assertEqual(result["status"], "pending")
        self.assertIn("approval_id", result)
        self.assertEqual(result["request_payload"]["batch_count"], 2)
        self.assertIn("device_transport_overrides", result["request_payload"])

    @patch("gateway_ai_assistant.service.OtaTools.create_ota_task")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    def test_workflow_ota_batch_request_confirm(
        self,
        mock_get_firmware_manifest,
        mock_create_ota_task,
    ) -> None:
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
            "message": "manifest loaded",
        }
        mock_create_ota_task.return_value = {
            "ok": True,
            "task": {"task_uuid": "ota-created"},
            "message": "created",
        }
        approval_id = "approval-batch-test-confirm"
        self.service.assistant_db.create_approval_request(
            ApprovalRequestRecord(
                approval_id=approval_id,
                request_type="ota_batch_create_task",
                request_payload={
                    "device_ids": [1, 2],
                    "ready_device_ids": [1, 2],
                    "blocked_device_ids": [],
                    "firmware_id": "fw-1",
                    "transport": "serial",
                    "target": "127.0.0.1:19090",
                    "batch_count": 2,
                },
                risk_summary="test",
                status="pending",
            )
        )

        result = self.service.workflow_ota_batch_request_confirm(approval_id, "tester")

        self.assertTrue(result["ok"])
        self.assertEqual(result["status"], "created")
        self.assertEqual(result["started_batch_index"], 1)
        self.assertIn("batch_run_id", result)
        self.assertEqual(len(result["per_device_results"]), 2)
        self.assertEqual(mock_create_ota_task.call_count, 2)

    @patch("gateway_ai_assistant.service.OtaTools.create_ota_task")
    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_gateway_status")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_workflow_ota_request_confirm(
        self,
        mock_get_device_status,
        mock_get_gateway_status,
        mock_get_firmware_manifest,
        mock_list_recent_ota_tasks,
        mock_create_ota_task,
    ) -> None:
        mock_get_device_status.return_value = {
            "ok": True,
            "source": "http",
            "status": "online",
            "status_source": "http_api:/api/devices",
            "message": "device online",
        }
        mock_get_gateway_status.return_value = {"ok": True, "serial_available": True}
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "source": "filesystem",
            "firmware_id": "fw-1",
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
            "message": "manifest loaded",
        }
        mock_list_recent_ota_tasks.return_value = {
            "ok": True,
            "source": "sqlite",
            "device_id": 1,
            "count": 0,
            "tasks": [],
            "message": "recent tasks loaded",
        }
        mock_create_ota_task.return_value = {
            "ok": True,
            "source": "http",
            "task": {
                "task_uuid": "ota-new-1",
                "device_id": 1,
                "device_type": "stm32",
                "firmware_id": "fw-1",
                "state": "CREATED",
            },
            "message": "created",
        }

        request_result = self.service.workflow_ota_request_create(1, "fw-1")
        approval_id = request_result["approval_id"]
        result = self.service.workflow_ota_request_confirm(approval_id, "tester")

        self.assertTrue(result["ok"])
        self.assertEqual(result["status"], "approved")
        self.assertEqual(result["approved_by"], "tester")
        self.assertEqual(result["create_result"]["task"]["task_uuid"], "ota-new-1")

    @patch("gateway_ai_assistant.service.OtaTools.create_ota_task")
    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_gateway_status")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_workflow_ota_request_confirm_marks_execute_failed_when_gateway_submit_fails(
        self,
        mock_get_device_status,
        mock_get_gateway_status,
        mock_get_firmware_manifest,
        mock_list_recent_ota_tasks,
        mock_create_ota_task,
    ) -> None:
        mock_get_device_status.return_value = {
            "ok": True,
            "source": "http",
            "status": "online",
            "status_source": "http_api:/api/devices",
            "message": "device online",
        }
        mock_get_gateway_status.return_value = {"ok": True, "serial_available": True}
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "source": "filesystem",
            "firmware_id": "fw-1",
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
            "message": "manifest loaded",
        }
        mock_list_recent_ota_tasks.return_value = {
            "ok": True,
            "source": "sqlite",
            "device_id": 1,
            "count": 0,
            "tasks": [],
            "message": "recent tasks loaded",
        }
        mock_create_ota_task.return_value = {
            "ok": False,
            "source": "http",
            "message": "Gateway OTA API returned an HTTP error.",
            "error": "HTTPError: 502",
            "response": {},
        }

        request_result = self.service.workflow_ota_request_create(1, "fw-1")
        approval_id = request_result["approval_id"]

        result = self.service.workflow_ota_request_confirm(approval_id, "tester", "manual retry approved")

        self.assertFalse(result["ok"])
        self.assertEqual(result["status"], "execute_failed")
        self.assertEqual(result["approved_by"], "tester")
        approval = self.service.get_approval(approval_id)
        self.assertTrue(approval["ok"])
        self.assertEqual(approval["approval"]["status"], "execute_failed")
        self.assertEqual(approval["approval"]["approved_by"], "tester")
        self.assertEqual(approval["approval"]["decision_note"], "manual retry approved")
        self.assertEqual(approval["approval"]["result_payload"]["status"], "execute_failed")
        self.assertFalse(approval["approval"]["result_payload"]["create_result"]["ok"])

    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_gateway_status")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_workflow_ota_request_confirm_marks_execute_failed_when_manifest_lookup_fails(
        self,
        mock_get_device_status,
        mock_get_gateway_status,
        mock_get_firmware_manifest,
        mock_list_recent_ota_tasks,
    ) -> None:
        mock_get_device_status.return_value = {
            "ok": True,
            "source": "http",
            "status": "online",
            "status_source": "http_api:/api/devices",
            "message": "device online",
        }
        mock_get_gateway_status.return_value = {"ok": True, "serial_available": True}
        mock_get_firmware_manifest.return_value = {
            "ok": False,
            "message": "firmware_id is required.",
        }
        mock_list_recent_ota_tasks.return_value = {
            "ok": True,
            "source": "sqlite",
            "device_id": 1,
            "count": 0,
            "tasks": [],
            "message": "recent tasks loaded",
        }

        request_result = self.service.workflow_ota_request_create(1, "fw-1")
        approval_id = request_result["approval_id"]

        result = self.service.workflow_ota_request_confirm(approval_id, "tester", "manifest failed")

        self.assertFalse(result["ok"])
        self.assertEqual(result["status"], "execute_failed")
        approval = self.service.get_approval(approval_id)
        self.assertEqual(approval["approval"]["status"], "execute_failed")
        self.assertEqual(approval["approval"]["result_payload"]["reason"], "firmware_manifest_lookup_failed")

    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.OtaTools.get_firmware_manifest")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_gateway_status")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_workflow_ota_request_create_blocks_when_serial_transport_unavailable(
        self,
        mock_get_device_status,
        mock_get_gateway_status,
        mock_get_firmware_manifest,
        mock_list_recent_ota_tasks,
    ) -> None:
        mock_get_device_status.return_value = {"ok": True, "status": "online", "status_source": "http_api:/api/devices", "message": "ok"}
        mock_get_gateway_status.return_value = {"ok": True, "serial_available": False}
        mock_get_firmware_manifest.return_value = {
            "ok": True,
            "manifest": {"firmware_id": "fw-1", "device_type": "stm32"},
            "message": "manifest loaded",
        }
        mock_list_recent_ota_tasks.return_value = {"ok": True, "count": 0, "tasks": [], "message": "recent tasks loaded"}

        result = self.service.workflow_ota_request_create(1, "fw-1")

        self.assertFalse(result["ok"])
        self.assertEqual(result["transport"], "serial")
        self.assertEqual(result["transport_guard"]["gateway_status"]["serial_available"], False)

    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_system_events")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_sensor_history")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_workflow_generate_fault_ticket(
        self,
        mock_get_device_status,
        mock_get_sensor_history,
        mock_get_system_events,
        mock_list_recent_ota_tasks,
    ) -> None:
        mock_get_device_status.return_value = {
            "ok": True,
            "status": "offline",
            "status_source": "http_api:/api/devices",
            "message": "device offline",
        }
        mock_get_sensor_history.return_value = {
            "ok": True,
            "count": 2,
            "records": [{"device_id": 1, "temperature": 25.1}],
            "message": "history loaded",
        }
        mock_get_system_events.return_value = {
            "ok": True,
            "count": 1,
            "records": [{"event_type": "device_offline", "device_id": 1}],
            "message": "events loaded",
        }
        mock_list_recent_ota_tasks.return_value = {
            "ok": True,
            "count": 1,
            "tasks": [{"task_uuid": "ota-1", "state": "FAILED"}],
            "message": "recent tasks loaded",
        }

        result = self.service.workflow_generate_report("fault_ticket", device_id=1, task_id="", limit=5)

        self.assertTrue(result["ok"])
        self.assertEqual(result["report_type"], "fault_ticket")
        self.assertIn("Fault Ticket", result["title"])
        self.assertIn("device_status", result["markdown"])
        self.assertIn("diagnosis", result["markdown"])
        self.assertIn("diagnosis_result", result)
        self.assertEqual(len(result["tool_trace"]), 4)
        self.assertEqual(result["collection_mode"], "multi_agent_read_team")
        self.assertEqual(len(result["team_trace"]), 4)

    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_system_events")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_sensor_history")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_workflow_diagnose_fault_detects_offline_and_crc(
        self,
        mock_get_device_status,
        mock_get_sensor_history,
        mock_get_system_events,
        mock_list_recent_ota_tasks,
    ) -> None:
        mock_get_device_status.return_value = {
            "ok": True,
            "status": "offline",
            "status_source": "http_api:/api/devices",
            "message": "device offline",
        }
        mock_get_sensor_history.return_value = {
            "ok": True,
            "count": 0,
            "records": [],
            "message": "no history",
        }
        mock_get_system_events.return_value = {
            "ok": True,
            "count": 3,
            "records": [
                {"event_type": "device_offline", "device_id": 1, "detail": "offline"},
                {"event_type": "crc_error", "device_id": 1, "detail": "count=2"},
                {"event_type": "device_online", "device_id": 2, "detail": "other device"},
            ],
            "message": "events loaded",
        }
        mock_list_recent_ota_tasks.return_value = {
            "ok": True,
            "count": 2,
            "tasks": [
                {"task_uuid": "ota-1", "state": "FAILED"},
                {"task_uuid": "ota-2", "state": "FAILED"},
            ],
            "message": "tasks loaded",
        }

        result = self.service.workflow_diagnose_fault(device_id=1, limit=10)

        self.assertTrue(result["ok"])
        self.assertEqual(result["overall_severity"], "high")
        codes = {item["code"] for item in result["findings"]}
        self.assertIn("device_offline", codes)
        self.assertIn("no_recent_sensor_data", codes)
        self.assertIn("crc_error", codes)
        self.assertIn("repeated_ota_failures", codes)
        self.assertEqual(len(result["tool_trace"]), 4)
        self.assertEqual(result["collection_mode"], "multi_agent_read_team")
        self.assertEqual(len(result["team_trace"]), 4)
        self.assertEqual(result["team_trace"][0]["agent_name"], "StatusAgent")


class WorkflowPersistenceTests(unittest.TestCase):
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

    @patch("gateway_ai_assistant.service.OtaTools.list_recent_ota_tasks_for_device")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_system_events")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_sensor_history")
    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_report_generation_persists_files_and_record(
        self,
        mock_get_device_status,
        mock_get_sensor_history,
        mock_get_system_events,
        mock_list_recent_ota_tasks,
    ) -> None:
        mock_get_device_status.return_value = {"ok": True, "status": "online", "message": "ok"}
        mock_get_sensor_history.return_value = {"ok": True, "count": 1, "records": [{"device_id": 1}]}
        mock_get_system_events.return_value = {"ok": True, "count": 1, "records": [{"event_type": "device_online"}]}
        mock_list_recent_ota_tasks.return_value = {"ok": True, "count": 0, "tasks": []}

        result = self.service.workflow_generate_report("fault_ticket", device_id=1, task_id="", limit=5)

        self.assertTrue(result["ok"])
        self.assertTrue(Path(result["markdown_path"]).exists())
        self.assertTrue(Path(result["json_path"]).exists())
        reports = self.service.list_reports(limit=10)
        self.assertEqual(reports["count"], 1)
        self.assertEqual(reports["items"][0]["report_id"], result["report_id"])
        audits = self.service.list_action_audits(limit=10)
        self.assertTrue(any(item["action_type"] == "generate_report" for item in audits["items"]))
