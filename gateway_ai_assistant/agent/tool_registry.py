from __future__ import annotations


TOOL_REGISTRY = {
    "chat": {"mode": "read_only", "description": "General assistant entrypoint with existing routing."},
    "workflow_diagnose_fault": {"mode": "read_only", "description": "Deterministic fault diagnosis workflow."},
    "workflow_generate_report": {"mode": "read_only", "description": "Generate a persisted markdown/json report."},
    "workflow_ota_risk_check": {"mode": "read_only", "description": "Evaluate OTA upgrade risks before task creation."},
    "workflow_ota_request_create": {"mode": "approval_required", "description": "Create OTA approval request only."},
    "workflow_ota_request_confirm": {"mode": "write_requires_approval", "description": "Confirm approval request and create the real OTA task."},
    "workflow_ota_batch_plan": {"mode": "read_only", "description": "Generate a phased batch/grayscale OTA rollout plan."},
    "workflow_ota_batch_request_create": {"mode": "approval_required", "description": "Create a batch OTA approval request without pushing real tasks."},
    "start_next_batch": {"mode": "write_requires_approval", "description": "Start the next pending OTA batch for an existing batch run."},
    "pause_batch_run": {"mode": "write_requires_approval", "description": "Pause a batch rollout before starting the next batch."},
    "terminate_batch_run": {"mode": "write_requires_approval", "description": "Terminate pending parts of a batch rollout."},
    "refresh_batch_run": {"mode": "read_only", "description": "Refresh a tracked OTA batch run from gateway task states."},
    "retry_batch_failed_devices": {"mode": "write_requires_approval", "description": "Retry failed devices from an approved OTA batch."},
    "reopen_ticket": {"mode": "write_requires_approval", "description": "Reopen a closed fault ticket."},
}
