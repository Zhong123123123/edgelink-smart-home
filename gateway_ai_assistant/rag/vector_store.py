from __future__ import annotations


class VectorStore:
    def __init__(self) -> None:
        self._chunks: list[dict[str, object]] = []

    def add(self, chunk: dict[str, object]) -> None:
        self._chunks.append(chunk)

    def all(self) -> list[dict[str, object]]:
        return list(self._chunks)
