from __future__ import annotations

from pydantic import BaseModel
from pydantic import Field


class ChatRequest(BaseModel):
    question: str


class RealtimeEventRequest(BaseModel):
    event_type: str
    source: str = "api"
    severity: str = "info"
    device_id: int | None = None
    payload: dict = Field(default_factory=dict)
    happened_at: str = ""


class AlertActionRequest(BaseModel):
    actor: str = "operator"
    suppress_minutes: int = 0


class DecisionActionRequest(BaseModel):
    actor: str = "operator"
    decision_note: str = ""
    force_execute: bool = False


class AgentRequest(BaseModel):
    question: str


class AgentChatRequest(BaseModel):
    question: str
    session_id: str = ""


class AgentResumeRequest(BaseModel):
    session_id: str
    decision: str = "approve"
    edit_args: dict = Field(default_factory=dict)


class AgentEvalRunRequest(BaseModel):
    cases: list[dict] = Field(default_factory=list)


class AgentSessionMemoryRequest(BaseModel):
    actor: str = "operator"
    summary: str = ""
    key_facts: dict = Field(default_factory=dict)
    last_question: str = ""
    last_answer: str = ""


class AgentLongTermMemoryRequest(BaseModel):
    actor: str = "operator"
    memory_key: str
    content: str
    scope: str = "global"
    confidence: float = 0.7
    source_session_id: str = ""
    memory_type: str = "manual"
    provenance: str = "manual_curated"
    status: str = "active"
    is_pinned: bool = False
    expires_at: str = ""


class AgentLongTermMemoryUpdateRequest(BaseModel):
    actor: str = "operator"
    content: str | None = None
    confidence: float | None = None
    memory_type: str | None = None
    provenance: str | None = None
    status: str | None = None
    is_pinned: bool | None = None
    expires_at: str | None = None


class AgentMemoryCandidateDecisionRequest(BaseModel):
    reviewed_by: str = "operator"
    decision_note: str = ""


class RuntimeDeviceRequest(BaseModel):
    device_id: str


class RuntimeHistoryRequest(BaseModel):
    device_id: str
    limit: int = 10


class RuntimeOtaRequest(BaseModel):
    task_id: str


class WorkflowOtaRequest(BaseModel):
    device_id: int
    firmware_id: str


class WorkflowOtaCreateRequest(BaseModel):
    device_id: int
    firmware_id: str
    transport: str = "serial"
    target: str = "127.0.0.1:19090"


class WorkflowOtaBatchRequest(BaseModel):
    device_ids: list[int]
    firmware_id: str
    transport: str = "serial"
    target: str = "127.0.0.1:19090"
    batch_size: int = 2


class WorkflowOtaConfirmRequest(BaseModel):
    approval_id: str
    approved_by: str = "operator"


class WorkflowReportRequest(BaseModel):
    report_type: str = "fault_ticket"
    device_id: int | None = None
    task_id: str = ""
    limit: int = 10


class WorkflowDiagnoseRequest(BaseModel):
    device_id: int
    limit: int = 10


class ApprovalActionRequest(BaseModel):
    operator: str = "operator"
    note: str = ""


class TicketCreateRequest(BaseModel):
    title: str = ""
    severity: str = "medium"
    device_id: int | None = None
    report_id: str = ""
    description: str = ""


class TicketAssignRequest(BaseModel):
    assignee: str
