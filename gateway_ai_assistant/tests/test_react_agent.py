from __future__ import annotations

import asyncio
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock

from langchain_core.messages import AIMessage, HumanMessage, ToolMessage

from gateway_ai_assistant.agent.react_agent import ReActAgent
from gateway_ai_assistant.config import Settings, get_settings
from gateway_ai_assistant.sql.assistant_db import AssistantDB


class ReActAgentTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmpdir = tempfile.TemporaryDirectory()
        base = get_settings()
        data_dir = Path(self.tmpdir.name) / "data"
        reports_dir = Path(self.tmpdir.name) / "reports"
        prompts_dir = Path(self.tmpdir.name) / "prompts"
        data_dir.mkdir(parents=True, exist_ok=True)
        reports_dir.mkdir(parents=True, exist_ok=True)
        prompts_dir.mkdir(parents=True, exist_ok=True)
        (prompts_dir / "system_prompt.txt").write_text("test prompt", encoding="utf-8")
        self.settings = Settings(
            repo_root=base.repo_root,
            app_root=base.app_root,
            data_dir=data_dir,
            reports_dir=reports_dir,
            prompts_dir=prompts_dir,
            index_path=data_dir / "rag_index.json",
            gateway_root=base.gateway_root,
            gateway_history_db=base.gateway_history_db,
            gateway_ota_db=base.gateway_ota_db,
            assistant_db=data_dir / "assistant.db",
            llm_api_key="test-key",
            llm_base_url=base.llm_base_url,
            llm_model=base.llm_model,
        )
        self.assistant_db = AssistantDB(self.settings.assistant_db)
        self.agent = ReActAgent.__new__(ReActAgent)
        self.agent.settings = self.settings
        self.agent.assistant_db = self.assistant_db
        self.agent.service = MagicMock()
        self.agent._pending_interrupt_actions = {}
        self.agent._pending_manual_actions = {}
        self.agent.native_hitl_enabled = True
        self.agent._stringify_content = ReActAgent._stringify_content.__get__(self.agent, ReActAgent)
        self.agent._tool_message_to_output = ReActAgent._tool_message_to_output.__get__(self.agent, ReActAgent)
        self.agent._persist_tool_call_logs = ReActAgent._persist_tool_call_logs.__get__(self.agent, ReActAgent)
        self.agent._persist_resumed_action_logs = ReActAgent._persist_resumed_action_logs.__get__(self.agent, ReActAgent)
        self.agent._extract_interrupt_action_requests = ReActAgent._extract_interrupt_action_requests.__get__(self.agent, ReActAgent)
        self.agent._extract_manual_approval_actions = ReActAgent._extract_manual_approval_actions.__get__(self.agent, ReActAgent)
        self.agent._execute_pending_manual_actions = ReActAgent._execute_pending_manual_actions.__get__(self.agent, ReActAgent)
        self.agent._execute_write_action = ReActAgent._execute_write_action.__get__(self.agent, ReActAgent)
        self.agent._build_manual_resume_answer = ReActAgent._build_manual_resume_answer.__get__(self.agent, ReActAgent)
        self.agent._build_manual_resume_skipped_answer = ReActAgent._build_manual_resume_skipped_answer.__get__(self.agent, ReActAgent)
        self.agent._build_resume_answer = ReActAgent._build_resume_answer.__get__(self.agent, ReActAgent)
        self.agent._normalize_resume_decision = ReActAgent._normalize_resume_decision.__get__(self.agent, ReActAgent)
        self.agent._build_resume_command = ReActAgent._build_resume_command.__get__(self.agent, ReActAgent)
        self.agent._agent_config = ReActAgent._agent_config.__get__(self.agent, ReActAgent)
        self.agent._state_messages = ReActAgent._state_messages.__get__(self.agent, ReActAgent)
        self.agent._extract_answer_from_messages = ReActAgent._extract_answer_from_messages.__get__(self.agent, ReActAgent)
        self.agent._approx_message_size = ReActAgent._approx_message_size.__get__(self.agent, ReActAgent)
        self.agent.inspect_session = ReActAgent.inspect_session.__get__(self.agent, ReActAgent)
        self.agent.astream = ReActAgent.astream.__get__(self.agent, ReActAgent)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_invoke_logs_completed_tool_call(self) -> None:
        self.agent._agent = SimpleNamespace(
            invoke=lambda *_args, **_kwargs: SimpleNamespace(
                value={
                    "messages": [
                        AIMessage(
                            content="",
                            tool_calls=[
                                {
                                    "id": "call-1",
                                    "name": "get_device_status",
                                    "args": {"device_ref": "DEVICE_001"},
                                }
                            ],
                        ),
                        ToolMessage(
                            content='{"ok": true, "status": "online"}',
                            tool_call_id="call-1",
                            name="get_device_status",
                        ),
                        AIMessage(content="设备在线"),
                    ]
                },
                interrupts=[],
            )
        )

        result = ReActAgent.invoke(self.agent, "DEVICE_001 当前状态正常吗？", "agent-session-1")

        self.assertTrue(result["ok"])
        self.assertEqual(result["tool_call_count"], 1)
        logs = self.assistant_db.list_tool_call_logs(limit=10, session_id="agent-session-1")
        self.assertEqual(len(logs), 1)
        self.assertEqual(logs[0]["tool_name"], "get_device_status")
        self.assertEqual(logs[0]["tool_input"]["device_ref"], "DEVICE_001")
        self.assertTrue(logs[0]["tool_output"]["ok"])
        self.assertEqual(logs[0]["status"], "ok")

    def test_invoke_logs_pending_approval_for_interrupted_write_tool(self) -> None:
        interrupt = SimpleNamespace(
            value={
                "action_requests": [
                    {
                        "name": "approve_ota_request",
                        "arguments": {"approval_id": "approval-1"},
                    }
                ]
            }
        )
        self.agent._agent = SimpleNamespace(
            invoke=lambda *_args, **_kwargs: SimpleNamespace(
                value={
                    "messages": [
                        AIMessage(
                            content="",
                            tool_calls=[
                                {
                                    "id": "call-2",
                                    "name": "approve_ota_request",
                                    "args": {"approval_id": "approval-1"},
                                }
                            ],
                        )
                    ]
                },
                interrupts=[interrupt],
            )
        )

        result = ReActAgent.invoke(self.agent, "批准 approval-1", "agent-session-2")

        self.assertTrue(result["ok"])
        self.assertTrue(result["interrupted"])
        self.assertIn("暂停等待人工审批", result["answer"])
        logs = self.assistant_db.list_tool_call_logs(limit=10, session_id="agent-session-2")
        self.assertEqual(len(logs), 1)
        self.assertEqual(logs[0]["tool_name"], "approve_ota_request")
        self.assertEqual(logs[0]["status"], "pending_approval")

    def test_resume_returns_final_answer(self) -> None:
        self.agent._pending_interrupt_actions["agent-session-3"] = [
            {"name": "approve_ota_request", "args": {"approval_id": "approval-1", "approved_by": "operator"}}
        ]
        self.agent._agent = SimpleNamespace(
            get_state=lambda *_args, **_kwargs: SimpleNamespace(
                interrupts=[SimpleNamespace(value={"action_requests": self.agent._pending_interrupt_actions["agent-session-3"]})]
            ),
            invoke=lambda *_args, **_kwargs: SimpleNamespace(
                value={"messages": [AIMessage(content="已继续执行")]},
                interrupts=[],
            )
        )

        result = ReActAgent.resume(self.agent, "agent-session-3")

        self.assertTrue(result["ok"])
        self.assertEqual(result["mode"], "react_agent_resumed")
        self.assertEqual(result["answer"], "已继续执行")

    def test_resume_uses_tool_failure_output_for_answer_and_logs_execution(self) -> None:
        self.agent._pending_interrupt_actions["agent-session-5"] = [
            {"name": "approve_ota_request", "args": {"approval_id": "approval-live-valid-1", "approved_by": "operator"}}
        ]
        self.agent._agent = SimpleNamespace(
            get_state=lambda *_args, **_kwargs: SimpleNamespace(
                interrupts=[
                    SimpleNamespace(
                        value={
                            "action_requests": [
                                {
                                    "name": "approve_ota_request",
                                    "args": {"approval_id": "approval-live-valid-1", "approved_by": "operator"},
                                }
                            ]
                        }
                    )
                ]
            ),
            invoke=lambda *_args, **_kwargs: SimpleNamespace(
                value={
                    "messages": [
                        ToolMessage(
                            content='{"ok": false, "message": "Approval confirmed, but gateway OTA task creation failed.", "error": "HTTPError: 502"}',
                            tool_call_id="call-approve-1",
                            name="approve_ota_request",
                            status="error",
                        ),
                        AIMessage(content="模型误判摘要"),
                    ]
                },
                interrupts=[],
            ),
        )

        result = ReActAgent.resume(self.agent, "agent-session-5")

        self.assertTrue(result["ok"])
        self.assertIn("操作执行失败", result["answer"])
        self.assertIn("approve_ota_request", result["answer"])
        logs = self.assistant_db.list_tool_call_logs(limit=10, session_id="agent-session-5")
        self.assertEqual(len(logs), 1)
        self.assertEqual(logs[0]["tool_name"], "approve_ota_request")
        self.assertEqual(logs[0]["status"], "error")
        self.assertFalse(logs[0]["tool_output"]["ok"])

    def test_resume_rejects_session_without_pending_interrupt(self) -> None:
        self.agent._agent = SimpleNamespace(
            get_state=lambda *_args, **_kwargs: SimpleNamespace(interrupts=[]),
        )

        result = ReActAgent.resume(self.agent, "agent-session-4")

        self.assertFalse(result["ok"])
        self.assertIn("No pending approval interrupt", result["error"])

    def test_invoke_detects_manual_approval_tool_output(self) -> None:
        self.agent.native_hitl_enabled = False
        self.agent._agent = SimpleNamespace(
            invoke=lambda *_args, **_kwargs: SimpleNamespace(
                value={
                    "messages": [
                        AIMessage(
                            content="",
                            tool_calls=[
                                {
                                    "id": "call-3",
                                    "name": "approve_ota_request",
                                    "args": {"approval_id": "approval-2", "approved_by": "operator"},
                                }
                            ],
                        ),
                        ToolMessage(
                            content='{"ok": true, "requires_approval": true, "action_name": "approve_ota_request", "action_args": {"approval_id": "approval-2", "approved_by": "operator"}}',
                            tool_call_id="call-3",
                            name="approve_ota_request",
                        ),
                        AIMessage(content="请审批后继续"),
                    ]
                },
                interrupts=[],
            )
        )

        result = ReActAgent.invoke(self.agent, "批准 approval-2", "agent-session-6")

        self.assertTrue(result["ok"])
        self.assertTrue(result["interrupted"])
        self.assertEqual(
            self.agent._pending_manual_actions["agent-session-6"][0]["arguments"]["approval_id"],
            "approval-2",
        )
        logs = self.assistant_db.list_tool_call_logs(limit=10, session_id="agent-session-6")
        self.assertEqual(logs[0]["status"], "pending_approval")

    def test_resume_executes_pending_manual_action(self) -> None:
        self.agent._pending_manual_actions["agent-session-7"] = [
            {"name": "approve_ota_request", "arguments": {"approval_id": "approval-3", "approved_by": "operator"}}
        ]
        self.agent.service.workflow_ota_request_confirm.return_value = {
            "ok": True,
            "message": "Approval request has been confirmed and OTA task creation was triggered.",
        }

        result = ReActAgent.resume(self.agent, "agent-session-7")

        self.assertTrue(result["ok"])
        self.assertIn("操作执行完成", result["answer"])
        self.agent.service.workflow_ota_request_confirm.assert_called_once_with("approval-3", "operator")
        logs = self.assistant_db.list_tool_call_logs(limit=10, session_id="agent-session-7")
        self.assertEqual(logs[0]["tool_name"], "approve_ota_request")
        self.assertEqual(logs[0]["status"], "ok")

    def test_resume_rejects_pending_manual_action_without_executing(self) -> None:
        self.agent._pending_manual_actions["agent-session-8"] = [
            {"name": "approve_ota_request", "arguments": {"approval_id": "approval-4", "approved_by": "operator"}}
        ]

        result = ReActAgent.resume(self.agent, "agent-session-8", decision="reject")

        self.assertTrue(result["ok"])
        self.assertEqual(result["decision"], "reject")
        self.assertIn("审批已拒绝", result["answer"])
        self.agent.service.workflow_ota_request_confirm.assert_not_called()
        logs = self.assistant_db.list_tool_call_logs(limit=10, session_id="agent-session-8")
        self.assertEqual(len(logs), 0)

    def test_resume_edits_pending_manual_action_arguments(self) -> None:
        self.agent._pending_manual_actions["agent-session-9"] = [
            {"name": "approve_ota_request", "arguments": {"approval_id": "approval-5", "approved_by": "operator"}}
        ]
        self.agent.service.workflow_ota_request_confirm.return_value = {
            "ok": True,
            "message": "Approval request has been confirmed and OTA task creation was triggered.",
        }

        result = ReActAgent.resume(
            self.agent,
            "agent-session-9",
            decision="edit",
            edit_args={"approved_by": "reviewer"},
        )

        self.assertTrue(result["ok"])
        self.assertEqual(result["decision"], "edit")
        self.assertIn("修改后执行完成", result["answer"])
        self.agent.service.workflow_ota_request_confirm.assert_called_once_with("approval-5", "reviewer")

    def test_resume_native_hitl_reject_uses_reject_command(self) -> None:
        self.agent._pending_interrupt_actions["agent-session-10"] = [
            {"name": "approve_ota_request", "args": {"approval_id": "approval-6", "approved_by": "operator"}}
        ]
        invoke_mock = MagicMock(
            return_value=SimpleNamespace(
                value={"messages": [AIMessage(content="审批已拒绝")]},
                interrupts=[],
            )
        )
        self.agent._agent = SimpleNamespace(
            get_state=lambda *_args, **_kwargs: SimpleNamespace(
                interrupts=[SimpleNamespace(value={"action_requests": self.agent._pending_interrupt_actions["agent-session-10"]})]
            ),
            invoke=invoke_mock,
        )

        result = ReActAgent.resume(self.agent, "agent-session-10", decision="reject")

        self.assertTrue(result["ok"])
        self.assertEqual(result["decision"], "reject")
        resume_cmd = invoke_mock.call_args.args[0]
        self.assertEqual(resume_cmd.resume["decisions"][0]["type"], "reject")

    def test_astream_emits_tool_token_and_final_answer_events(self) -> None:
        async def fake_astream_events(*_args, **_kwargs):
            yield {"event": "on_tool_start", "name": "get_device_status", "data": {"input": {"device_ref": "1"}}}
            yield {"event": "on_chat_model_stream", "data": {"chunk": SimpleNamespace(content="设备")}}
            yield {"event": "on_chat_model_stream", "data": {"chunk": SimpleNamespace(content="在线")}}
            yield {"event": "on_tool_end", "name": "get_device_status", "data": {"output": {"ok": True}}}

        self.agent._agent = SimpleNamespace(
            astream_events=fake_astream_events,
            get_state=lambda *_args, **_kwargs: SimpleNamespace(
                values={"messages": [AIMessage(content="设备在线")]},
                interrupts=[],
            ),
        )

        async def collect():
            items = []
            async for item in self.agent.astream("DEVICE_001 当前状态正常吗？", "agent-session-stream-1"):
                items.append(item)
            return items

        events = asyncio.run(collect())

        self.assertEqual(events[0]["type"], "tool_start")
        self.assertEqual(events[1]["type"], "token")
        self.assertEqual(events[2]["type"], "token")
        self.assertEqual(events[3]["type"], "tool_end")
        self.assertEqual(events[4]["type"], "final_answer")
        self.assertEqual(events[4]["content"], "设备在线")
        self.assertEqual(events[5]["type"], "done")

    def test_astream_emits_interrupt_event_when_state_has_pending_actions(self) -> None:
        async def fake_astream_events(*_args, **_kwargs):
            if False:
                yield {}

        pending_actions = [{"name": "approve_ota_request", "arguments": {"approval_id": "approval-7"}}]
        self.agent._agent = SimpleNamespace(
            astream_events=fake_astream_events,
            get_state=lambda *_args, **_kwargs: SimpleNamespace(
                values={"messages": [AIMessage(content="")]},
                interrupts=[SimpleNamespace(value={"action_requests": pending_actions})],
            ),
        )

        async def collect():
            items = []
            async for item in self.agent.astream("批准 approval-7", "agent-session-stream-2"):
                items.append(item)
            return items

        events = asyncio.run(collect())

        self.assertEqual(events[0]["type"], "interrupt")
        self.assertEqual(events[0]["actions"][0]["name"], "approve_ota_request")
        self.assertEqual(events[1]["type"], "done")

    def test_inspect_session_reports_context_warning(self) -> None:
        self.agent._agent = SimpleNamespace(
            get_state=lambda *_args, **_kwargs: SimpleNamespace(
                values={
                    "messages": [
                        HumanMessage(content="A" * 7000),
                        AIMessage(content="B" * 7000),
                    ]
                },
                interrupts=[],
            )
        )

        result = self.agent.inspect_session("agent-session-inspect-1")

        self.assertTrue(result["ok"])
        self.assertEqual(result["message_count"], 2)
        self.assertGreaterEqual(result["approx_char_count"], 14000)
        self.assertTrue(result["context_warning"])
