from __future__ import annotations

from dataclasses import dataclass

from ..realtime.event_models import EventEnvelope


@dataclass(frozen=True)
class AlertProposal:
    rule_id: str
    title: str
    severity: str
    summary: str
    dedupe_key: str
    payload: dict[str, object]


class AlertPipeline:
    def evaluate(self, event: EventEnvelope) -> list[AlertProposal]:
        proposals: list[AlertProposal] = []
        if event.event_type == "device_offline":
            proposals.append(
                AlertProposal(
                    rule_id="device_offline_alert",
                    title="Device Offline",
                    severity="high" if event.severity in {"warning", "high"} else "critical",
                    summary=f"Device {event.device_id} appears offline and requires investigation.",
                    dedupe_key=event.dedupe_subject,
                    payload={"category": "availability", **event.payload},
                )
            )
        elif event.event_type == "sensor_upload_gap":
            proposals.append(
                AlertProposal(
                    rule_id="sensor_upload_gap_alert",
                    title="Sensor Upload Gap",
                    severity="medium" if event.severity == "info" else event.severity,
                    summary=f"Sensor uploads slowed down for device {event.device_id or 'unknown'}.",
                    dedupe_key=event.dedupe_subject,
                    payload={"category": "telemetry", **event.payload},
                )
            )
        elif event.event_type == "ota_failure_spike":
            proposals.append(
                AlertProposal(
                    rule_id="ota_failure_spike_alert",
                    title="OTA Failure Spike",
                    severity="critical",
                    summary="OTA failure rate spiked above the configured threshold.",
                    dedupe_key=event.dedupe_subject,
                    payload={"category": "ota", **event.payload},
                )
            )
        return proposals
