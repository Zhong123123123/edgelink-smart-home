from __future__ import annotations

from fastapi import APIRouter

from ..models import (
    WorkflowDiagnoseRequest,
    WorkflowOtaBatchRequest,
    WorkflowOtaConfirmRequest,
    WorkflowOtaCreateRequest,
    WorkflowOtaRequest,
    WorkflowReportRequest,
)
from ..service import AssistantService


def create_workflow_router(service: AssistantService) -> APIRouter:
    router = APIRouter()

    @router.post("/assistant/workflow/ota_risk_check")
    def ota_risk_check(payload: WorkflowOtaRequest) -> dict:
        return service.workflow_ota_risk_check(payload.device_id, payload.firmware_id)

    @router.post("/assistant/workflow/ota_plan")
    def ota_plan(payload: WorkflowOtaRequest) -> dict:
        return service.workflow_ota_plan(payload.device_id, payload.firmware_id)

    @router.post("/assistant/workflow/ota_request_create")
    def ota_request_create(payload: WorkflowOtaCreateRequest) -> dict:
        return service.workflow_ota_request_create(
            payload.device_id,
            payload.firmware_id,
            payload.transport,
            payload.target,
        )

    @router.post("/assistant/workflow/ota_batch_plan")
    def ota_batch_plan(payload: WorkflowOtaBatchRequest) -> dict:
        return service.workflow_ota_batch_plan(
            payload.device_ids,
            payload.firmware_id,
            payload.transport,
            payload.target,
            payload.batch_size,
        )

    @router.post("/assistant/workflow/ota_batch_request_create")
    def ota_batch_request_create(payload: WorkflowOtaBatchRequest) -> dict:
        return service.workflow_ota_batch_request_create(
            payload.device_ids,
            payload.firmware_id,
            payload.transport,
            payload.target,
            payload.batch_size,
        )

    @router.post("/assistant/workflow/ota_request_confirm")
    def ota_request_confirm(payload: WorkflowOtaConfirmRequest) -> dict:
        return service.workflow_ota_request_confirm(payload.approval_id, payload.approved_by)

    @router.post("/assistant/workflow/report_generate")
    def report_generate(payload: WorkflowReportRequest) -> dict:
        return service.workflow_generate_report(
            report_type=payload.report_type,
            device_id=payload.device_id,
            task_id=payload.task_id,
            limit=payload.limit,
        )

    @router.post("/assistant/workflow/diagnose_fault")
    def diagnose_fault(payload: WorkflowDiagnoseRequest) -> dict:
        return service.workflow_diagnose_fault(
            device_id=payload.device_id,
            limit=payload.limit,
        )

    return router
