from __future__ import annotations

import json
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any

from .config import Settings


@dataclass(frozen=True)
class LLMResponse:
    answer: str
    mode: str


class LLMClient:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings

    def answer(self, prompt: str, route_type: str, context: dict[str, Any] | None = None) -> LLMResponse:
        if not self.settings.llm_api_key:
            return self._mock_response(route_type, context)
        try:
            return self._api_response(prompt)
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, ValueError, KeyError, json.JSONDecodeError):
            return self._mock_response(route_type, context)

    def _api_response(self, prompt: str) -> LLMResponse:
        payload = {
            "model": self.settings.llm_model,
            "messages": [
                {
                    "role": "system",
                    "content": "You are an edge gateway AI assistant. Answer conservatively and never fabricate facts.",
                },
                {"role": "user", "content": prompt},
            ],
            "temperature": 0.2,
        }
        req = urllib.request.Request(
            url=f"{self.settings.llm_base_url.rstrip('/')}/chat/completions",
            data=json.dumps(payload).encode("utf-8"),
            headers={
                "Authorization": f"Bearer {self.settings.llm_api_key}",
                "Content-Type": "application/json",
            },
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=20) as resp:
            body = json.loads(resp.read().decode("utf-8"))
        answer = body["choices"][0]["message"]["content"].strip()
        return LLMResponse(answer=answer, mode="api")

    def _mock_response(self, route_type: str, context: dict[str, Any] | None) -> LLMResponse:
        prefix = "当前未配置 LLM API，以下为 mock 结果。"
        question = ""
        if context:
            question = str(context.get("question", ""))
        if route_type == "SQL":
            answer = f"{prefix} 已识别为结构化查询问题：{question or '未提供问题'}。请结合返回的 SQL 结果查看最近事件或传感器数据。"
        elif route_type == "Guide":
            answer = (
                f"{prefix} 我可以帮助你做四类事情："
                "1）查设备运行状态和传感器历史；"
                "2）查系统事件和 OTA 状态；"
                "3）回答协议、流程、接口类文档问题；"
                "4）结合数据和文档做故障分析。"
            )
        elif route_type == "Runtime":
            answer = f"{prefix} 已识别为运行态查询问题：{question or '未提供问题'}。请结合工具返回的实时状态、历史记录或 OTA 信息查看结果。"
        elif route_type == "Hybrid":
            answer = f"{prefix} 已识别为混合分析问题：先看数据库事实，再结合文档判断可能原因，当前结果仅作演示。"
        else:
            answer = f"{prefix} 已识别为文档问答问题：{question or '未提供问题'}。当前阶段可先返回引用片段或占位说明。"
        return LLMResponse(answer=answer, mode="mock")
