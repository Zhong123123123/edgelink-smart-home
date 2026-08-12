from __future__ import annotations

from ..config import get_settings
from ..sql.assistant_db import AssistantDB
from .indexer import build_and_save_index


def main() -> None:
    settings = get_settings()
    assistant_db = AssistantDB(settings.assistant_db)
    payload = build_and_save_index(settings, assistant_db)
    print(f"wrote {payload['total_chunks']} chunks to {settings.index_path}")


if __name__ == "__main__":
    main()
