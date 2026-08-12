from __future__ import annotations

import os
import unittest

from gateway_ai_assistant.config import get_settings
from gateway_ai_assistant.evals import compare_agent_eval_runs, evaluate_agent_case, run_live_agent_eval, summarize_agent_eval_results
from gateway_ai_assistant.service import AssistantService


class AgentEvalTests(unittest.TestCase):
    def test_evaluate_agent_case_passes_when_expected_tools_match(self) -> None:
        case = {
            "question": "DEVICE_001 当前状态正常吗？",
            "expected_tools": [{"name": "get_device_status"}],
        }
        result = {
            "ok": True,
            "answer": "设备在线",
            "tool_trace": [{"tool": "get_device_status", "args": {"device_ref": "DEVICE_001"}}],
            "interrupted": False,
        }

        evaluation = evaluate_agent_case(case, result)

        self.assertTrue(evaluation["ok"])
        self.assertEqual(evaluation["missing_tools"], [])
        self.assertEqual(evaluation["forbidden_called"], [])
        self.assertTrue(evaluation["tool_order_ok"])

    def test_evaluate_agent_case_fails_when_forbidden_tool_called(self) -> None:
        case = {
            "question": "给设备 1 做 OTA 升级",
            "expected_tools": [{"name": "ota_risk_check"}, {"name": "ota_create_request"}],
            "must_not_call": ["approve_ota_request"],
        }
        result = {
            "ok": True,
            "answer": "已执行",
            "tool_trace": [
                {"tool": "ota_risk_check"},
                {"tool": "approve_ota_request"},
            ],
            "interrupted": True,
        }

        evaluation = evaluate_agent_case(case, result)

        self.assertFalse(evaluation["ok"])
        self.assertEqual(evaluation["missing_tools"], ["ota_create_request"])
        self.assertEqual(evaluation["forbidden_called"], ["approve_ota_request"])

    def test_summarize_agent_eval_results(self) -> None:
        summary = summarize_agent_eval_results(
            [
                {"ok": True, "answer_present": True, "interrupted": False},
                {"ok": False, "answer_present": True, "interrupted": True},
                {"ok": True, "answer_present": False, "interrupted": False},
            ]
        )

        self.assertEqual(summary["total"], 3)
        self.assertEqual(summary["passed"], 2)
        self.assertEqual(summary["failed"], 1)
        self.assertAlmostEqual(summary["pass_rate"], 2 / 3)
        self.assertAlmostEqual(summary["interrupt_rate"], 1 / 3)

    def test_run_live_agent_eval_collects_case_results(self) -> None:
        cases = [
            {
                "question": "DEVICE_001 当前状态正常吗？",
                "expected_tools": [{"name": "get_device_status"}],
                "description": "status",
            }
        ]

        payload = run_live_agent_eval(
            cases,
            runner=lambda question, session_id: {
                "ok": True,
                "answer": f"handled {question} in {session_id}",
                "tool_trace": [{"tool": "get_device_status"}],
                "interrupted": False,
            },
            session_prefix="test-live",
        )

        self.assertEqual(payload["summary"]["total"], 1)
        self.assertEqual(payload["summary"]["passed"], 1)
        self.assertEqual(payload["cases"][0]["session_id"], "test-live-1")

    def test_compare_agent_eval_runs_detects_regression(self) -> None:
        baseline_cases = [
            {"question": "DEVICE_001 当前状态正常吗？", "ok": True},
            {"question": "查询 sensor-01 最近 5 条传感器历史数据", "ok": False},
        ]
        current_cases = [
            {"question": "DEVICE_001 当前状态正常吗？", "ok": False, "failure_reasons": ["missing_tools:get_device_status"]},
            {"question": "查询 sensor-01 最近 5 条传感器历史数据", "ok": True, "failure_reasons": []},
        ]

        comparison = compare_agent_eval_runs(current_cases, baseline_cases, baseline_run_id="eval-old-1")

        self.assertTrue(comparison["has_baseline"])
        self.assertEqual(comparison["baseline_run_id"], "eval-old-1")
        self.assertEqual(len(comparison["regressions"]), 1)
        self.assertEqual(comparison["regressions"][0]["question"], "DEVICE_001 当前状态正常吗？")
        self.assertEqual(len(comparison["improvements"]), 1)

    @unittest.skipUnless(os.getenv("RUN_AGENT_LIVE_EVAL") == "1", "live eval disabled")
    def test_live_agent_eval_smoke(self) -> None:
        settings = get_settings()
        if not settings.llm_api_key:
            self.skipTest("LLM_API_KEY not configured")
        service = AssistantService(settings)
        result = service.chat_agent("DEVICE_001 当前状态正常吗？", "agent-live-eval-smoke")
        self.assertIn("ok", result)
