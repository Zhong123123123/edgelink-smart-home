from __future__ import annotations

from fastapi import APIRouter

from ..models import TicketAssignRequest, TicketCreateRequest
from ..service import AssistantService


def create_ticket_router(service: AssistantService) -> APIRouter:
    router = APIRouter()

    @router.post("/assistant/tickets")
    def ticket_create(payload: TicketCreateRequest) -> dict:
        return service.create_ticket(
            title=payload.title,
            severity=payload.severity,
            device_id=payload.device_id,
            report_id=payload.report_id,
            description=payload.description,
        )

    @router.get("/assistant/tickets")
    def ticket_list(limit: int = 20, status: str = "", severity: str = "", assignee: str = "") -> dict:
        return service.list_tickets(limit=limit, status=status, severity=severity, assignee=assignee)

    @router.get("/assistant/tickets/{ticket_id}")
    def ticket_detail(ticket_id: str) -> dict:
        return service.get_ticket(ticket_id)

    @router.post("/assistant/tickets/{ticket_id}/assign")
    def ticket_assign(ticket_id: str, payload: TicketAssignRequest) -> dict:
        return service.assign_ticket(ticket_id, payload.assignee)

    @router.post("/assistant/tickets/{ticket_id}/close")
    def ticket_close(ticket_id: str) -> dict:
        return service.close_ticket(ticket_id)

    @router.post("/assistant/tickets/{ticket_id}/reopen")
    def ticket_reopen(ticket_id: str) -> dict:
        return service.reopen_ticket(ticket_id)

    return router
