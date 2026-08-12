from __future__ import annotations

from typing import Any

from fastapi import APIRouter

from ..models import ChatRequest, RuntimeDeviceRequest, RuntimeHistoryRequest, RuntimeOtaRequest
from ..service import AssistantService, DEMO_QUESTIONS


def create_assistant_router(service: AssistantService) -> APIRouter:
    router = APIRouter()

    @router.get("/assistant/health")
    def health() -> dict[str, Any]:
        return service.health()

    @router.get("/assistant/demo_questions")
    def demo_questions() -> dict[str, list[str]]:
        return {"questions": DEMO_QUESTIONS}

    @router.get("/assistant/schema")
    def schema_overview() -> dict[str, Any]:
        return service.schema_overview()

    @router.post("/assistant/rebuild_index")
    def rebuild_index() -> dict[str, Any]:
        return service.rebuild_index()

    @router.post("/assistant/chat")
    def chat(payload: ChatRequest) -> dict[str, Any]:
        return service.chat(payload.question)

    @router.post("/assistant/runtime/device_status")
    def runtime_device_status(payload: RuntimeDeviceRequest) -> dict[str, Any]:
        return service.runtime_gateway.get_device_status(payload.device_id)

    @router.post("/assistant/runtime/sensor_history")
    def runtime_sensor_history(payload: RuntimeHistoryRequest) -> dict[str, Any]:
        return service.runtime_gateway.get_sensor_history(payload.device_id, payload.limit)

    @router.get("/assistant/runtime/system_events")
    def runtime_system_events(limit: int = 10) -> dict[str, Any]:
        return service.runtime_gateway.get_system_events(limit=limit)

    @router.post("/assistant/runtime/ota_task_status")
    def runtime_ota_task_status(payload: RuntimeOtaRequest) -> dict[str, Any]:
        return service.runtime_gateway.get_ota_task_status(payload.task_id)

    @router.get("/assistant/runtime/database_summary")
    def runtime_database_summary() -> dict[str, Any]:
        return service.runtime_gateway.get_database_summary()

    return router
