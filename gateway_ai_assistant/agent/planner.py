from __future__ import annotations

import re
from typing import Any

from .tool_registry import TOOL_REGISTRY


class AgentPlanner:
    def plan(self, question: str) -> dict[str, Any]:
        normalized = question.strip().lower()
        limit = self._extract_limit(question)
        device_id = self._extract_device_id(question)
        task_id = self._extract_task_id(question) or ""
        firmware_id = self._extract_firmware_id(question) or ""
        approval_id = self._extract_approval_id(question) or ""
        batch_run_id = self._extract_batch_run_id(question) or ""
        ticket_id = self._extract_ticket_id(question) or ""
        operator = self._extract_operator(question) or "operator"
        device_ids = self._extract_device_ids(question)

        wants_diagnosis = "诊断" in question or "排查" in question or "故障" in question
        wants_report = "报告" in question or "工单" in question
        wants_ota_create = "ota" in normalized and ("创建" in question or "升级" in question)
        wants_ota_risk = "ota" in normalized and "风险" in question
        wants_batch = any(word in question for word in ("批量", "灰度", "分批", "batch"))
        wants_ota_confirm = (
            ("确认" in question or "批准" in question or "approve" in normalized)
            and "ota" in normalized
            and bool(approval_id)
        )
        wants_start_next_batch = ("下一批" in question or "start next batch" in normalized or "推进批次" in question) and bool(batch_run_id)
        wants_refresh_batch = ("刷新" in question or "refresh" in normalized) and ("batch" in normalized or "批量" in question) and bool(batch_run_id)
        wants_retry_failed = ("重试" in question or "retry" in normalized) and bool(approval_id)
        wants_reopen_ticket = ("重开" in question or "重新打开" in question or "reopen" in normalized) and bool(ticket_id)
        wants_pause_batch = ("暂停" in question or "pause" in normalized) and bool(batch_run_id)
        wants_terminate_batch = ("终止" in question or "停止后续批次" in question or "terminate" in normalized) and bool(batch_run_id)

        if wants_pause_batch:
            return self._build_plan(
                goal="ota_batch_pause",
                question=question,
                approval_required=True,
                steps=[
                    self._step(
                        1,
                        "pause_batch_run",
                        {"batch_run_id": batch_run_id, "operator": operator},
                        "在当前批次完成后暂停灰度推进，避免自动进入下一批。",
                    )
                ],
            )

        if wants_terminate_batch:
            return self._build_plan(
                goal="ota_batch_terminate",
                question=question,
                approval_required=True,
                steps=[
                    self._step(
                        1,
                        "terminate_batch_run",
                        {"batch_run_id": batch_run_id, "operator": operator},
                        "终止后续批次的推进，保留已经发出的 OTA 任务记录。",
                    )
                ],
            )

        if wants_start_next_batch:
            return self._build_plan(
                goal="ota_batch_start_next",
                question=question,
                approval_required=True,
                steps=[
                    self._step(
                        1,
                        "start_next_batch",
                        {"batch_run_id": batch_run_id, "operator": operator},
                        "在前一批成功后启动下一批 OTA 下发，保持真正的灰度推进节奏。",
                    )
                ],
            )

        if wants_refresh_batch:
            return self._build_plan(
                goal="ota_batch_refresh",
                question=question,
                approval_required=False,
                steps=[
                    self._step(
                        1,
                        "refresh_batch_run",
                        {"batch_run_id": batch_run_id},
                        "刷新批量执行进度，把已有 OTA task 状态同步回 batch run。",
                    )
                ],
            )

        if wants_retry_failed:
            return self._build_plan(
                goal="ota_batch_retry_failed",
                question=question,
                approval_required=True,
                steps=[
                    self._step(
                        1,
                        "retry_batch_failed_devices",
                        {"approval_id": approval_id, "operator": operator},
                        "针对批量 OTA 中失败的设备执行重试，避免直接重跑整批。",
                    )
                ],
            )

        if wants_reopen_ticket:
            return self._build_plan(
                goal="ticket_reopen",
                question=question,
                approval_required=True,
                steps=[
                    self._step(
                        1,
                        "reopen_ticket",
                        {"ticket_id": ticket_id},
                        "把已关闭工单恢复为 open，继续跟踪处理。",
                    )
                ],
            )

        if wants_diagnosis and wants_report and device_id is not None:
            return self._build_plan(
                goal="diagnose_and_generate_report",
                question=question,
                approval_required=False,
                steps=[
                    self._step(
                        1,
                        "workflow_diagnose_fault",
                        {"device_id": device_id, "limit": limit},
                        "先基于运行态数据做确定性故障诊断，确认是否存在离线、CRC、OTA 失败等信号。",
                    ),
                    self._step(
                        2,
                        "workflow_generate_report",
                        {
                            "report_type": "fault_ticket",
                            "device_id": device_id,
                            "task_id": task_id,
                            "limit": limit,
                            "reuse_diagnosis_from_step": 1,
                        },
                        "再把诊断结论和运行态证据写入故障报告，形成可留档结果。",
                    ),
                ],
            )

        if wants_diagnosis and device_id is not None:
            return self._build_plan(
                goal="diagnose_fault",
                question=question,
                approval_required=False,
                steps=[self._step(1, "workflow_diagnose_fault", {"device_id": device_id, "limit": limit}, "基于设备状态、历史上报、系统事件和 OTA 历史做规则诊断。")],
            )

        if wants_report and device_id is not None:
            return self._build_plan(
                goal="generate_report",
                question=question,
                approval_required=False,
                steps=[
                    self._step(
                        1,
                        "workflow_generate_report",
                        {
                            "report_type": "fault_ticket" if "工单" in question or "故障" in question else "test_report",
                            "device_id": device_id,
                            "task_id": task_id,
                            "limit": limit,
                        },
                        "生成结构化 Markdown/JSON 报告并持久化到本地报告中心。",
                    )
                ],
            )

        if wants_ota_confirm:
            return self._build_plan(
                goal="ota_confirm_request",
                question=question,
                approval_required=False,
                steps=[
                    self._step(
                        1,
                        "workflow_ota_request_confirm",
                        {"approval_id": approval_id, "approved_by": operator},
                        "根据已存在的审批请求执行最终确认，并创建真实 OTA 任务。",
                    )
                ],
            )

        if wants_batch and wants_ota_create and wants_ota_risk and device_ids and firmware_id:
            return self._build_plan(
                goal="ota_batch_risk_then_request_create",
                question=question,
                approval_required=True,
                steps=[
                    self._step(
                        1,
                        "workflow_ota_batch_plan",
                        {
                            "device_ids": device_ids,
                            "firmware_id": firmware_id,
                            "transport": "serial",
                            "target": "127.0.0.1:19090",
                            "batch_size": min(2, len(device_ids)),
                        },
                        "先为多台设备生成分批 / 灰度 OTA 计划，区分可执行设备和阻塞设备。",
                    ),
                    self._step(
                        2,
                        "workflow_ota_batch_request_create",
                        {
                            "device_ids": device_ids,
                            "firmware_id": firmware_id,
                            "transport": "serial",
                            "target": "127.0.0.1:19090",
                            "batch_size": min(2, len(device_ids)),
                        },
                        "在批量风险检查完成后创建审批请求，但仍不直接下发真实 OTA 任务。",
                    ),
                ],
            )

        if wants_batch and device_ids and firmware_id:
            return self._build_plan(
                goal="ota_batch_plan",
                question=question,
                approval_required=False,
                steps=[
                    self._step(
                        1,
                        "workflow_ota_batch_plan",
                        {
                            "device_ids": device_ids,
                            "firmware_id": firmware_id,
                            "transport": "serial",
                            "target": "127.0.0.1:19090",
                            "batch_size": min(2, len(device_ids)),
                        },
                        "生成批量 / 灰度 OTA 分批计划，作为后续审批和执行依据。",
                    )
                ],
            )

        if wants_ota_risk and device_id is not None and firmware_id:
            return self._build_plan(
                goal="ota_risk_check",
                question=question,
                approval_required=False,
                steps=[self._step(1, "workflow_ota_risk_check", {"device_id": device_id, "firmware_id": firmware_id}, "先校验设备在线状态、固件 manifest 和最近 OTA 历史，判断升级风险。")],
            )

        if wants_ota_create and wants_ota_risk and device_id is not None and firmware_id:
            return self._build_plan(
                goal="ota_risk_then_request_create",
                question=question,
                approval_required=True,
                steps=[
                    self._step(
                        1,
                        "workflow_ota_risk_check",
                        {"device_id": device_id, "firmware_id": firmware_id},
                        "先做 OTA 风险检查，确认设备与固件条件是否满足。",
                    ),
                    self._step(
                        2,
                        "workflow_ota_request_create",
                        {
                            "device_id": device_id,
                            "firmware_id": firmware_id,
                            "transport": "serial",
                            "target": "127.0.0.1:19090",
                        },
                        "风险检查完成后创建审批请求，但不会直接下发 OTA 任务。",
                    ),
                ],
            )

        if wants_ota_create and device_id is not None and firmware_id:
            return self._build_plan(
                goal="ota_request_create",
                question=question,
                approval_required=True,
                steps=[
                    self._step(
                        1,
                        "workflow_ota_request_create",
                        {
                            "device_id": device_id,
                            "firmware_id": firmware_id,
                            "transport": "serial",
                            "target": "127.0.0.1:19090",
                        },
                        "直接创建 OTA 审批请求，后续仍需人工确认才能真正下发任务。",
                    )
                ],
            )

        return self._build_plan(
            goal="chat",
            question=question,
            approval_required=False,
            steps=[self._step(1, "chat", {"question": question}, "使用现有智能助手路由处理通用查询。")],
        )

    def _build_plan(self, goal: str, question: str, approval_required: bool, steps: list[dict[str, Any]]) -> dict[str, Any]:
        return {
            "ok": True,
            "goal": goal,
            "question": question,
            "approval_required": approval_required,
            "step_count": len(steps),
            "steps": steps,
            "plan_summary": " -> ".join(f"{step['step_id']}.{step['action']}" for step in steps),
        }

    def _step(self, step_id: int, action: str, args: dict[str, Any], reason: str) -> dict[str, Any]:
        return {
            "step_id": step_id,
            "action": action,
            "args": args,
            "mode": TOOL_REGISTRY[action]["mode"],
            "reason": reason,
            "description": TOOL_REGISTRY[action]["description"],
        }

    def _extract_limit(self, question: str) -> int:
        match = re.search(r"最近\s*(\d+)\s*条", question)
        if not match:
            match = re.search(r"(\d+)\s*条", question)
        return max(1, min(int(match.group(1)), 20)) if match else 10

    def _extract_device_id(self, question: str) -> int | None:
        patterns = (
            r"device_id\s*=\s*(\d+)",
            r"(?:设备|device)\s*(\d+)",
        )
        for pattern in patterns:
            match = re.search(pattern, question, flags=re.IGNORECASE)
            if match:
                return int(match.group(1))
        return None

    def _extract_task_id(self, question: str) -> str | None:
        match = re.search(r"task_id\s*=\s*([A-Za-z0-9._:-]+)", question, flags=re.IGNORECASE)
        return match.group(1) if match else None

    def _extract_device_ids(self, question: str) -> list[int]:
        match = re.search(r"device_ids\s*=\s*([0-9,\s]+)", question, flags=re.IGNORECASE)
        if not match:
            return []
        raw = match.group(1)
        result = []
        for item in raw.split(","):
            text = item.strip()
            if text.isdigit():
                result.append(int(text))
        return result

    def _extract_firmware_id(self, question: str) -> str | None:
        match = re.search(r"firmware_id\s*=\s*([A-Za-z0-9._:-]+)", question, flags=re.IGNORECASE)
        return match.group(1) if match else None

    def _extract_approval_id(self, question: str) -> str | None:
        patterns = (
            r"approval_id\s*=\s*([A-Za-z0-9._:-]+)",
            r"审批\s*([A-Za-z0-9._:-]+)",
            r"approval\s+([A-Za-z0-9._:-]+)",
        )
        for pattern in patterns:
            match = re.search(pattern, question, flags=re.IGNORECASE)
            if match:
                return match.group(1)
        return None

    def _extract_batch_run_id(self, question: str) -> str | None:
        patterns = (
            r"batch_run_id\s*=\s*([A-Za-z0-9._:-]+)",
            r"batch-run[-_A-Za-z0-9.:]+",
        )
        for pattern in patterns:
            match = re.search(pattern, question, flags=re.IGNORECASE)
            if match:
                value = match.group(1) if match.lastindex else match.group(0)
                return value
        return None

    def _extract_ticket_id(self, question: str) -> str | None:
        match = re.search(r"ticket_id\s*=\s*([A-Za-z0-9._:-]+)", question, flags=re.IGNORECASE)
        return match.group(1) if match else None

    def _extract_operator(self, question: str) -> str | None:
        patterns = (
            r"approved_by\s*=\s*([A-Za-z0-9._:-]+)",
            r"operator\s*=\s*([A-Za-z0-9._:-]+)",
            r"由\s*([A-Za-z0-9._:-]+)\s*确认",
        )
        for pattern in patterns:
            match = re.search(pattern, question, flags=re.IGNORECASE)
            if match:
                return match.group(1)
        return None
