from __future__ import annotations

from pathlib import Path

from .policy_evaluator import DecisionProposal, PolicyEvaluator
from .policy_loader import load_default_policies


class DecisionEngine:
    def __init__(self, policy_path: Path) -> None:
        self._policy_path = policy_path
        self._policies = load_default_policies(self._policy_path)
        self._evaluator = PolicyEvaluator(self._policies)

    def evaluate_alert(self, alert: dict[str, object]) -> list[DecisionProposal]:
        return self._evaluator.evaluate(alert)

    def reload(self) -> list[dict[str, object]]:
        self._policies = load_default_policies(self._policy_path)
        self._evaluator = PolicyEvaluator(self._policies)
        return self.list_policies()

    def list_policies(self) -> list[dict[str, object]]:
        return [
            {
                "policy_id": item.policy_id,
                "version": item.version,
                "alert_rule_id": item.alert_rule_id,
                "action_type": item.action_type,
                "risk_level": item.risk_level,
                "requires_approval": item.requires_approval,
                "target_type": item.target_type,
                "title": item.title,
                "enabled": item.enabled,
                "notification_target": item.notification_target,
                "escalation_target": item.escalation_target,
            }
            for item in self._policies
        ]

    def first_policy_for_rule(self, alert_rule_id: str) -> dict[str, object] | None:
        for item in self.list_policies():
            if str(item.get("alert_rule_id", "")) == alert_rule_id and bool(item.get("enabled", True)):
                return item
        return None
