from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class DecisionPolicy:
    policy_id: str
    version: str
    alert_rule_id: str
    action_type: str
    risk_level: str
    requires_approval: bool
    target_type: str
    title: str
    enabled: bool = True
    notification_target: str = "oncall"
    escalation_target: str = "supervisor"
