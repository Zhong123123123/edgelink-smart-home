from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Settings:
    repo_root: Path
    app_root: Path
    data_dir: Path
    reports_dir: Path
    prompts_dir: Path
    index_path: Path
    gateway_root: Path
    gateway_history_db: Path
    gateway_ota_db: Path
    assistant_db: Path
    llm_api_key: str
    llm_base_url: str
    llm_model: str
    decision_policy_path: Path | None = None
    agent_max_iterations: int = 10
    agent_recursion_limit: int = 25
    agent_session_warn_message_count: int = 40
    agent_session_warn_char_count: int = 12000
    agent_memory_long_term_limit: int = 8
    agent_memory_answer_char_limit: int = 240
    agent_memory_related_session_limit: int = 3
    agent_memory_session_store_limit: int = 30
    agent_memory_long_term_store_limit: int = 20
    agent_admin_users: tuple[str, ...] = ("admin",)
    agent_approver_users: tuple[str, ...] = ("reviewer", "approver")
    agent_operator_users: tuple[str, ...] = ("operator",)
    agent_viewer_users: tuple[str, ...] = ("viewer",)

    def __post_init__(self) -> None:
        if self.decision_policy_path is None:
            object.__setattr__(
                self,
                "decision_policy_path",
                self.assistant_db.parent / "decision_policies.json",
            )


def get_settings() -> Settings:
    app_root = Path(__file__).resolve().parent
    repo_root = app_root.parent
    data_dir = app_root / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    reports_dir = app_root / "reports"
    reports_dir.mkdir(parents=True, exist_ok=True)
    prompts_dir = app_root / "prompts"
    gateway_root = repo_root / "serial-gateway"
    def parse_user_list(env_name: str, default: tuple[str, ...]) -> tuple[str, ...]:
        raw = os.getenv(env_name, "")
        if not raw.strip():
            return default
        return tuple(item.strip() for item in raw.split(",") if item.strip())
    return Settings(
        repo_root=repo_root,
        app_root=app_root,
        data_dir=data_dir,
        reports_dir=reports_dir,
        prompts_dir=prompts_dir,
        index_path=data_dir / "rag_index.json",
        gateway_root=gateway_root,
        gateway_history_db=gateway_root / "data" / "gateway_history.db",
        gateway_ota_db=gateway_root / "data" / "ota_tasks.json",
        assistant_db=data_dir / "assistant.db",
        decision_policy_path=data_dir / "decision_policies.json",
        llm_api_key=os.getenv("LLM_API_KEY", "").strip(),
        llm_base_url=os.getenv("LLM_BASE_URL", "https://api.openai.com/v1").strip(),
        llm_model=os.getenv("LLM_MODEL", "gpt-4o-mini").strip(),
        agent_max_iterations=int(os.getenv("AGENT_MAX_ITERATIONS", "10")),
        agent_recursion_limit=int(os.getenv("AGENT_RECURSION_LIMIT", "25")),
        agent_session_warn_message_count=int(os.getenv("AGENT_SESSION_WARN_MESSAGE_COUNT", "40")),
        agent_session_warn_char_count=int(os.getenv("AGENT_SESSION_WARN_CHAR_COUNT", "12000")),
        agent_memory_long_term_limit=int(os.getenv("AGENT_MEMORY_LONG_TERM_LIMIT", "8")),
        agent_memory_answer_char_limit=int(os.getenv("AGENT_MEMORY_ANSWER_CHAR_LIMIT", "240")),
        agent_memory_related_session_limit=int(os.getenv("AGENT_MEMORY_RELATED_SESSION_LIMIT", "3")),
        agent_memory_session_store_limit=int(os.getenv("AGENT_MEMORY_SESSION_STORE_LIMIT", "30")),
        agent_memory_long_term_store_limit=int(os.getenv("AGENT_MEMORY_LONG_TERM_STORE_LIMIT", "20")),
        agent_admin_users=parse_user_list("AGENT_ADMIN_USERS", ("admin",)),
        agent_approver_users=parse_user_list("AGENT_APPROVER_USERS", ("reviewer", "approver")),
        agent_operator_users=parse_user_list("AGENT_OPERATOR_USERS", ("operator",)),
        agent_viewer_users=parse_user_list("AGENT_VIEWER_USERS", ("viewer",)),
    )
