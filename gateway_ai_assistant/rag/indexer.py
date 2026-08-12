from __future__ import annotations

import json
import math
import re
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from ..config import Settings
from ..sql.assistant_db import AssistantDB
from .document_loader import discover_documents
from .embedding import embed_texts, embedding_backend_name
from .text_splitter import split_markdown_sections

TOKEN_RE = re.compile(r"[A-Za-z0-9_/\-.]+|[\u4e00-\u9fff]+")
INDEX_SCHEMA_VERSION = 2


def tokenize(text: str) -> list[str]:
    tokens: list[str] = []
    for match in TOKEN_RE.findall(text.lower()):
        if re.fullmatch(r"[\u4e00-\u9fff]+", match):
            if len(match) == 1:
                tokens.append(match)
            else:
                tokens.extend(match[i : i + 2] for i in range(len(match) - 1))
                tokens.append(match)
        else:
            tokens.append(match)
            if re.fullmatch(r"[0-9a-f]+", match) and len(match) >= 4 and len(match) % 2 == 0:
                tokens.extend(match[i : i + 2] for i in range(0, len(match), 2))
    return tokens


@dataclass(frozen=True)
class IndexedChunk:
    chunk_id: str
    source_path: str
    source_type: str
    source_name: str
    section_title: str
    section_index: int
    chunk_index: int
    start_offset: int
    end_offset: int
    chunk_text: str
    token_counts: dict[str, int]
    token_total: int


def get_index_roots(settings: Settings) -> list[Path]:
    return [settings.gateway_root / "README.md", settings.gateway_root / "docs"]


def discover_index_source_paths(settings: Settings) -> list[Path]:
    source_paths: list[Path] = []
    for root in get_index_roots(settings):
        paths = [root] if root.is_file() else discover_documents(root)
        for path in paths:
            if path.exists() and path.is_file():
                source_paths.append(path)
    return source_paths


def build_source_manifest(paths: list[Path]) -> list[dict[str, object]]:
    manifest: list[dict[str, object]] = []
    for path in paths:
        stat = path.stat()
        manifest.append(
            {
                "source_path": str(path),
                "mtime": datetime.fromtimestamp(stat.st_mtime, timezone.utc).isoformat(),
                "size": stat.st_size,
            }
        )
    return manifest


def build_index_payload(settings: Settings) -> tuple[dict[str, object], list[tuple[str, str, str, int, str]]]:
    roots = get_index_roots(settings)
    chunks: list[IndexedChunk] = []
    metadata_rows: list[tuple[str, str, str, int, str]] = []
    now = datetime.now(timezone.utc).isoformat()
    indexed_paths: list[Path] = []

    for root in roots:
        paths = [root] if root.is_file() else discover_documents(root)
        for path in paths:
            if not path.exists() or not path.is_file():
                continue
            indexed_paths.append(path)
            text = path.read_text(encoding="utf-8", errors="ignore")
            split_chunks = split_markdown_sections(text)
            chunk_count = 0
            for idx, chunk_item in enumerate(split_chunks):
                chunk_text = str(chunk_item["chunk_text"])
                tokens = tokenize(chunk_text)
                if not tokens:
                    continue
                chunk_count += 1
                chunks.append(
                    IndexedChunk(
                        chunk_id=f"{path}:{idx}",
                        source_path=str(path),
                        source_type=path.suffix.lower().lstrip(".") or "text",
                        source_name=path.name,
                        section_title=str(chunk_item["section_title"]),
                        section_index=int(chunk_item["section_index"]),
                        chunk_index=idx,
                        start_offset=int(chunk_item["start_offset"]),
                        end_offset=int(chunk_item["end_offset"]),
                        chunk_text=chunk_text,
                        token_counts=dict(Counter(tokens)),
                        token_total=len(tokens),
                    )
                )
            metadata_rows.append(
                (
                    str(path),
                    path.suffix.lower().lstrip(".") or "text",
                    datetime.fromtimestamp(path.stat().st_mtime, timezone.utc).isoformat(),
                    chunk_count,
                    now,
                )
            )

    doc_freq: Counter[str] = Counter()
    for chunk in chunks:
        doc_freq.update(chunk.token_counts.keys())

    embeddings = embed_texts([chunk.chunk_text for chunk in chunks]) if chunks else []

    payload = {
        "schema_version": INDEX_SCHEMA_VERSION,
        "created_at": now,
        "backend": embedding_backend_name(),
        "total_chunks": len(chunks),
        "doc_count": len(chunks),
        "source_count": len(indexed_paths),
        "source_manifest": build_source_manifest(indexed_paths),
        "doc_freq": dict(doc_freq),
        "chunks": [
            {
                "chunk_id": chunk.chunk_id,
                "source_path": chunk.source_path,
                "source_type": chunk.source_type,
                "source_name": chunk.source_name,
                "section_title": chunk.section_title,
                "section_index": chunk.section_index,
                "chunk_index": chunk.chunk_index,
                "start_offset": chunk.start_offset,
                "end_offset": chunk.end_offset,
                "chunk_text": chunk.chunk_text,
                "embedding": embeddings[idx] if idx < len(embeddings) else [],
                "token_counts": chunk.token_counts,
                "token_total": chunk.token_total,
            }
            for idx, chunk in enumerate(chunks)
        ],
    }
    return payload, metadata_rows


def save_index(settings: Settings, payload: dict[str, object]) -> None:
    settings.index_path.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")


def build_and_save_index(settings: Settings, assistant_db: AssistantDB) -> dict[str, object]:
    payload, metadata_rows = build_index_payload(settings)
    save_index(settings, payload)
    assistant_db.replace_rag_metadata(metadata_rows)
    return payload


def load_index(index_path: Path) -> dict[str, object]:
    return json.loads(index_path.read_text(encoding="utf-8"))


def index_status(settings: Settings) -> dict[str, Any]:
    if not settings.index_path.exists():
        return {"exists": False, "is_stale": True, "reason": "missing"}
    try:
        payload = load_index(settings.index_path)
    except json.JSONDecodeError:
        return {"exists": True, "is_stale": True, "reason": "invalid_json"}
    status = inspect_index_freshness(settings, payload)
    status["exists"] = True
    return status


def inspect_index_freshness(settings: Settings, payload: dict[str, Any]) -> dict[str, Any]:
    if int(payload.get("schema_version", 0)) < INDEX_SCHEMA_VERSION:
        return {"is_stale": True, "reason": "schema_outdated"}
    if str(payload.get("backend", "")) != embedding_backend_name():
        return {"is_stale": True, "reason": "backend_changed"}

    current_paths = discover_index_source_paths(settings)
    current_manifest = build_source_manifest(current_paths)
    payload_manifest = payload.get("source_manifest", [])
    if len(payload_manifest) != len(current_manifest):
        return {"is_stale": True, "reason": "source_count_changed"}

    payload_by_path = {
        str(item.get("source_path")): (
            str(item.get("mtime")),
            int(item.get("size", 0)),
        )
        for item in payload_manifest
    }
    for item in current_manifest:
        source_path = str(item["source_path"])
        current_signature = (str(item["mtime"]), int(item["size"]))
        if source_path not in payload_by_path:
            return {"is_stale": True, "reason": "source_added_or_removed"}
        if payload_by_path[source_path] != current_signature:
            return {"is_stale": True, "reason": "source_modified"}
    return {
        "is_stale": False,
        "reason": "fresh",
        "backend": payload.get("backend", "unknown"),
        "schema_version": payload.get("schema_version", 1),
        "source_count": payload.get("source_count", len(current_manifest)),
    }


def ensure_index(settings: Settings, assistant_db: AssistantDB) -> tuple[dict[str, Any], dict[str, Any]]:
    status = index_status(settings)
    if status.get("is_stale", True):
        payload = build_and_save_index(settings, assistant_db)
        return payload, {
            "exists": True,
            "is_stale": False,
            "reason": f"rebuilt_from_{status.get('reason', 'unknown')}",
            "backend": payload.get("backend", "unknown"),
            "schema_version": payload.get("schema_version", 1),
            "source_count": payload.get("source_count", 0),
        }
    payload = load_index(settings.index_path)
    return payload, status


def score_chunk(query_tokens: list[str], payload: dict[str, object], chunk: dict[str, object]) -> float:
    if not query_tokens:
        return 0.0
    doc_count = max(1, int(payload.get("doc_count", 1)))
    doc_freq = payload.get("doc_freq", {})
    token_counts = chunk.get("token_counts", {})
    token_total = max(1, int(chunk.get("token_total", 1)))
    score = 0.0
    for token in set(query_tokens):
        tf = float(token_counts.get(token, 0))
        if tf <= 0:
            continue
        df = max(1, int(doc_freq.get(token, 1)))
        idf = math.log(1.0 + doc_count / df)
        score += (tf / token_total) * idf
    text = str(chunk.get("chunk_text", "")).lower()
    compact_text = re.sub(r"\s+", "", text)
    for token in set(query_tokens):
        if len(token) >= 3 and token in text:
            score += 0.15
        if len(token) >= 4 and token in compact_text:
            score += 0.25
    return score
