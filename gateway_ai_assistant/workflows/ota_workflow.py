from __future__ import annotations

from typing import Any


class OtaWorkflow:
    ACTIVE_STATES = {
        "CREATED",
        "WAIT_DEVICE_ONLINE",
        "PRECHECK",
        "PREPARE_DEVICE",
        "TRANSFERRING",
        "VERIFYING",
        "COMMITTING",
        "REBOOTING",
        "VERSION_CHECK",
        "HEALTH_CONFIRM",
    }

    def build_risk_check(
        self,
        device_id: int,
        firmware_id: str,
        device_status: dict[str, Any],
        firmware_manifest_result: dict[str, Any],
        recent_ota_tasks_result: dict[str, Any],
    ) -> dict[str, Any]:
        checks: list[dict[str, Any]] = []

        device_online = bool(device_status.get("ok") and device_status.get("status") == "online")
        checks.append(
            {
                "name": "device_online",
                "passed": device_online,
                "detail": device_status.get("message", "device status unavailable"),
            }
        )

        firmware_exists = bool(firmware_manifest_result.get("ok"))
        manifest = firmware_manifest_result.get("manifest", {}) if firmware_exists else {}
        checks.append(
            {
                "name": "firmware_exists",
                "passed": firmware_exists,
                "detail": firmware_manifest_result.get("message", "manifest unavailable"),
            }
        )

        recent_tasks = recent_ota_tasks_result.get("tasks", []) if recent_ota_tasks_result.get("ok") else []
        active_tasks = [task for task in recent_tasks if task.get("state") in self.ACTIVE_STATES]
        checks.append(
            {
                "name": "pending_ota_tasks",
                "passed": len(active_tasks) == 0,
                "detail": f"active task count={len(active_tasks)}",
            }
        )

        failed_tasks = [task for task in recent_tasks if task.get("state") == "FAILED"]
        checks.append(
            {
                "name": "recent_failed_ota_tasks",
                "passed": len(failed_tasks) == 0,
                "detail": f"recent failed task count={len(failed_tasks)}",
            }
        )

        latest_task = recent_tasks[0] if recent_tasks else None
        manifest_device_type = manifest.get("device_type", "")
        latest_device_type = latest_task.get("device_type", "") if latest_task else ""
        compatible = True
        compatibility_detail = "device type could not be cross-checked from OTA history"
        if manifest_device_type and latest_device_type:
            compatible = manifest_device_type == latest_device_type
            compatibility_detail = (
                f"manifest device_type={manifest_device_type}, latest OTA device_type={latest_device_type}"
            )
        elif manifest_device_type:
            compatibility_detail = f"manifest device_type={manifest_device_type}, latest OTA task unavailable"
        checks.append(
            {
                "name": "device_type_compatibility",
                "passed": compatible,
                "detail": compatibility_detail,
            }
        )

        failed_count = sum(1 for item in checks if not item["passed"])
        if failed_count == 0:
            risk_level = "low"
        elif failed_count == 1:
            risk_level = "medium"
        else:
            risk_level = "high"

        return {
            "ok": True,
            "device_id": device_id,
            "firmware_id": firmware_id,
            "risk_level": risk_level,
            "checks": checks,
            "device_status": device_status,
            "firmware_manifest": firmware_manifest_result,
            "recent_ota_tasks": recent_ota_tasks_result,
            "message": "OTA risk check completed.",
        }

    def build_upgrade_plan(
        self,
        device_id: int,
        firmware_id: str,
        risk_result: dict[str, Any],
    ) -> dict[str, Any]:
        checks = risk_result.get("checks", [])
        blocking_items = [item for item in checks if not item.get("passed")]

        steps = [
            "确认目标设备在线且最近上报时间正常。",
            "确认目标固件 manifest 存在且与设备类型兼容。",
            "确认当前没有未完成的 OTA 任务。",
            "在低峰时段执行 OTA，执行前记录当前版本与关键状态。",
            "人工确认后再调用现有网关 /api/ota/tasks 创建任务。",
            "执行后持续观察 OTA 事件轨迹与设备恢复状态。",
        ]

        approval_required = True
        ready_to_create = len(blocking_items) == 0

        return {
            "ok": True,
            "device_id": device_id,
            "firmware_id": firmware_id,
            "risk_level": risk_result.get("risk_level", "unknown"),
            "approval_required": approval_required,
            "ready_to_create": ready_to_create,
            "blocking_items": blocking_items,
            "steps": steps,
            "message": "OTA upgrade plan was generated from the latest risk assessment.",
        }
