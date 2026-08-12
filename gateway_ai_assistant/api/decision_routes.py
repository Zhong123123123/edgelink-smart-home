from __future__ import annotations

from fastapi import APIRouter, HTTPException

from ..models import AlertActionRequest, DecisionActionRequest, RealtimeEventRequest
from ..service import AssistantService


def create_decision_router(service: AssistantService) -> APIRouter:
    router = APIRouter()

    @router.post("/assistant/realtime/events")
    def ingest_realtime_event(payload: RealtimeEventRequest) -> dict:
        return service.ingest_realtime_event(
            payload.event_type,
            source=payload.source,
            severity=payload.severity,
            device_id=payload.device_id,
            payload=payload.payload,
            happened_at=payload.happened_at,
        )

    @router.get("/assistant/realtime/events")
    def list_realtime_events(limit: int = 50, event_type: str = "", severity: str = "", device_id: int | None = None) -> dict:
        return service.list_realtime_events(limit=limit, event_type=event_type, severity=severity, device_id=device_id)

    @router.get("/assistant/alerts")
    def list_alerts(limit: int = 50, status: str = "", severity: str = "") -> dict:
        return service.list_alert_records(limit=limit, status=status, severity=severity)

    @router.get("/assistant/alerts/notifications")
    def list_notifications(limit: int = 50, status: str = "", severity: str = "") -> dict:
        return service.list_notification_records(limit=limit, status=status, severity=severity)

    @router.post("/assistant/alerts/{alert_id}/suppress")
    def suppress_alert(alert_id: str, payload: AlertActionRequest) -> dict:
        result = service.suppress_alert(
            alert_id,
            actor=payload.actor,
            minutes=payload.suppress_minutes or 30,
            reason=f"Suppressed by {payload.actor}",
        )
        if not result.get("ok"):
            status_code = 403 if result.get("required_role") else 404
            raise HTTPException(status_code=status_code, detail=result.get("message", "Alert was not found."))
        return result

    @router.post("/assistant/alerts/{alert_id}/ack")
    def acknowledge_alert(alert_id: str, payload: AlertActionRequest) -> dict:
        result = service.acknowledge_alert(alert_id, actor=payload.actor)
        if not result.get("ok"):
            status_code = 403 if result.get("required_role") else 404
            raise HTTPException(status_code=status_code, detail=result.get("message", "Alert was not found."))
        return result

    @router.post("/assistant/alerts/{alert_id}/resolve")
    def resolve_alert(alert_id: str, payload: AlertActionRequest) -> dict:
        result = service.resolve_alert(alert_id, actor=payload.actor)
        if not result.get("ok"):
            status_code = 403 if result.get("required_role") else 404
            raise HTTPException(status_code=status_code, detail=result.get("message", "Alert was not found."))
        return result

    @router.get("/assistant/decisions")
    def list_decisions(limit: int = 50, status: str = "", risk_level: str = "") -> dict:
        return service.list_decision_records(limit=limit, status=status, risk_level=risk_level)

    @router.get("/assistant/decisions/policies")
    def list_decision_policies() -> dict:
        return service.list_decision_policies()

    @router.post("/assistant/decisions/policies/reload")
    def reload_decision_policies() -> dict:
        return service.reload_decision_policies()

    @router.get("/assistant/decisions/executions")
    def list_executions(limit: int = 50, status: str = "") -> dict:
        return service.list_execution_records(limit=limit, status=status)

    @router.get("/assistant/decisions/rollbacks")
    def list_rollbacks(limit: int = 50, status: str = "") -> dict:
        return service.list_rollback_records(limit=limit, status=status)

    @router.post("/assistant/decisions/{decision_id}/execute")
    def execute_decision(decision_id: str, payload: DecisionActionRequest) -> dict:
        result = service.execute_decision(
            decision_id,
            actor=payload.actor,
            decision_note=payload.decision_note,
            force_execute=payload.force_execute,
        )
        if not result.get("ok"):
            status_code = 403 if result.get("required_role") else 409 if result.get("status") == "blocked" else 404
            raise HTTPException(status_code=status_code, detail=result.get("message", "Decision could not be executed."))
        return result

    @router.post("/assistant/decisions/{decision_id}/rollback")
    def rollback_decision(decision_id: str, payload: DecisionActionRequest) -> dict:
        result = service.rollback_decision_execution(
            decision_id,
            actor=payload.actor,
            reason=payload.decision_note,
        )
        if not result.get("ok"):
            status_code = 403 if result.get("required_role") else 409 if result.get("status") == "blocked" else 404
            raise HTTPException(status_code=status_code, detail=result.get("message", "Decision rollback failed."))
        return result

    @router.post("/assistant/decisions/metrics/snapshot")
    def snapshot_decision_metrics(reason: str = "manual") -> dict:
        return service.snapshot_decision_metrics(reason=reason)

    @router.get("/assistant/decisions/metrics")
    def decision_metrics() -> dict:
        return service.decision_metrics()

    @router.get("/assistant/decisions/metrics/history")
    def decision_metric_history(limit: int = 100, metric_name: str = "") -> dict:
        return service.list_decision_metric_snapshots(limit=limit, metric_name=metric_name)

    @router.get("/assistant/decisions/metrics/export")
    def export_decision_metrics(metric_name: str = "", limit: int = 200) -> dict:
        return service.export_decision_metrics_csv(metric_name=metric_name, limit=limit)

    return router
