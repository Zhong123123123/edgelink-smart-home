from __future__ import annotations

from datetime import datetime, timezone
from typing import Any


class AgentExecutor:
    def execute(self, plan: dict[str, Any], service: Any, session_id: str = "") -> dict[str, Any]:
        results: list[dict[str, Any]] = []
        final_output: dict[str, Any] | None = None
        step_outputs: dict[int, dict[str, Any]] = {}
        aggregated_tool_trace: list[dict[str, Any]] = []
        failed_step_id: int | None = None
        for step in plan.get("steps", []):
            action = str(step.get("action", ""))
            args = self._resolve_args(dict(step.get("args", {})), step_outputs)
            if session_id:
                args["session_id_override"] = session_id
            started_at = datetime.now(timezone.utc).isoformat()
            output = self._run_action(action, args, service)
            finished_at = datetime.now(timezone.utc).isoformat()
            ok = bool(output.get("ok", True)) if isinstance(output, dict) else True
            failure_reason = ""
            if isinstance(output, dict) and not ok:
                failure_reason = str(output.get("message") or output.get("error") or "step failed")
            results.append(
                {
                    "step_id": step.get("step_id"),
                    "action": action,
                    "status": "success" if ok else "error",
                    "reason": step.get("reason", ""),
                    "description": step.get("description", ""),
                    "mode": step.get("mode", ""),
                    "args": args,
                    "ok": ok,
                    "failure_reason": failure_reason,
                    "started_at": started_at,
                    "finished_at": finished_at,
                    "output": output,
                }
            )
            final_output = output if isinstance(output, dict) else {"ok": True, "result": output}
            if isinstance(output, dict):
                step_outputs[int(step.get("step_id", len(results)))] = output
                aggregated_tool_trace.extend(output.get("tool_trace", []))
            if isinstance(output, dict) and not output.get("ok", True):
                failed_step_id = int(step.get("step_id", len(results)))
                break
        final_status = "ok" if failed_step_id is None else "error"
        execution_summary = {
            "completed_steps": len(results),
            "failed_step_id": failed_step_id,
            "final_status": final_status,
            "final_action": results[-1]["action"] if results else "",
            "tool_call_count": len(aggregated_tool_trace),
            "failure_reason": results[-1]["failure_reason"] if results and results[-1]["status"] == "error" else "",
        }
        return {
            "ok": final_status == "ok",
            "session_id": session_id,
            "goal": plan.get("goal", ""),
            "approval_required": plan.get("approval_required", False),
            "step_results": results,
            "final_output": final_output or {},
            "tool_trace": aggregated_tool_trace,
            "step_count": len(results),
            "execution_summary": execution_summary,
        }

    def _resolve_args(self, args: dict[str, Any], step_outputs: dict[int, dict[str, Any]]) -> dict[str, Any]:
        resolved = dict(args)
        reuse_step = resolved.pop("reuse_diagnosis_from_step", None)
        if reuse_step is not None:
            previous_output = step_outputs.get(int(reuse_step), {})
            if previous_output:
                resolved["diagnosis_result_override"] = previous_output
        return resolved

    def _run_action(self, action: str, args: dict[str, Any], service: Any) -> Any:
        if action == "chat":
            return service.chat(str(args.get("question", "")), session_id_override=str(args.get("session_id_override", "")))
        if action == "workflow_diagnose_fault":
            return service.workflow_diagnose_fault(
                int(args.get("device_id", 0)),
                int(args.get("limit", 10)),
                session_id_override=str(args.get("session_id_override", "")),
            )
        if action == "workflow_generate_report":
            return service.workflow_generate_report(
                report_type=str(args.get("report_type", "fault_ticket")),
                device_id=int(args.get("device_id")) if args.get("device_id") is not None else None,
                task_id=str(args.get("task_id", "")),
                limit=int(args.get("limit", 10)),
                diagnosis_result_override=args.get("diagnosis_result_override"),
                session_id_override=str(args.get("session_id_override", "")),
            )
        if action == "workflow_ota_risk_check":
            return service.workflow_ota_risk_check(
                int(args.get("device_id", 0)),
                str(args.get("firmware_id", "")),
                session_id_override=str(args.get("session_id_override", "")),
            )
        if action == "workflow_ota_request_create":
            return service.workflow_ota_request_create(
                int(args.get("device_id", 0)),
                str(args.get("firmware_id", "")),
                str(args.get("transport", "serial")),
                str(args.get("target", "127.0.0.1:19090")),
                session_id_override=str(args.get("session_id_override", "")),
            )
        if action == "workflow_ota_request_confirm":
            return service.workflow_ota_request_confirm(
                approval_id=str(args.get("approval_id", "")),
                approved_by=str(args.get("approved_by", "operator")),
                session_id_override=str(args.get("session_id_override", "")),
            )
        if action == "workflow_ota_batch_plan":
            return service.workflow_ota_batch_plan(
                device_ids=list(args.get("device_ids", [])),
                firmware_id=str(args.get("firmware_id", "")),
                transport=str(args.get("transport", "serial")),
                target=str(args.get("target", "127.0.0.1:19090")),
                batch_size=int(args.get("batch_size", 2)),
                session_id_override=str(args.get("session_id_override", "")),
            )
        if action == "workflow_ota_batch_request_create":
            return service.workflow_ota_batch_request_create(
                device_ids=list(args.get("device_ids", [])),
                firmware_id=str(args.get("firmware_id", "")),
                transport=str(args.get("transport", "serial")),
                target=str(args.get("target", "127.0.0.1:19090")),
                batch_size=int(args.get("batch_size", 2)),
                session_id_override=str(args.get("session_id_override", "")),
            )
        if action == "start_next_batch":
            return service.start_next_batch(
                batch_run_id=str(args.get("batch_run_id", "")),
                operator=str(args.get("operator", "operator")),
                session_id_override=str(args.get("session_id_override", "")),
            )
        if action == "pause_batch_run":
            return service.pause_batch_run(
                batch_run_id=str(args.get("batch_run_id", "")),
                operator=str(args.get("operator", "operator")),
            )
        if action == "terminate_batch_run":
            return service.terminate_batch_run(
                batch_run_id=str(args.get("batch_run_id", "")),
                operator=str(args.get("operator", "operator")),
            )
        if action == "refresh_batch_run":
            return service.refresh_batch_run(str(args.get("batch_run_id", "")))
        if action == "retry_batch_failed_devices":
            return service.retry_batch_failed_devices(
                approval_id=str(args.get("approval_id", "")),
                operator=str(args.get("operator", "operator")),
                session_id_override=str(args.get("session_id_override", "")),
            )
        if action == "reopen_ticket":
            return service.reopen_ticket(str(args.get("ticket_id", "")))
        return {"ok": False, "message": f"Unsupported agent action: {action}"}
