from __future__ import annotations

from typing import Any


class DiagnosticsAnalyzer:
    def analyze(
        self,
        device_id: int,
        device_status: dict[str, Any],
        sensor_history: dict[str, Any],
        system_events: dict[str, Any],
        recent_ota_tasks: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        findings: list[dict[str, Any]] = []
        event_records = self._device_events(system_events.get("records", []), device_id)
        sensor_count = int(sensor_history.get("count", 0) or 0)
        device_state = str(device_status.get("status", "unknown"))
        status_source = str(device_status.get("status_source", "unknown"))

        if device_state == "offline":
            severity = "high" if sensor_count == 0 else "medium"
            findings.append(
                {
                    "code": "device_offline",
                    "severity": severity,
                    "title": "Device is offline",
                    "reason": f"Current status is offline; status_source={status_source}.",
                    "recommendation": "Check power, link status, and whether the gateway still receives reports from this device.",
                }
            )

        if sensor_count == 0:
            findings.append(
                {
                    "code": "no_recent_sensor_data",
                    "severity": "high" if device_state == "offline" else "medium",
                    "title": "No recent sensor data",
                    "reason": "Recent sensor history returned zero records.",
                    "recommendation": "Check whether the device stopped reporting, the serial/WiFi link failed, or history ingestion is blocked.",
                }
            )

        if any(str(item.get("event_type", "")) == "device_offline" for item in event_records):
            findings.append(
                {
                    "code": "device_offline_event",
                    "severity": "medium",
                    "title": "Offline event detected",
                    "reason": "Recent system events include device_offline for this device.",
                    "recommendation": "Compare offline timestamps with the last sensor report and inspect gateway connectivity logs.",
                }
            )

        if any("heartbeat" in str(item.get("event_type", "")).lower() and "timeout" in str(item.get("detail", "")).lower() for item in event_records):
            findings.append(
                {
                    "code": "heartbeat_timeout",
                    "severity": "high",
                    "title": "Heartbeat timeout suspected",
                    "reason": "Recent system events contain heartbeat-related timeout signals.",
                    "recommendation": "Check heartbeat interval settings, TCP binary timeout settings, and transport stability.",
                }
            )

        if any(str(item.get("event_type", "")).lower() == "crc_error" for item in event_records):
            findings.append(
                {
                    "code": "crc_error",
                    "severity": "medium",
                    "title": "CRC errors observed",
                    "reason": "Recent system events include crc_error.",
                    "recommendation": "Check serial signal quality, baudrate consistency, frame integrity, and OTA image transfer stability.",
                }
            )

        ota_tasks = (recent_ota_tasks or {}).get("tasks", [])
        failed_ota = [item for item in ota_tasks if str(item.get("state", "")).upper() == "FAILED"]
        if len(failed_ota) >= 2:
            findings.append(
                {
                    "code": "repeated_ota_failures",
                    "severity": "high",
                    "title": "Repeated OTA failures",
                    "reason": f"Recent OTA history contains {len(failed_ota)} failed tasks.",
                    "recommendation": "Pause rollout, inspect failure reasons, verify firmware manifest, transport path, and device bootloader state.",
                }
            )

        overall_severity = self._overall_severity(findings)
        return {
            "ok": True,
            "device_id": device_id,
            "overall_severity": overall_severity,
            "finding_count": len(findings),
            "findings": findings,
            "summary": self._summary(overall_severity, findings),
        }

    def _device_events(self, records: list[dict[str, Any]], device_id: int) -> list[dict[str, Any]]:
        result = []
        for item in records:
            if int(item.get("device_id", 0) or 0) == device_id:
                result.append(item)
        return result

    def _overall_severity(self, findings: list[dict[str, Any]]) -> str:
        if any(item.get("severity") == "high" for item in findings):
            return "high"
        if any(item.get("severity") == "medium" for item in findings):
            return "medium"
        if any(item.get("severity") == "low" for item in findings):
            return "low"
        return "info"

    def _summary(self, overall_severity: str, findings: list[dict[str, Any]]) -> str:
        if not findings:
            return "No deterministic fault signals were detected from current runtime data."
        codes = ", ".join(str(item.get("code", "")) for item in findings[:4])
        return f"Detected {len(findings)} diagnostic findings; overall_severity={overall_severity}; top_signals={codes}."

