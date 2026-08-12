from __future__ import annotations

from fastapi import APIRouter

from ..service import AssistantService


def create_audit_router(service: AssistantService) -> APIRouter:
    router = APIRouter()

    @router.get("/assistant/audit/tool_calls")
    def tool_calls(limit: int = 50) -> dict:
        return service.list_tool_call_logs(limit=limit)

    @router.get("/assistant/audit/actions")
    def action_audits(limit: int = 50) -> dict:
        return service.list_action_audits(limit=limit)

    return router

