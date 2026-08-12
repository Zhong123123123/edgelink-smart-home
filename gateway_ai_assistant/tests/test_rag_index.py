from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from gateway_ai_assistant.config import Settings
from gateway_ai_assistant.rag.indexer import build_and_save_index, ensure_index, inspect_index_freshness
from gateway_ai_assistant.rag.retriever import expand_query, retrieve
from gateway_ai_assistant.sql.assistant_db import AssistantDB


class RagIndexTests(unittest.TestCase):
    def test_index_and_retrieve_protocol_definition(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            app_root = root / "gateway_ai_assistant"
            data_dir = app_root / "data"
            prompts_dir = app_root / "prompts"
            gateway_root = root / "serial-gateway"
            docs_dir = gateway_root / "docs"
            data_dir.mkdir(parents=True)
            prompts_dir.mkdir(parents=True)
            docs_dir.mkdir(parents=True)
            (gateway_root / "README.md").write_text(
                "串口帧格式：\n\nAA 55 | LEN | TYPE | PAYLOAD | CRC16\n\nCRC16 对 LEN+TYPE+PAYLOAD 计算。",
                encoding="utf-8",
            )
            (docs_dir / "protocol.md").write_text(
                "# 协议\n\nAA 55 | LEN | TYPE | PAYLOAD | CRC16\n\nLEN 表示 TYPE + PAYLOAD 长度。",
                encoding="utf-8",
            )
            settings = Settings(
                repo_root=root,
                app_root=app_root,
                data_dir=data_dir,
                reports_dir=app_root / "reports",
                prompts_dir=prompts_dir,
                index_path=data_dir / "rag_index.json",
                gateway_root=gateway_root,
                gateway_history_db=root / "history.db",
                gateway_ota_db=root / "ota.db",
                assistant_db=data_dir / "assistant.db",
                llm_api_key="",
                llm_base_url="",
                llm_model="",
            )
            assistant_db = AssistantDB(settings.assistant_db)
            payload = build_and_save_index(settings, assistant_db)
            self.assertGreater(payload["total_chunks"], 0)
            self.assertEqual(payload["schema_version"], 2)
            self.assertIn("section_title", payload["chunks"][0])
            hits = retrieve("AA55 协议帧格式是什么", settings, assistant_db, top_k=2)
            self.assertTrue(hits)
            self.assertIn("AA 55 | LEN | TYPE | PAYLOAD | CRC16", hits[0]["chunk_text"])
            self.assertIn("citation", hits[0])
            self.assertIn("protocol.md", hits[0]["citation"])
            self.assertIn("matched_terms", hits[0])

    def test_index_becomes_stale_after_source_change(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            app_root = root / "gateway_ai_assistant"
            data_dir = app_root / "data"
            prompts_dir = app_root / "prompts"
            gateway_root = root / "serial-gateway"
            docs_dir = gateway_root / "docs"
            data_dir.mkdir(parents=True)
            prompts_dir.mkdir(parents=True)
            docs_dir.mkdir(parents=True)
            readme = gateway_root / "README.md"
            readme.write_text("协议概览", encoding="utf-8")
            settings = Settings(
                repo_root=root,
                app_root=app_root,
                data_dir=data_dir,
                reports_dir=app_root / "reports",
                prompts_dir=prompts_dir,
                index_path=data_dir / "rag_index.json",
                gateway_root=gateway_root,
                gateway_history_db=root / "history.db",
                gateway_ota_db=root / "ota.db",
                assistant_db=data_dir / "assistant.db",
                llm_api_key="",
                llm_base_url="",
                llm_model="",
            )
            assistant_db = AssistantDB(settings.assistant_db)
            payload = build_and_save_index(settings, assistant_db)
            fresh = inspect_index_freshness(settings, payload)
            self.assertFalse(fresh["is_stale"])
            readme.write_text("协议概览\n\n新增内容", encoding="utf-8")
            stale = inspect_index_freshness(settings, payload)
            self.assertTrue(stale["is_stale"])
            self.assertEqual(stale["reason"], "source_modified")

    def test_ensure_index_rebuilds_stale_index(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            app_root = root / "gateway_ai_assistant"
            data_dir = app_root / "data"
            prompts_dir = app_root / "prompts"
            gateway_root = root / "serial-gateway"
            docs_dir = gateway_root / "docs"
            data_dir.mkdir(parents=True)
            prompts_dir.mkdir(parents=True)
            docs_dir.mkdir(parents=True)
            readme = gateway_root / "README.md"
            readme.write_text("协议概览", encoding="utf-8")
            settings = Settings(
                repo_root=root,
                app_root=app_root,
                data_dir=data_dir,
                reports_dir=app_root / "reports",
                prompts_dir=prompts_dir,
                index_path=data_dir / "rag_index.json",
                gateway_root=gateway_root,
                gateway_history_db=root / "history.db",
                gateway_ota_db=root / "ota.db",
                assistant_db=data_dir / "assistant.db",
                llm_api_key="",
                llm_base_url="",
                llm_model="",
            )
            assistant_db = AssistantDB(settings.assistant_db)
            build_and_save_index(settings, assistant_db)
            readme.write_text("协议概览\n\n升级后内容", encoding="utf-8")
            payload, status = ensure_index(settings, assistant_db)
            self.assertEqual(status["reason"], "rebuilt_from_source_modified")
            self.assertGreaterEqual(payload["source_count"], 1)

    def test_expand_query_adds_domain_terms(self) -> None:
        expanded = expand_query("OTA 升级失败原因")
        self.assertIn("ack", expanded.lower())
        self.assertIn("timeout", expanded.lower())


if __name__ == "__main__":
    unittest.main()
