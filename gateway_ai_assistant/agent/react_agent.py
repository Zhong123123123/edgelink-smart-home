from __future__ import annotations

from datetime import datetime, timezone
from collections.abc import AsyncIterator
import json
from pathlib import Path
from typing import Any

from langgraph.checkpoint.memory import MemorySaver
from langgraph.types import Command
from langchain.agents import create_agent
from langchain.agents.middleware import HumanInTheLoopMiddleware
from langchain_core.messages import AIMessage, HumanMessage, ToolMessage

from ..sql.assistant_db import QueryLogRecord, ToolCallLogRecord
from .langgraph_tools import create_readonly_tools, create_write_tools


class ReActAgent:
    def __init__(self, settings, assistant_db, service):
        self.settings = settings
        self.assistant_db = assistant_db
        self.service = service
        self._pending_interrupt_actions: dict[str, list[dict[str, Any]]] = {}
        self._pending_manual_actions: dict[str, list[dict[str, Any]]] = {}
        self.native_hitl_enabled = self._supports_native_hitl()

        self.readonly_tools = create_readonly_tools(service)
        self.write_tools = create_write_tools(
            service,
            approval_mode="execute" if self.native_hitl_enabled else "manual",
        )
        self.all_tools = self.readonly_tools + self.write_tools

        from langchain_openai import ChatOpenAI

        self.llm = ChatOpenAI(
            model=settings.llm_model,
            api_key=settings.llm_api_key,
            base_url=settings.llm_base_url,
            temperature=0.2,
            timeout=30,
        )
        self._checkpointer = MemorySaver()
        middleware = []
        if self.native_hitl_enabled:
            middleware = [
                HumanInTheLoopMiddleware(
                    interrupt_on={
                        "ota_create_request": {"allowed_decisions": ["approve", "reject"]},
                        "approve_ota_request": {"allowed_decisions": ["approve", "reject"]},
                        "pause_batch": {"allowed_decisions": ["approve", "reject"]},
                        "retry_failed_devices": {"allowed_decisions": ["approve", "reject"]},
                    }
                )
            ]
        self._agent = create_agent(
            model=self.llm,
            tools=self.all_tools,
            checkpointer=self._checkpointer,
            middleware=middleware,
            system_prompt=self._load_system_prompt(),
        )

    def _load_system_prompt(self) -> str:
        prompt_path = Path(self.settings.prompts_dir) / "system_prompt.txt"
        return prompt_path.read_text(encoding="utf-8").strip()

    def _supports_native_hitl(self) -> bool:
        base_url = str(self.settings.llm_base_url).strip().lower()
        return base_url.startswith("https://api.openai.com")

    def invoke(self, question: str, session_id: str) -> dict[str, Any]:
        config = self._agent_config(session_id)
        started_at = datetime.now(timezone.utc)
        tool_trace: list[dict[str, Any]] = []
        request_content = self._build_request_content(question, session_id)

        try:
            result = self._agent.invoke(
                {"messages": [HumanMessage(content=request_content)]},
                config=config,
                version="v2",
            )
            final_state = getattr(result, "value", result)
            interrupts = list(getattr(result, "interrupts", []))
            final_messages = final_state.get("messages", [])
            answer = ""
            for msg in final_messages:
                if isinstance(msg, AIMessage):
                    if msg.content and not msg.tool_calls:
                        answer = self._stringify_content(msg.content)
                    for tc in msg.tool_calls:
                        tool_trace.append(
                            {
                                "tool": tc.get("name", ""),
                                "args": tc.get("args", {}),
                                "step": len(tool_trace) + 1,
                            }
                        )

            interrupted = bool(interrupts)
            manual_actions = self._extract_manual_approval_actions(final_messages)
            if manual_actions:
                tool_trace = self._dedupe_tool_trace(tool_trace)
            if manual_actions:
                interrupted = True
                self._pending_manual_actions[session_id] = manual_actions
            else:
                self._pending_manual_actions.pop(session_id, None)
            if interrupted:
                action_requests = manual_actions or self._extract_interrupt_action_requests(interrupts)
                self._pending_interrupt_actions[session_id] = action_requests
                first_action = action_requests[0] if action_requests else {}
                answer = (
                    "Agent 请求执行高风险写操作，已暂停等待人工审批。\n"
                    f"工具: {first_action.get('name', '')}\n"
                    f"参数: {first_action.get('arguments', {})}\n"
                    "请调用 /assistant/agent/resume 继续。"
                )
            else:
                self._pending_interrupt_actions.pop(session_id, None)

            self._persist_tool_call_logs(
                session_id=session_id,
                question=question,
                messages=final_messages,
                interrupts=interrupts,
            )
            self.assistant_db.log_query(
                QueryLogRecord(
                    question=question,
                    route_type="react_agent",
                    sql_used="",
                    answer_summary=answer[:200],
                )
            )
            self.service.remember_agent_interaction(session_id, question, answer, tool_trace)
            return {
                "ok": True,
                "session_id": session_id,
                "answer": answer,
                "tool_trace": tool_trace,
                "tool_call_count": len(tool_trace),
                "interrupted": interrupted,
                "mode": "react_agent",
                "started_at": started_at.isoformat(),
                "finished_at": datetime.now(timezone.utc).isoformat(),
            }
        except Exception as exc:
            return {
                "ok": False,
                "session_id": session_id,
                "error": f"{type(exc).__name__}: {exc}",
                "tool_trace": tool_trace,
                "mode": "react_agent",
            }

    def resume(
        self,
        session_id: str,
        decision: str = "approve",
        edit_args: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        config = self._agent_config(session_id)
        try:
            normalized_decision = self._normalize_resume_decision(decision)
            if normalized_decision not in {"approve", "reject", "edit"}:
                return {
                    "ok": False,
                    "session_id": session_id,
                    "error": f"Unsupported resume decision: {decision}",
                }
            pending_actions = self._pending_interrupt_actions.get(session_id)
            pending_manual_actions = self._pending_manual_actions.get(session_id)
            if pending_manual_actions:
                if normalized_decision == "reject":
                    self._pending_manual_actions.pop(session_id, None)
                    self._pending_interrupt_actions.pop(session_id, None)
                    return {
                        "ok": True,
                        "session_id": session_id,
                        "answer": self._build_manual_resume_skipped_answer("reject", pending_manual_actions),
                        "mode": "react_agent_resumed",
                        "decision": "reject",
                        "results": [],
                    }
                outputs = self._execute_pending_manual_actions(
                    session_id,
                    pending_manual_actions,
                    edit_args=edit_args if normalized_decision == "edit" else None,
                )
                self._pending_manual_actions.pop(session_id, None)
                self._pending_interrupt_actions.pop(session_id, None)
                return {
                    "ok": True,
                    "session_id": session_id,
                    "answer": self._build_manual_resume_answer(outputs, normalized_decision),
                    "mode": "react_agent_resumed",
                    "decision": normalized_decision,
                    "results": outputs,
                }
            if not pending_actions:
                state = self._agent.get_state(config)
                pending_interrupts = list(getattr(state, "interrupts", []))
                pending_actions = self._extract_interrupt_action_requests(pending_interrupts)
            if not pending_actions:
                return {
                    "ok": False,
                    "session_id": session_id,
                    "error": "No pending approval interrupt for this session.",
                }
            resume_cmd = self._build_resume_command(normalized_decision, edit_args)
            if resume_cmd is None:
                return {
                    "ok": False,
                    "session_id": session_id,
                    "error": f"Unsupported resume decision: {decision}",
                }
            result = self._agent.invoke(
                resume_cmd,
                config=config,
                version="v2",
            )
            final_state = getattr(result, "value", result)
            interrupts = list(getattr(result, "interrupts", []))
            final_messages = final_state.get("messages", [])
            answer = ""
            for msg in final_messages:
                if isinstance(msg, AIMessage) and msg.content and not msg.tool_calls:
                    answer = self._stringify_content(msg.content)
            logged_count = self._persist_tool_call_logs(
                session_id=session_id,
                question="resume",
                messages=final_messages,
                interrupts=interrupts,
            )
            if pending_actions:
                self._persist_resumed_action_logs(
                    session_id=session_id,
                    question="resume",
                    pending_actions=pending_actions,
                    messages=final_messages,
                    already_logged=logged_count,
                )
            deterministic_answer = self._build_resume_answer(final_messages, pending_actions)
            if deterministic_answer:
                answer = deterministic_answer
            if normalized_decision == "reject" and not answer.strip():
                answer = self._build_manual_resume_skipped_answer("reject", pending_actions)
            if not interrupts:
                self._pending_interrupt_actions.pop(session_id, None)
            else:
                self._pending_interrupt_actions[session_id] = self._extract_interrupt_action_requests(interrupts)
            resumed_tool_trace = [
                {
                    "tool": str(action.get("name", "")),
                    "args": action.get("args", {}) or action.get("arguments", {}) or {},
                    "step": index + 1,
                }
                for index, action in enumerate(pending_actions)
            ]
            self.service.remember_agent_interaction(session_id, "resume", answer, resumed_tool_trace)
            return {
                "ok": True,
                "session_id": session_id,
                "answer": answer,
                "mode": "react_agent_resumed",
                "decision": normalized_decision,
            }
        except Exception as exc:
            return {
                "ok": False,
                "session_id": session_id,
                "error": f"{type(exc).__name__}: {exc}",
            }

    def get_state(self, session_id: str) -> dict[str, Any]:
        config = self._agent_config(session_id)
        try:
            state = self._agent.get_state(config)
            return {"ok": True, "session_id": session_id, "state": str(state)}
        except Exception as exc:
            return {"ok": False, "error": f"{type(exc).__name__}: {exc}"}

    def inspect_session(self, session_id: str) -> dict[str, Any]:
        config = self._agent_config(session_id)
        try:
            state = self._agent.get_state(config)
            messages = self._state_messages(state)
            pending_interrupts = list(getattr(state, "interrupts", []))
            pending_actions = self._extract_interrupt_action_requests(pending_interrupts)
            message_types: dict[str, int] = {}
            approx_chars = 0
            for msg in messages:
                msg_type = str(getattr(msg, "type", type(msg).__name__)).strip() or "unknown"
                message_types[msg_type] = message_types.get(msg_type, 0) + 1
                approx_chars += self._approx_message_size(msg)
            warn_message_count = self.settings.agent_session_warn_message_count
            warn_char_count = self.settings.agent_session_warn_char_count
            context_warning = len(messages) >= warn_message_count or approx_chars >= warn_char_count
            return {
                "ok": True,
                "session_id": session_id,
                "message_count": len(messages),
                "approx_char_count": approx_chars,
                "message_types": message_types,
                "pending_interrupt": bool(pending_actions),
                "pending_action_count": len(pending_actions),
                "context_warning": context_warning,
                "warning_thresholds": {
                    "message_count": warn_message_count,
                    "approx_char_count": warn_char_count,
                },
            }
        except Exception as exc:
            return {
                "ok": False,
                "session_id": session_id,
                "error": f"{type(exc).__name__}: {exc}",
            }

    async def astream(self, question: str, session_id: str) -> AsyncIterator[dict[str, Any]]:
        config = self._agent_config(session_id)
        yielded_text_parts: list[str] = []
        request_content = self._build_request_content(question, session_id)
        try:
            async for event in self._agent.astream_events(
                {"messages": [HumanMessage(content=request_content)]},
                config=config,
                version="v2",
            ):
                kind = str(event.get("event", ""))
                if kind == "on_chat_model_stream":
                    chunk = event.get("data", {}).get("chunk")
                    content = self._stringify_content(getattr(chunk, "content", "")) if chunk is not None else ""
                    if content:
                        yielded_text_parts.append(content)
                        yield {"type": "token", "content": content, "session_id": session_id}
                elif kind == "on_tool_start":
                    yield {
                        "type": "tool_start",
                        "tool": event.get("name", ""),
                        "input": event.get("data", {}).get("input", {}),
                        "session_id": session_id,
                    }
                elif kind == "on_tool_end":
                    output = event.get("data", {}).get("output", {})
                    output_ok = output.get("ok") if isinstance(output, dict) else None
                    yield {
                        "type": "tool_end",
                        "tool": event.get("name", ""),
                        "output_ok": output_ok,
                        "session_id": session_id,
                    }

            state = self._agent.get_state(config)
            final_messages = self._state_messages(state)
            pending_interrupts = list(getattr(state, "interrupts", []))
            tool_trace = self._extract_tool_trace(final_messages)
            self._persist_tool_call_logs(
                session_id=session_id,
                question=question,
                messages=final_messages,
                interrupts=pending_interrupts,
            )
            final_answer = self._extract_answer_from_messages(final_messages)
            if not final_answer.strip():
                final_answer = "".join(yielded_text_parts).strip()
            pending_actions = self._extract_interrupt_action_requests(pending_interrupts)
            self.service.remember_agent_interaction(session_id, question, final_answer, tool_trace)
            if pending_actions:
                self._pending_interrupt_actions[session_id] = pending_actions
                yield {
                    "type": "interrupt",
                    "session_id": session_id,
                    "actions": pending_actions,
                }
            elif final_answer:
                self._pending_interrupt_actions.pop(session_id, None)
                yield {
                    "type": "final_answer",
                    "session_id": session_id,
                    "content": final_answer,
                }
        except Exception as exc:
            yield {
                "type": "error",
                "session_id": session_id,
                "error": f"{type(exc).__name__}: {exc}",
            }
        finally:
            yield {"type": "done", "session_id": session_id}

    def _stringify_content(self, content: Any) -> str:
        if isinstance(content, str):
            return content
        if isinstance(content, list):
            parts = []
            for item in content:
                if isinstance(item, str):
                    parts.append(item)
                elif isinstance(item, dict) and item.get("type") == "text":
                    parts.append(str(item.get("text", "")))
            return "".join(parts)
        return str(content)

    def _build_request_content(self, question: str, session_id: str) -> str:
        memory_context = self.service.build_agent_memory_context(session_id, question=question)
        memory_prompt = str(memory_context.get("memory_prompt", "")).strip()
        if not memory_prompt:
            return question
        return (
            "[Memory Context]\n"
            f"{memory_prompt}\n\n"
            "[User Request]\n"
            f"{question}"
        )

    def _agent_config(self, session_id: str) -> dict[str, Any]:
        return {
            "configurable": {"thread_id": session_id},
            "recursion_limit": self.settings.agent_recursion_limit,
        }

    def _state_messages(self, state: Any) -> list[Any]:
        values = getattr(state, "values", {})
        if isinstance(values, dict):
            messages = values.get("messages", [])
            if isinstance(messages, list):
                return messages
        value = getattr(state, "value", {})
        if isinstance(value, dict):
            messages = value.get("messages", [])
            if isinstance(messages, list):
                return messages
        if isinstance(state, dict):
            messages = state.get("messages", [])
            if isinstance(messages, list):
                return messages
        return []

    def _extract_answer_from_messages(self, messages: list[Any]) -> str:
        answer = ""
        for msg in messages:
            if isinstance(msg, AIMessage) and msg.content and not msg.tool_calls:
                answer = self._stringify_content(msg.content)
        return answer

    def _extract_tool_trace(self, messages: list[Any]) -> list[dict[str, Any]]:
        tool_trace: list[dict[str, Any]] = []
        for msg in messages:
            if not isinstance(msg, AIMessage):
                continue
            for tc in msg.tool_calls or []:
                tool_trace.append(
                    {
                        "tool": tc.get("name", ""),
                        "args": tc.get("args", {}),
                        "step": len(tool_trace) + 1,
                    }
                )
        return self._dedupe_tool_trace(tool_trace)

    def _approx_message_size(self, message: Any) -> int:
        total = 0
        total += len(self._stringify_content(getattr(message, "content", "")))
        if isinstance(message, AIMessage):
            for tool_call in getattr(message, "tool_calls", []) or []:
                total += len(json.dumps(tool_call, ensure_ascii=False, sort_keys=True))
        if isinstance(message, ToolMessage) and isinstance(message.artifact, dict):
            total += len(json.dumps(message.artifact, ensure_ascii=False, sort_keys=True))
        return total

    def _persist_tool_call_logs(
        self,
        session_id: str,
        question: str,
        messages: list[Any],
        interrupts: list[Any],
    ) -> int:
        tool_outputs: dict[str, ToolMessage] = {}
        for msg in messages:
            if isinstance(msg, ToolMessage):
                tool_outputs[msg.tool_call_id] = msg

        seen_keys: set[tuple[str, str]] = set()
        logged_count = 0
        for msg in messages:
            if not isinstance(msg, AIMessage) or not msg.tool_calls:
                continue
            for tc in msg.tool_calls:
                tool_call_id = str(tc.get("id", ""))
                tool_name = str(tc.get("name", ""))
                if not tool_name:
                    continue
                dedupe_key = (tool_call_id, tool_name)
                if dedupe_key in seen_keys:
                    continue
                seen_keys.add(dedupe_key)
                tool_message = tool_outputs.get(tool_call_id)
                if tool_message is not None:
                    tool_output = self._tool_message_to_output(tool_message)
                    if tool_output.get("requires_approval"):
                        status = "pending_approval"
                    else:
                        status = "ok" if tool_message.status != "error" else "error"
                else:
                    tool_output = {
                        "ok": False,
                        "message": "Tool call interrupted pending approval.",
                    }
                    status = "pending_approval" if interrupts else "pending"
                self.assistant_db.log_tool_call(
                    ToolCallLogRecord(
                        session_id=session_id,
                        question=question,
                        tool_name=tool_name,
                        tool_input=tc.get("args", {}) or {},
                        tool_output=tool_output,
                        status=status,
                    )
                )
                logged_count += 1
        return logged_count

    def _persist_resumed_action_logs(
        self,
        session_id: str,
        question: str,
        pending_actions: list[dict[str, Any]],
        messages: list[Any],
        already_logged: int,
    ) -> None:
        if already_logged:
            return
        tool_messages = [msg for msg in messages if isinstance(msg, ToolMessage)]
        if not tool_messages:
            return
        for action, tool_message in zip(pending_actions, tool_messages):
            tool_name = str(action.get("name", "") or tool_message.name or "")
            if not tool_name:
                continue
            tool_output = self._tool_message_to_output(tool_message)
            status = "ok" if tool_message.status != "error" and tool_output.get("ok", True) else "error"
            self.assistant_db.log_tool_call(
                ToolCallLogRecord(
                    session_id=session_id,
                    question=question,
                    tool_name=tool_name,
                    tool_input=action.get("args", {}) or action.get("arguments", {}) or {},
                    tool_output=tool_output,
                    status=status,
                )
            )

    def _extract_interrupt_action_requests(self, interrupts: list[Any]) -> list[dict[str, Any]]:
        actions: list[dict[str, Any]] = []
        for interrupt in interrupts:
            value = getattr(interrupt, "value", {})
            for action in value.get("action_requests", []):
                if isinstance(action, dict):
                    actions.append(action)
        return actions

    def _extract_manual_approval_actions(self, messages: list[Any]) -> list[dict[str, Any]]:
        actions: list[dict[str, Any]] = []
        seen: set[tuple[str, str]] = set()
        for msg in messages:
            if not isinstance(msg, ToolMessage):
                continue
            output = self._tool_message_to_output(msg)
            if not output.get("requires_approval"):
                continue
            action_name = str(output.get("action_name", "") or msg.name or "")
            if not action_name:
                continue
            action_args = output.get("action_args", {})
            if not isinstance(action_args, dict):
                action_args = {}
            dedupe_key = (action_name, json.dumps(action_args, ensure_ascii=False, sort_keys=True))
            if dedupe_key in seen:
                continue
            seen.add(dedupe_key)
            actions.append({"name": action_name, "arguments": action_args})
        return actions

    def _execute_pending_manual_actions(
        self,
        session_id: str,
        actions: list[dict[str, Any]],
        edit_args: dict[str, Any] | None = None,
    ) -> list[dict[str, Any]]:
        outputs: list[dict[str, Any]] = []
        seen: set[tuple[str, str]] = set()
        for action in actions:
            tool_name = str(action.get("name", ""))
            action_args = action.get("arguments", {}) or action.get("args", {}) or {}
            if not isinstance(action_args, dict):
                action_args = {}
            if edit_args:
                action_args = {**action_args, **edit_args}
            dedupe_key = (tool_name, json.dumps(action_args, ensure_ascii=False, sort_keys=True))
            if dedupe_key in seen:
                continue
            seen.add(dedupe_key)
            result = self._execute_write_action(tool_name, action_args)
            outputs.append({"tool_name": tool_name, "tool_input": action_args, "tool_output": result})
            status = "ok" if result.get("ok") else "error"
            self.assistant_db.log_tool_call(
                ToolCallLogRecord(
                    session_id=session_id,
                    question="resume",
                    tool_name=tool_name,
                    tool_input=action_args,
                    tool_output=result,
                    status=status,
                )
            )
        return outputs

    def _execute_write_action(self, tool_name: str, action_args: dict[str, Any]) -> dict[str, Any]:
        if tool_name == "ota_create_request":
            return self.service.workflow_ota_request_create(
                int(action_args.get("device_id", 0)),
                str(action_args.get("firmware_id", "")),
                str(action_args.get("transport", "serial")),
                str(action_args.get("target", "127.0.0.1:19090")),
            )
        if tool_name == "approve_ota_request":
            return self.service.workflow_ota_request_confirm(
                str(action_args.get("approval_id", "")),
                str(action_args.get("approved_by", "operator")),
            )
        if tool_name == "pause_batch":
            return self.service.pause_batch_run(
                str(action_args.get("batch_run_id", "")),
                str(action_args.get("operator", "operator")),
            )
        if tool_name == "retry_failed_devices":
            return self.service.retry_batch_failed_devices(
                str(action_args.get("approval_id", "")),
                str(action_args.get("operator", "operator")),
            )
        return {"ok": False, "message": f"Unsupported manual write action: {tool_name}"}

    def _build_manual_resume_answer(self, outputs: list[dict[str, Any]], decision: str = "approve") -> str:
        if not outputs:
            return "没有可执行的待审批动作。"
        first = outputs[0]
        tool_name = str(first.get("tool_name", ""))
        result = first.get("tool_output", {})
        if not isinstance(result, dict):
            return "操作执行完成。"
        if result.get("ok"):
            message = str(result.get("message", "")).strip()
            action_text = "修改后执行完成" if decision == "edit" else "操作执行完成"
            if message:
                return f"{action_text}。\n工具: {tool_name}\n结果: {message}"
            return f"{action_text}。\n工具: {tool_name}"
        message = str(result.get("message", "Tool execution failed.")).strip()
        error = str(result.get("error", "")).strip()
        action_text = "修改后执行失败" if decision == "edit" else "操作执行失败"
        lines = [f"{action_text}。"]
        if tool_name:
            lines.append(f"工具: {tool_name}")
        lines.append(f"原因: {message}")
        if error:
            lines.append(f"错误: {error}")
        return "\n".join(lines)

    def _build_manual_resume_skipped_answer(self, decision: str, actions: list[dict[str, Any]]) -> str:
        if decision != "reject":
            return "待审批动作未执行。"
        first_action = actions[0] if actions else {}
        tool_name = str(first_action.get("name", ""))
        lines = ["审批已拒绝，待执行写操作未下发。"]
        if tool_name:
            lines.append(f"工具: {tool_name}")
        return "\n".join(lines)

    def _normalize_resume_decision(self, decision: str) -> str:
        return str(decision or "approve").strip().lower()

    def _build_resume_command(self, decision: str, edit_args: dict[str, Any] | None) -> Command | None:
        if decision == "approve":
            return Command(resume={"decisions": [{"type": "approve"}]})
        if decision == "reject":
            return Command(resume={"decisions": [{"type": "reject", "message": "审批人已拒绝"}]})
        if decision == "edit":
            return Command(resume={"decisions": [{"type": "edit", "editedAction": edit_args or {}}]})
        return None

    def _build_resume_answer(self, messages: list[Any], pending_actions: list[dict[str, Any]]) -> str:
        tool_messages = [msg for msg in messages if isinstance(msg, ToolMessage)]
        if not tool_messages:
            return ""
        failed_outputs: list[tuple[str, dict[str, Any]]] = []
        for idx, tool_message in enumerate(tool_messages):
            output = self._tool_message_to_output(tool_message)
            tool_name = tool_message.name or ""
            if not tool_name and idx < len(pending_actions):
                tool_name = str(pending_actions[idx].get("name", ""))
            if tool_message.status == "error" or output.get("ok") is False:
                failed_outputs.append((tool_name, output))
        if failed_outputs:
            tool_name, output = failed_outputs[0]
            message = str(output.get("message", "Tool execution failed."))
            error = str(output.get("error", "")).strip()
            lines = ["操作执行失败。"]
            if tool_name:
                lines.append(f"工具: {tool_name}")
            lines.append(f"原因: {message}")
            if error:
                lines.append(f"错误: {error}")
            return "\n".join(lines)
        return ""

    def _dedupe_tool_trace(self, tool_trace: list[dict[str, Any]]) -> list[dict[str, Any]]:
        deduped: list[dict[str, Any]] = []
        seen: set[tuple[str, str]] = set()
        for item in tool_trace:
            tool_name = str(item.get("tool", ""))
            tool_args = item.get("args", {})
            if not isinstance(tool_args, dict):
                tool_args = {}
            dedupe_key = (tool_name, json.dumps(tool_args, ensure_ascii=False, sort_keys=True))
            if dedupe_key in seen:
                continue
            seen.add(dedupe_key)
            deduped.append(
                {
                    "tool": tool_name,
                    "args": tool_args,
                    "step": len(deduped) + 1,
                }
            )
        return deduped

    def _tool_message_to_output(self, message: ToolMessage) -> dict[str, Any]:
        if isinstance(message.artifact, dict):
            return message.artifact
        content = message.content
        if isinstance(content, str):
            try:
                parsed = json.loads(content)
                if isinstance(parsed, dict):
                    return parsed
            except json.JSONDecodeError:
                return {"ok": message.status != "error", "content": content}
            return {"ok": message.status != "error", "content": content}
        return {
            "ok": message.status != "error",
            "content": self._stringify_content(content),
        }
