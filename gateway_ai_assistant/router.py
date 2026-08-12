from __future__ import annotations

from dataclasses import dataclass


HYBRID_DATA_WORDS = (
    "最近",
    "今天",
    "24小时",
    "24 小时",
    "本周",
    "统计",
    "多少",
    "最多",
    "趋势",
    "变多",
    "失败率",
    "异常次数",
    "离线次数",
    "减少",
)
HYBRID_REASON_WORDS = (
    "为什么",
    "原因",
    "分析",
    "可能",
    "排查",
    "结合日志",
    "结合数据",
    "怎么处理",
    "是否是",
    "导致",
)
RUNTIME_WORDS = (
    "当前状态",
    "现在状态",
    "在线吗",
    "离线吗",
    "正常吗",
    "设备状态",
    "运行状态",
    "最近传感器",
    "传感器历史",
    "历史数据",
    "系统事件",
    "数据库概况",
    "数据库摘要",
    "database summary",
    "sensor-",
    "device_",
    "device-",
    "node-",
    "wifi-node-",
    "task_id",
)
GUIDE_WORDS = (
    "你会干什么",
    "你能干什么",
    "你能做什么",
    "你支持什么",
    "支持哪些",
    "怎么用",
    "如何使用",
    "怎么问",
    "如何提问",
    "能查什么",
    "可以问什么",
    "help",
    "usage",
)
SQL_WORDS = (
    "最近",
    "今天",
    "24小时",
    "24 小时",
    "统计",
    "多少",
    "数量",
    "条",
    "最近一次",
    "event",
    "sensor",
    "事件",
    "上报",
    "device_id",
    "ota任务",
    "ota 任务",
    "任务",
)
RAG_WORDS = (
    "是什么",
    "原理",
    "流程",
    "机制",
    "协议",
    "接口",
    "如何处理",
    "bootloader",
    "crc",
    "ota",
    "/api/",
    "要做什么",
)


@dataclass(frozen=True)
class RouteDecision:
    route_type: str
    reason: str


def classify_question(question: str) -> RouteDecision:
    normalized = question.strip().lower()
    has_data_signal = any(word in normalized for word in HYBRID_DATA_WORDS)
    has_reason_signal = any(word in normalized for word in HYBRID_REASON_WORDS)
    if has_data_signal and has_reason_signal:
        return RouteDecision("Hybrid", "matched data signal and diagnosis signal")

    if any(word in normalized for word in GUIDE_WORDS):
        return RouteDecision("Guide", "matched capability or usage keywords")

    if any(word in normalized for word in RUNTIME_WORDS):
        return RouteDecision("Runtime", "matched runtime query keywords")

    if any(word in normalized for word in SQL_WORDS):
        return RouteDecision("SQL", "matched structured query keywords")

    if any(word in normalized for word in RAG_WORDS):
        return RouteDecision("RAG", "matched documentation question keywords")

    return RouteDecision("RAG", "defaulted to documentation route")
