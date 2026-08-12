from __future__ import annotations

import unittest
from unittest.mock import patch

from fastapi.testclient import TestClient

from gateway_ai_assistant.app import app


class AppRuntimeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.client = TestClient(app)

    @patch("gateway_ai_assistant.app.service.runtime_gateway.get_device_status")
    def test_runtime_device_status_endpoint(self, mock_get_device_status) -> None:
        mock_get_device_status.return_value = {"ok": True, "status": "online"}
        resp = self.client.post("/assistant/runtime/device_status", json={"device_id": "DEVICE_001"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"], "online")

    @patch("gateway_ai_assistant.app.service.runtime_gateway.get_system_events")
    def test_runtime_system_events_endpoint(self, mock_get_system_events) -> None:
        mock_get_system_events.return_value = {"ok": True, "count": 1, "records": [{"event_type": "device_online"}]}
        resp = self.client.get("/assistant/runtime/system_events", params={"limit": 5})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["count"], 1)


if __name__ == "__main__":
    unittest.main()
