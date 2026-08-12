from __future__ import annotations

from .api.agent_routes import create_agent_router
from .api.batch_run_routes import create_batch_run_router
from .api.decision_routes import create_decision_router
from .api.approval_routes import create_approval_router
from .api.assistant_routes import create_assistant_router
from .api.audit_routes import create_audit_router
from .api.report_routes import create_report_router
from .api.ticket_routes import create_ticket_router
from .api.workflow_routes import create_workflow_router
from .config import get_settings
from .service import AssistantService

try:
    from fastapi import FastAPI
except ModuleNotFoundError as exc:
    raise ModuleNotFoundError(
        "FastAPI dependencies are not installed. Install with: "
        "python3 -m pip install -r gateway_ai_assistant/requirements.txt"
    ) from exc


settings = get_settings()
service = AssistantService(settings)
app = FastAPI(title="Gateway AI Assistant")
app.state.service = service
app.include_router(create_assistant_router(service))
app.include_router(create_agent_router(service))
app.include_router(create_workflow_router(service))
app.include_router(create_approval_router(service))
app.include_router(create_batch_run_router(service))
app.include_router(create_decision_router(service))
app.include_router(create_audit_router(service))
app.include_router(create_report_router(service))
app.include_router(create_ticket_router(service))


if __name__ == "__main__":
    import uvicorn

    uvicorn.run("gateway_ai_assistant.app:app", host="127.0.0.1", port=8010, reload=False)
