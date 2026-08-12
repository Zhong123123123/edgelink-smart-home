from __future__ import annotations

import json

from fastapi import APIRouter
from fastapi import HTTPException
from fastapi.responses import StreamingResponse

from ..models import (
    AgentChatRequest,
    AgentEvalRunRequest,
    AgentLongTermMemoryRequest,
    AgentLongTermMemoryUpdateRequest,
    AgentMemoryCandidateDecisionRequest,
    AgentRequest,
    AgentResumeRequest,
    AgentSessionMemoryRequest,
)
from ..service import AssistantService


def create_agent_router(service: AssistantService) -> APIRouter:
    router = APIRouter()

    def _is_permission_error(result: dict) -> bool:
        if result.get("required_role"):
            return True
        message = str(result.get("message", "")).lower()
        return "requires role" in message or "required role" in message

    @router.post("/assistant/agent/plan")
    def agent_plan(payload: AgentRequest) -> dict:
        return service.agent_plan(payload.question)

    @router.post("/assistant/agent/execute")
    def agent_execute(payload: AgentRequest) -> dict:
        return service.agent_execute(payload.question)

    @router.get("/assistant/agent/sessions/{session_id}")
    def agent_session(session_id: str) -> dict:
        return service.get_agent_session(session_id)

    @router.get("/assistant/agent/sessions/{session_id}/replay")
    def agent_session_replay(session_id: str) -> dict:
        return service.export_agent_session_replay(session_id)

    @router.get("/assistant/agent/sessions/{session_id}/memory")
    def agent_session_memory(session_id: str) -> dict:
        return service.get_agent_memory_snapshot(session_id)

    @router.get("/assistant/agent/memories")
    def agent_memories(limit: int = 20) -> dict:
        return service.list_agent_memories(limit=limit)

    @router.get("/assistant/agent/memory_candidates")
    def agent_memory_candidates(limit: int = 20, status: str = "review_queue") -> dict:
        return service.list_memory_candidates(limit=limit, status=status)

    @router.get("/assistant/agent/memory_audits")
    def agent_memory_audits(limit: int = 20, memory_key: str = "", actor: str = "", action_type: str = "") -> dict:
        return service.list_memory_audits(limit=limit, memory_key=memory_key, actor=actor, action_type=action_type)

    @router.post("/assistant/agent/sessions/{session_id}/memory")
    def upsert_agent_session_memory(session_id: str, payload: AgentSessionMemoryRequest) -> dict:
        return service.upsert_agent_session_memory(
            session_id,
            actor=payload.actor,
            summary=payload.summary,
            key_facts=payload.key_facts,
            last_question=payload.last_question,
            last_answer=payload.last_answer,
        )

    @router.delete("/assistant/agent/sessions/{session_id}/memory")
    def delete_agent_session_memory(session_id: str, actor: str = "operator") -> dict:
        result = service.delete_agent_session_memory(session_id, actor=actor)
        if not result.get("ok"):
            status_code = 403 if _is_permission_error(result) else 404
            raise HTTPException(status_code=status_code, detail=result.get("message", "Session memory was not found."))
        return result

    @router.post("/assistant/agent/memories/long_term")
    def create_long_term_memory(payload: AgentLongTermMemoryRequest) -> dict:
        result = service.create_or_update_long_term_memory(
            memory_key=payload.memory_key,
            content=payload.content,
            scope=payload.scope,
            confidence=payload.confidence,
            source_session_id=payload.source_session_id,
            memory_type=payload.memory_type,
            provenance=payload.provenance,
            status=payload.status,
            is_pinned=payload.is_pinned,
            expires_at=payload.expires_at,
            actor=payload.actor,
        )
        if not result.get("ok"):
            status_code = 403 if _is_permission_error(result) else 400
            raise HTTPException(status_code=status_code, detail=result.get("message", "Long-term memory could not be created."))
        return result

    @router.post("/assistant/agent/memories/long_term/{memory_id}")
    def update_long_term_memory(memory_id: int, payload: AgentLongTermMemoryUpdateRequest) -> dict:
        result = service.update_long_term_memory(
            memory_id,
            actor=payload.actor,
            content=payload.content,
            confidence=payload.confidence,
            memory_type=payload.memory_type,
            provenance=payload.provenance,
            status=payload.status,
            is_pinned=payload.is_pinned,
            expires_at=payload.expires_at,
        )
        if not result.get("ok"):
            status_code = 403 if _is_permission_error(result) else 404
            raise HTTPException(status_code=status_code, detail=result.get("message", "Long-term memory was not found."))
        return result

    @router.delete("/assistant/agent/memories/long_term/{memory_id}")
    def delete_long_term_memory(memory_id: int, actor: str = "operator") -> dict:
        result = service.delete_long_term_memory(memory_id, actor=actor)
        if not result.get("ok"):
            status_code = 403 if _is_permission_error(result) else 404
            raise HTTPException(status_code=status_code, detail=result.get("message", "Long-term memory was not found."))
        return result

    @router.post("/assistant/agent/memory_candidates/{candidate_id}/approve")
    def approve_memory_candidate(candidate_id: str, payload: AgentMemoryCandidateDecisionRequest) -> dict:
        result = service.approve_memory_candidate(candidate_id, reviewed_by=payload.reviewed_by, decision_note=payload.decision_note)
        if not result.get("ok"):
            status_code = 403 if _is_permission_error(result) else 404
            raise HTTPException(status_code=status_code, detail=result.get("message", "Memory candidate was not found."))
        return result

    @router.post("/assistant/agent/memory_candidates/{candidate_id}/reject")
    def reject_memory_candidate(candidate_id: str, payload: AgentMemoryCandidateDecisionRequest) -> dict:
        result = service.reject_memory_candidate(candidate_id, reviewed_by=payload.reviewed_by, decision_note=payload.decision_note)
        if not result.get("ok"):
            status_code = 403 if _is_permission_error(result) else 404
            raise HTTPException(status_code=status_code, detail=result.get("message", "Memory candidate was not found."))
        return result

    @router.post("/assistant/agent/chat")
    def agent_chat(payload: AgentChatRequest) -> dict:
        try:
            return service.chat_agent(payload.question, payload.session_id)
        except RuntimeError as exc:
            raise HTTPException(status_code=503, detail=str(exc)) from exc

    @router.post("/assistant/agent/stream")
    def agent_stream(payload: AgentChatRequest) -> StreamingResponse:
        try:
            stream = service.chat_agent_stream(payload.question, payload.session_id)
        except RuntimeError as exc:
            raise HTTPException(status_code=503, detail=str(exc)) from exc

        async def event_source():
            async for event in stream:
                yield f"data: {json.dumps(event, ensure_ascii=False)}\n\n"

        return StreamingResponse(event_source(), media_type="text/event-stream")

    @router.post("/assistant/agent/resume")
    def agent_resume(payload: AgentResumeRequest) -> dict:
        if not payload.session_id:
            raise HTTPException(status_code=400, detail="session_id is required to resume")
        result = service.chat_agent_resume(
            payload.session_id,
            decision=payload.decision,
            edit_args=payload.edit_args,
        )
        if not result.get("ok"):
            raise HTTPException(status_code=409, detail=result.get("error", "No pending approval interrupt for this session."))
        return result

    @router.post("/assistant/evals/agent/run")
    def agent_eval_run(payload: AgentEvalRunRequest) -> dict:
        try:
            return service.run_agent_eval_suite(payload.cases or None)
        except RuntimeError as exc:
            raise HTTPException(status_code=503, detail=str(exc)) from exc

    @router.get("/assistant/evals/agent/runs")
    def agent_eval_runs(limit: int = 20) -> dict:
        return service.list_agent_eval_runs(limit=limit)

    @router.get("/assistant/evals/agent/runs/{run_id}")
    def agent_eval_run_detail(run_id: str) -> dict:
        result = service.get_agent_eval_run(run_id)
        if not result.get("ok"):
            raise HTTPException(status_code=404, detail=result.get("message", "Eval run was not found."))
        return result

    return router
