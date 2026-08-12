from __future__ import annotations

import sqlite3
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from gateway_ai_assistant.config import Settings, get_settings
from gateway_ai_assistant.service import AssistantService


class ToolTraceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmpdir = tempfile.TemporaryDirectory()
        base = get_settings()
        data_dir = Path(self.tmpdir.name) / "data"
        data_dir.mkdir(parents=True, exist_ok=True)
        self.settings = Settings(
            repo_root=base.repo_root,
            app_root=base.app_root,
            data_dir=data_dir,
            reports_dir=Path(self.tmpdir.name) / "reports",
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

    @patch("gateway_ai_assistant.service.RuntimeGateway.get_device_status")
    def test_chat_runtime_returns_tool_trace_and_logs_it(self, mock_get_device_status) -> None:
        mock_get_device_status.return_value = {
            "ok": True,
            "source": "http",
            "status": "online",
            "status_source": "http_api:/api/devices",
            "message": "Device status was retrieved from gateway monitor HTTP API.",
        }

        result = self.service.chat("sensor-01 当前状态正常吗？")

        self.assertEqual(result["route_type"], "Runtime")
        self.assertEqual(len(result["tool_trace"]), 1)
        self.assertEqual(result["tool_trace"][0]["tool_name"], "get_device_status")
        self.assertEqual(result["tool_trace"][0]["tool_status"], "ok")

        conn = sqlite3.connect(self.settings.assistant_db)
        try:
            row = conn.execute(
                "SELECT tool_name, status, question FROM tool_call_logs ORDER BY id DESC LIMIT 1"
            ).fetchone()
        finally:
            conn.close()

        self.assertIsNotNone(row)
        assert row is not None
        self.assertEqual(row[0], "get_device_status")
        self.assertEqual(row[1], "ok")
        self.assertEqual(row[2], "sensor-01 当前状态正常吗？")
