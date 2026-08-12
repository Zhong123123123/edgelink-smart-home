from __future__ import annotations

from pathlib import Path


def discover_documents(root: Path) -> list[Path]:
    if not root.exists():
        return []
    return sorted([path for path in root.rglob("*") if path.suffix.lower() in {".md", ".txt"}])
