from __future__ import annotations

import unittest

from gateway_ai_assistant.router import classify_question


class RouterTests(unittest.TestCase):
    def test_guide_route(self) -> None:
        self.assertEqual(classify_question("你会干什么？").route_type, "Guide")

    def test_rag_route(self) -> None:
        self.assertEqual(classify_question("AA55 协议帧格式是什么？").route_type, "RAG")

    def test_sql_route(self) -> None:
        self.assertEqual(classify_question("统计 OTA 任务状态数量").route_type, "SQL")

    def test_runtime_route(self) -> None:
        self.assertEqual(classify_question("DEVICE_001 当前状态正常吗？").route_type, "Runtime")

    def test_hybrid_route(self) -> None:
        self.assertEqual(classify_question("最近 CRC 异常变多，可能是什么原因？").route_type, "Hybrid")

    def test_ota_rag_route(self) -> None:
        self.assertEqual(classify_question("OTA 流程是什么？").route_type, "RAG")

    def test_ota_hybrid_route(self) -> None:
        self.assertEqual(classify_question("最近 OTA 失败率高，帮我分析原因").route_type, "Hybrid")


if __name__ == "__main__":
    unittest.main()
