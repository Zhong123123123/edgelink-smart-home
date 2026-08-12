from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class EventEnvelope:
    event_type: str
    source: str
    severity: str
    payload: dict[str, Any]
    device_id: int | None = None
    happened_at: str = ""

    @property
    def dedupe_subject(self) -> str:
        if self.device_id is not None:
            return f"{self.event_type}:device:{self.device_id}"
        return f"{self.event_type}:global"
