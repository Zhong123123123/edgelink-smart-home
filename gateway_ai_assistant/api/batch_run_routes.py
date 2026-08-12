from __future__ import annotations

from fastapi import APIRouter

from ..models import ApprovalActionRequest
from ..service import AssistantService


def create_batch_run_router(service: AssistantService) -> APIRouter:
    router = APIRouter()

    @router.get("/assistant/ota/batch_runs")
    def batch_run_list(limit: int = 20) -> dict:
        return service.list_batch_runs(limit=limit)

    @router.get("/assistant/ota/batch_runs/{batch_run_id}")
    def batch_run_detail(batch_run_id: str) -> dict:
        return service.get_batch_run(batch_run_id)

    @router.post("/assistant/ota/batch_runs/{batch_run_id}/refresh")
    def batch_run_refresh(batch_run_id: str) -> dict:
        return service.refresh_batch_run(batch_run_id)

    @router.post("/assistant/ota/batch_runs/{batch_run_id}/start_next_batch")
    def batch_run_start_next(batch_run_id: str, payload: ApprovalActionRequest) -> dict:
        return service.start_next_batch(batch_run_id, payload.operator, payload.note)

    @router.post("/assistant/ota/batch_runs/{batch_run_id}/pause")
    def batch_run_pause(batch_run_id: str, payload: ApprovalActionRequest) -> dict:
        return service.pause_batch_run(batch_run_id, payload.operator, payload.note)

    @router.post("/assistant/ota/batch_runs/{batch_run_id}/terminate")
    def batch_run_terminate(batch_run_id: str, payload: ApprovalActionRequest) -> dict:
        return service.terminate_batch_run(batch_run_id, payload.operator, payload.note)

    return router
