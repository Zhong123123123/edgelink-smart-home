from __future__ import annotations

from fastapi import APIRouter

from ..service import AssistantService


def create_report_router(service: AssistantService) -> APIRouter:
    router = APIRouter()

    @router.get("/assistant/reports")
    def report_list(limit: int = 20) -> dict:
        return service.list_reports(limit=limit)

    @router.get("/assistant/reports/{report_id}")
    def report_detail(report_id: str) -> dict:
        return service.get_report(report_id)

    @router.get("/assistant/reports/{report_id}/markdown")
    def report_markdown(report_id: str) -> dict:
        return service.get_report_markdown(report_id)

    return router

