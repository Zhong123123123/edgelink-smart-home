from __future__ import annotations

import math
from collections import Counter
from functools import lru_cache
from typing import Sequence

import numpy as np


DEFAULT_EMBED_MODEL = "paraphrase-multilingual-MiniLM-L12-v2"


@lru_cache(maxsize=1)
def _load_sentence_transformer():
    try:
        from sentence_transformers import SentenceTransformer
    except ModuleNotFoundError:
        return None
    return SentenceTransformer(DEFAULT_EMBED_MODEL)


def embedding_backend_name() -> str:
    return "sentence_transformers" if _load_sentence_transformer() is not None else "hashing_fallback"


def embed_texts(texts: Sequence[str]) -> list[list[float]]:
    model = _load_sentence_transformer()
    if model is not None:
        vectors = model.encode(list(texts), normalize_embeddings=True)
        return [vector.astype(float).tolist() for vector in vectors]
    return [_hash_embed(text) for text in texts]


def embed_query(text: str) -> list[float]:
    return embed_texts([text])[0]


def cosine_similarity(vec_a: Sequence[float], vec_b: Sequence[float]) -> float:
    a = np.asarray(vec_a, dtype=float)
    b = np.asarray(vec_b, dtype=float)
    denom = np.linalg.norm(a) * np.linalg.norm(b)
    if denom == 0:
        return 0.0
    return float(np.dot(a, b) / denom)


def _hash_embed(text: str, dims: int = 256) -> list[float]:
    tokens = text.lower().split()
    if not tokens:
        return [0.0] * dims
    counts = Counter(tokens)
    vector = np.zeros(dims, dtype=float)
    for token, count in counts.items():
        idx = hash(token) % dims
        sign = -1.0 if hash(f"sign:{token}") % 2 else 1.0
        vector[idx] += sign * math.log1p(count)
    norm = np.linalg.norm(vector)
    if norm > 0:
        vector = vector / norm
    return vector.tolist()
