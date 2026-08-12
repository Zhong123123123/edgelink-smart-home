from __future__ import annotations

import unittest

from gateway_ai_assistant.rag.embedding import cosine_similarity, embed_query, embedding_backend_name


class EmbeddingTests(unittest.TestCase):
    def test_embed_query_returns_vector(self) -> None:
        vec = embed_query("AA55 协议帧格式")
        self.assertTrue(len(vec) > 0)

    def test_cosine_similarity_self_is_positive(self) -> None:
        vec = embed_query("OTA 升级流程")
        self.assertGreater(cosine_similarity(vec, vec), 0.9)

    def test_backend_name_known(self) -> None:
        self.assertIn(embedding_backend_name(), {"sentence_transformers", "hashing_fallback"})


if __name__ == "__main__":
    unittest.main()
