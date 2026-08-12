from __future__ import annotations

from fastapi import APIRouter

from ..models import ApprovalActionRequest
from ..service import AssistantService


def create_approval_router(service: AssistantService) -> APIRouter:
    router = APIRouter()

    @router.get("/assistant/approvals")
    def approval_list(limit: int = 20, status: str = "") -> dict:
        return service.list_approval_requests(limit=limit, status=status)

    @router.get("/assistant/approvals/{approval_id}")
    def approval_detail(approval_id: str) -> dict:
        item = service.assistant_db.get_approval_request(approval_id)
        if item is None:
            return {"ok": False, "message": "Approval request was not found.", "approval_id": approval_id}
        return {"ok": True, "approval": item}

    @router.post("/assistant/approvals/{approval_id}/approve")
    def approval_approve(approval_id: str, payload: ApprovalActionRequest) -> dict:
        return service.approve_request(approval_id, payload.operator, payload.note)

    @router.post("/assistant/approvals/{approval_id}/reject")
    def approval_reject(approval_id: str, payload: ApprovalActionRequest) -> dict:
        return service.reject_approval_request(approval_id, payload.operator, payload.note)

    @router.post("/assistant/approvals/{approval_id}/cancel")
    def approval_cancel(approval_id: str, payload: ApprovalActionRequest) -> dict:
        return service.cancel_approval_request(approval_id, payload.operator, payload.note)

    @router.post("/assistant/approvals/{approval_id}/retry_failed")
    def approval_retry_failed(approval_id: str, payload: ApprovalActionRequest) -> dict:
        return service.retry_batch_failed_devices(approval_id, payload.operator, payload.note)

    return router
