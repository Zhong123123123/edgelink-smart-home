from __future__ import annotations

from typing import Any


class ReportWorkflow:
    def build_report(
        self,
        report_type: str,
        device_id: int | None,
        limit: int,
        device_status: dict[str, Any] | None,
        sensor_history: dict[str, Any] | None,
        system_events: dict[str, Any] | None,
        ota_status: dict[str, Any] | None,
        diagnosis_result: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        normalized_type = report_type.strip() or "fault_ticket"
        title = self._title(normalized_type, device_id)
        summary = self._summary(normalized_type, device_status, sensor_history, system_events, ota_status, diagnosis_result)
        sections = self._sections(device_status, sensor_history, system_events, ota_status, diagnosis_result, limit)
        markdown = self._markdown(title, normalized_type, summary, sections)
        return {
            "ok": True,
            "report_type": normalized_type,
            "title": title,
            "summary": summary,
            "sections": sections,
            "markdown": markdown,
        }

    def _title(self, report_type: str, device_id: int | None) -> str:
        suffix = f"DEVICE_{device_id:03d}" if device_id is not None else "Gateway"
        if report_type == "test_report":
            return f"{suffix} OTA / Runtime Test Report"
        return f"{suffix} Fault Ticket"

    def _summary(
        self,
        report_type: str,
        device_status: dict[str, Any] | None,
        sensor_history: dict[str, Any] | None,
        system_events: dict[str, Any] | None,
        ota_status: dict[str, Any] | None,
        diagnosis_result: dict[str, Any] | None,
    ) -> str:
        status = (device_status or {}).get("status", "unknown")
        sensor_count = int((sensor_history or {}).get("count", 0) or 0)
        event_count = int((system_events or {}).get("count", 0) or 0)
        diagnosis_summary = ""
        if diagnosis_result and diagnosis_result.get("finding_count", 0):
            diagnosis_summary = (
                f" diagnosis={diagnosis_result.get('overall_severity', 'unknown')}"
                f" findings={diagnosis_result.get('finding_count', 0)};"
            )
        if report_type == "test_report":
            return (
                f"Device status={status}; sampled sensor records={sensor_count}; "
                f"sampled system events={event_count}; "
                f"ota_task={'present' if ota_status else 'absent'};{diagnosis_summary}".strip()
            )
        return (
            f"Suspected issue scope: device_status={status}; recent sensor samples={sensor_count}; "
            f"recent system events={event_count}; ota_context={'present' if ota_status else 'absent'};"
            f"{diagnosis_summary}".strip()
        )

    def _sections(
        self,
        device_status: dict[str, Any] | None,
        sensor_history: dict[str, Any] | None,
        system_events: dict[str, Any] | None,
        ota_status: dict[str, Any] | None,
        diagnosis_result: dict[str, Any] | None,
        limit: int,
    ) -> list[dict[str, Any]]:
        sections: list[dict[str, Any]] = []
        if device_status:
            sections.append(
                {
                    "name": "device_status",
                    "content": {
                        "status": device_status.get("status"),
                        "status_source": device_status.get("status_source"),
                        "last_seen_readable_time": device_status.get("last_seen_readable_time"),
                        "latest_ota_task": device_status.get("latest_ota_task"),
                        "message": device_status.get("message"),
                    },
                }
            )
        if sensor_history:
            sections.append(
                {
                    "name": "sensor_history",
                    "content": {
                        "count": sensor_history.get("count", 0),
                        "records": sensor_history.get("records", [])[: min(limit, 5)],
                    },
                }
            )
        if system_events:
            sections.append(
                {
                    "name": "system_events",
                    "content": {
                        "count": system_events.get("count", 0),
                        "records": system_events.get("records", [])[: min(limit, 8)],
                    },
                }
            )
        if ota_status:
            sections.append(
                {
                    "name": "ota_status",
                    "content": {
                        "task": ota_status.get("task"),
                        "total_events": ota_status.get("total_events"),
                        "events": ota_status.get("events", [])[:5],
                    },
                }
            )
        if diagnosis_result:
            sections.append(
                {
                    "name": "diagnosis",
                    "content": {
                        "overall_severity": diagnosis_result.get("overall_severity"),
                        "finding_count": diagnosis_result.get("finding_count"),
                        "summary": diagnosis_result.get("summary"),
                        "findings": diagnosis_result.get("findings", [])[: min(limit, 8)],
                    },
                }
            )
        return sections

    def _markdown(self, title: str, report_type: str, summary: str, sections: list[dict[str, Any]]) -> str:
        lines = [
            f"# {title}",
            "",
            f"- report_type: {report_type}",
            f"- summary: {summary}",
            "",
        ]
        for section in sections:
            lines.append(f"## {section['name']}")
            lines.append("```json")
            lines.append(str(section["content"]))
            lines.append("```")
            lines.append("")
        return "\n".join(lines).strip()
