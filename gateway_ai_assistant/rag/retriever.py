from __future__ import annotations

from typing import Any

from .embedding import cosine_similarity, embed_query
from .indexer import ensure_index, score_chunk, tokenize


QUERY_EXPANSIONS: dict[str, list[str]] = {
    "协议": ["帧格式", "frame", "payload", "crc16", "len", "type"],
    "帧": ["frame", "协议", "报文"],
    "ota": ["升级", "firmware", "ack", "nack", "progress"],
    "升级": ["ota", "firmware", "版本", "progress"],
    "传感器": ["sensor", "温度", "湿度", "light", "状态"],
    "失败": ["error", "timeout", "ack timeout", "last_error"],
}


def retrieve(question: str, settings, assistant_db, top_k: int = 3) -> list[dict[str, object]]:
    payload, _ = ensure_index(settings, assistant_db)
    expanded_question = expand_query(question)
    query_tokens = tokenize(expanded_question)
    query_embedding = embed_query(expanded_question)
    question_lower = question.lower()
    scored: list[dict[str, Any]] = []
    for chunk in payload.get("chunks", []):
        lexical_score = score_chunk(query_tokens, payload, chunk)
        chunk_embedding = chunk.get("embedding", [])
        semantic_score = 0.0
        if chunk_embedding:
            semantic_score = max(0.0, cosine_similarity(query_embedding, chunk_embedding))
        score = (lexical_score * 0.85) + (semantic_score * 1.15)
        if score <= 0:
            continue
        source_path = str(chunk["source_path"]).lower()
        chunk_text = str(chunk["chunk_text"])
        section_title = str(chunk.get("section_title", ""))
        if any(word in question for word in ("协议", "帧格式", "crc")):
            if "protocol" in source_path or "readme" in source_path:
                score += 0.35
            if "protocol.md" in source_path or "/docs/" in source_path:
                score += 0.18
            if "aa 55 | len | type | payload | crc16" in chunk_text.lower():
                score += 0.6
            if "crc16" in chunk_text.lower() and "len" in chunk_text.lower() and "payload" in chunk_text.lower():
                score += 0.2
            if any(keyword in section_title.lower() for keyword in ("协议", "protocol", "帧", "frame")):
                score += 0.2
        if "ota" in question_lower and "流程" in question:
            if "ota_" in source_path or "/ota" in source_path or "ota" in source_path:
                score += 0.2
            if any(keyword in section_title.lower() for keyword in ("ota", "升级", "流程")):
                score += 0.15
        scored.append(
            {
                "chunk_id": chunk["chunk_id"],
                "source_path": chunk["source_path"],
                "source_name": chunk.get("source_name", ""),
                "section_title": chunk.get("section_title", ""),
                "chunk_index": chunk.get("chunk_index", 0),
                "citation": _build_citation(chunk),
                "chunk_text": chunk["chunk_text"],
                "matched_terms": _matched_terms(query_tokens, chunk),
                "lexical_score": round(lexical_score, 4),
                "semantic_score": round(semantic_score, 4),
                "score": round(score, 4),
                "embedding": chunk_embedding,
            }
        )
    scored.sort(key=lambda item: item["score"], reverse=True)
    reranked = _mmr_rerank(scored, query_embedding, top_k=top_k)
    diversified = _apply_source_diversity(reranked, top_k=top_k)
    return [
        {
            key: value
            for key, value in item.items()
            if key != "embedding"
        }
        for item in diversified
    ]


def expand_query(question: str) -> str:
    expanded_terms: list[str] = [question]
    lowered = question.lower()
    for trigger, extra_terms in QUERY_EXPANSIONS.items():
        if trigger in question or trigger in lowered:
            expanded_terms.extend(extra_terms)
    return " ".join(expanded_terms)


def _build_citation(chunk: dict[str, object]) -> str:
    source_name = str(chunk.get("source_name") or chunk.get("source_path") or "unknown")
    section_title = str(chunk.get("section_title", "")).strip()
    chunk_index = int(chunk.get("chunk_index", 0))
    if section_title:
        return f"{source_name} / {section_title} / chunk-{chunk_index}"
    return f"{source_name} / chunk-{chunk_index}"


def _mmr_rerank(scored: list[dict[str, Any]], query_embedding: list[float], top_k: int) -> list[dict[str, Any]]:
    if not scored:
        return []
    candidates = scored[: max(top_k * 3, top_k)]
    selected: list[dict[str, Any]] = []
    lambda_weight = 0.72

    while candidates and len(selected) < top_k:
        best_index = 0
        best_value = float("-inf")
        for index, item in enumerate(candidates):
            relevance = float(item["score"])
            diversity_penalty = 0.0
            if selected:
                diversity_penalty = max(
                    _chunk_similarity(item, existing)
                    for existing in selected
                )
            query_bonus = 0.0
            embedding = item.get("embedding") or []
            if embedding and query_embedding:
                query_bonus = max(0.0, cosine_similarity(query_embedding, embedding))
            mmr_score = (lambda_weight * (relevance + query_bonus * 0.3)) - ((1 - lambda_weight) * diversity_penalty)
            if mmr_score > best_value:
                best_value = mmr_score
                best_index = index
        selected.append(candidates.pop(best_index))
    return selected


def _apply_source_diversity(items: list[dict[str, Any]], top_k: int) -> list[dict[str, Any]]:
    selected: list[dict[str, Any]] = []
    source_counts: dict[str, int] = {}
    deferred: list[dict[str, Any]] = []
    max_per_source = 2
    for item in items:
        source_name = str(item.get("source_name") or item.get("source_path") or "unknown")
        if source_counts.get(source_name, 0) < max_per_source:
            selected.append(item)
            source_counts[source_name] = source_counts.get(source_name, 0) + 1
        else:
            deferred.append(item)
    for item in deferred:
        if len(selected) >= top_k:
            break
        selected.append(item)
    return selected[:top_k]


def _chunk_similarity(left: dict[str, Any], right: dict[str, Any]) -> float:
    left_embedding = left.get("embedding") or []
    right_embedding = right.get("embedding") or []
    if left_embedding and right_embedding:
        return max(0.0, cosine_similarity(left_embedding, right_embedding))
    left_text = str(left.get("chunk_text", ""))
    right_text = str(right.get("chunk_text", ""))
    left_tokens = set(tokenize(left_text))
    right_tokens = set(tokenize(right_text))
    if not left_tokens or not right_tokens:
        return 0.0
    union_size = len(left_tokens | right_tokens)
    if union_size == 0:
        return 0.0
    return len(left_tokens & right_tokens) / union_size


def _matched_terms(query_tokens: list[str], chunk: dict[str, Any]) -> list[str]:
    chunk_tokens = set(tokenize(str(chunk.get("chunk_text", ""))))
    matches = []
    for token in query_tokens:
        if token in chunk_tokens and token not in matches:
            matches.append(token)
        if len(matches) >= 8:
            break
    return matches
