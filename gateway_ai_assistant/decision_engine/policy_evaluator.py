from __future__ import annotations

from dataclasses import dataclass

from .policy_models import DecisionPolicy


@dataclass(frozen=True)
class DecisionProposal:
    policy: DecisionPolicy
    reason: str
    payload: dict[str, object]
    target_id: str


class PolicyEvaluator:
    def __init__(self, policies: list[DecisionPolicy]) -> None:
        self._policies = policies

    def evaluate(self, alert: dict[str, object]) -> list[DecisionProposal]:
        proposals: list[DecisionProposal] = []
        rule_id = str(alert.get("rule_id", ""))
        payload = alert.get("payload", {})
        for policy in self._policies:
            if policy.alert_rule_id != rule_id:
                continue
            target_id = ""
            if policy.target_type == "device" and alert.get("device_id") is not None:
                target_id = str(alert.get("device_id"))
            elif policy.target_type == "ota_batch":
                target_id = str(payload.get("batch_run_id", ""))
            proposals.append(
                DecisionProposal(
                    policy=policy,
                    reason=f"Matched policy {policy.policy_id} from alert rule {rule_id}.",
                    payload={"alert_summary": alert.get("summary", ""), "alert_payload": payload, "policy_title": policy.title},
                    target_id=target_id,
                )
            )
        return proposals
