from __future__ import annotations

import json
from pathlib import Path

from .policy_models import DecisionPolicy


def _default_policy_dicts() -> list[dict[str, object]]:
    return [
        {
            "policy_id": "policy_device_offline_ticket",
            "version": "v1",
            "alert_rule_id": "device_offline_alert",
            "action_type": "create_ticket",
            "risk_level": "medium",
            "requires_approval": False,
            "target_type": "device",
            "title": "Create ticket for offline device",
            "enabled": True,
            "notification_target": "oncall_ops",
            "escalation_target": "ops_manager",
        },
        {
            "policy_id": "policy_sensor_gap_manual_review",
            "version": "v1",
            "alert_rule_id": "sensor_upload_gap_alert",
            "action_type": "manual_review",
            "risk_level": "medium",
            "requires_approval": True,
            "target_type": "device",
            "title": "Request manual review for telemetry gap",
            "enabled": True,
            "notification_target": "telemetry_oncall",
            "escalation_target": "telemetry_lead",
        },
        {
            "policy_id": "policy_ota_failure_pause_batch",
            "version": "v1",
            "alert_rule_id": "ota_failure_spike_alert",
            "action_type": "pause_batch",
            "risk_level": "critical",
            "requires_approval": True,
            "target_type": "ota_batch",
            "title": "Pause risky OTA batch rollout",
            "enabled": True,
            "notification_target": "ota_oncall",
            "escalation_target": "release_manager",
        },
    ]


def ensure_policy_file(policy_path: Path) -> None:
    policy_path.parent.mkdir(parents=True, exist_ok=True)
    if policy_path.exists():
        return
    policy_path.write_text(json.dumps(_default_policy_dicts(), ensure_ascii=False, indent=2), encoding="utf-8")


def load_default_policies(policy_path: Path) -> list[DecisionPolicy]:
    ensure_policy_file(policy_path)
    try:
        payload = json.loads(policy_path.read_text(encoding="utf-8"))
    except Exception:
        payload = _default_policy_dicts()
    policies: list[DecisionPolicy] = []
    for item in payload:
        if not isinstance(item, dict):
            continue
        policies.append(
            DecisionPolicy(
                policy_id=str(item.get("policy_id", "")).strip(),
                version=str(item.get("version", "v1")).strip() or "v1",
                alert_rule_id=str(item.get("alert_rule_id", "")).strip(),
                action_type=str(item.get("action_type", "")).strip(),
                risk_level=str(item.get("risk_level", "medium")).strip() or "medium",
                requires_approval=bool(item.get("requires_approval", False)),
                target_type=str(item.get("target_type", "")).strip(),
                title=str(item.get("title", "")).strip(),
                enabled=bool(item.get("enabled", True)),
                notification_target=str(item.get("notification_target", "oncall")).strip() or "oncall",
                escalation_target=str(item.get("escalation_target", "supervisor")).strip() or "supervisor",
            )
        )
    return [item for item in policies if item.policy_id and item.alert_rule_id and item.action_type]
