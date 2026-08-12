# Gateway AI Assistant V2a — LangGraph ReAct Agent 改造方案 (已核实现状代码)

## 实施状态更新（2026-06-30）

当前代码已经不是“纯方案阶段”，下面这些项已经落地：

- 已完成 LangGraph ReAct Agent 主链路接入，新增 `/assistant/agent/chat` 与 `/assistant/agent/resume`，保留 V1 作为降级路径。
- 已完成审批中断与恢复能力，并对非 OpenAI 兼容供应商增加人工确认兜底，避免 DeepSeek 这类 base URL 场景直接失效。
- 已完成 OTA 审批 `execute_failed` 状态，审批通过但 manifest 查询失败、`device_type` 缺失、网关建任务失败时都会显式落库。
- 已完成 OTA 自动传输链路选择。默认 `transport=serial,target=127.0.0.1:19090` 现在表示“自动模式”，会优先按设备当前活跃链路解析为 `tcp_binary`、`mqtt` 或 `serial`。
- 已完成 batch OTA 的逐设备传输覆盖，`start_next_batch()` 和 `retry_batch_failed_devices()` 不再错误复用整批固定 transport。
- 已完成 `serial-gateway` 串口降级启动。串口打不开时网关仍可启动 HTTP/监控/非串口链路，并在 `/api/status` 暴露 `serial_available=false`。
- 已补 assistant 侧串口预检查：只有解析结果仍为 `serial` 时，才会检查 `/api/status.serial_available`；若串口不可用，则阻止新建串口 OTA 审批或在执行点返回明确失败原因，不影响 `tcp_binary` / `mqtt`。

当前剩余工作主要是联调和文档收口，不是核心代码骨架未完成。

## 0. 现状核实

本方案每一处引用都经过以下文件核验：

| 引用 | 核实文件 | 关键行 |
|------|---------|--------|
| `QueryLogRecord` 字段 | `sql/assistant_db.py` | L12-16: `question, route_type, sql_used, answer_summary` |
| `ToolCallLogRecord` 字段 | `sql/assistant_db.py` | L20-26: `session_id, question, tool_name, tool_input, tool_output, status` |
| `_run_tool()` 签名与日志方式 | `service.py` | L2311-2345 |
| `ChatRequest` 模型 | `models.py` | L6-7: 仅 `question` 字段 |
| 单例 service | `app.py` | L23-24: `service = AssistantService(settings)` |
| Agent 现有路由前缀 | `api/agent_routes.py` | L12-23: `/assistant/agent/plan`, `/assistant/agent/execute`, `/assistant/agent/sessions/{session_id}` |
| 现有 service 方法名 | `service.py` | `route_sql`(L292), `route_hybrid_sql`(L1811), `list_approval_requests`(L1064), `agent_plan`(L1762), `agent_execute`(L1765), `get_agent_session`(L1799) |
| Settings 字段 | `config.py` | L10-22 |
| `agent/` 目录实际文件 | `agent/` | `__init__.py, planner.py, executor.py, tool_registry.py` (无 `state.py`) |

### 已有 `_run_tool` 机制

当前 `service.py` 的 `_run_tool()` (L2311-2345) 已实现：调用工具函数 → 自动写 `tool_call_logs` 表 → 返回 `(result, trace_item)`。V2a 会保留这套能力，但本阶段不承诺 Agent 工具默认全部经由 `_run_tool()` 落库。

---

## 1. 核心变化：与初版方案的关键差异

| 问题 | 初版方案错误 | 修正后 |
|------|------------|--------|
| DB 字段不对齐 | `QueryLogRecord` 被传了不存在的 `session_id`/`created_at` | 用真实字段 `question, route_type, sql_used, answer_summary` |
| Tool 日志字段不对 | `ToolCallLogRecord` 被传了不存在的 `args` | 用真实字段 `session_id, question, tool_name, tool_input, tool_output, status`；V2a 仅对齐字段模型，`tool_call_logs` 全量落库留到 V2b |
| 每个请求 new service | 路由示例里 `AssistantService()` 反复创建 | 复用 `app.py` 的模块级单例 + `create_xxx_router(service)` |
| 会话记忆失效 | 每次新建 `ReActAgent` + `MemorySaver` | `ReActAgent` 作为 service 的延迟属性，单例化 `MemorySaver`，`thread_id` 由客户端传入保持 |
| 写操作无技术拦截 | 只靠 prompt 约束 | 用 `HumanInTheLoopMiddleware` 审批写工具，并通过 `Command(resume=...)` 恢复 |
| 方法名杜撰 | `_execute_sql_route()`, `list_approvals()` | 用真实方法 `route_sql()`, `route_hybrid_sql()`, `list_approval_requests()` |
| API 路径不一致 | `/agent/chat`, `/agent/stream` | 加在现有 `/assistant/agent/` 前缀下: `/assistant/agent/chat`, `/assistant/agent/resume` |
| 引用不存在的文件 | `agent/state.py` | 不引用任何不存在文件 |
| `ChatRequest` 模型 | 称"轻改"但不改模型 | 新增 `AgentChatRequest(BaseModel)` 含 `question` + `session_id` |
| 依赖缺失 | `langchain-openai` 没在 requirements diff | 正确列出 `langgraph`, `langchain`, `langchain-core`, `langchain-openai`, `openai` |

---

## 2. V2a 范围定义：只做安全可落地的部分

```
V2a (本方案):
  ✅ 只读工具 10 个 (设备状态/传感器/事件/文档/统计/诊断/风险检查/审批列表/报告列表/数据库概况)
  ✅ 写工具 4 个 (创建审批/批准/暂停批次/重试失败)，由 HumanInTheLoopMiddleware 硬拦截
  ✅ 单例 service + 持久化 session memory
  ✅ query 日志接入现有 assistant_db；tool_call 审计分阶段落地
  ✅ API 路径与现有 /assistant/agent/* 一致

V2b (后续):
  ⏸ OTA 批量创建/确认的完整 interrupt + approve 闭环
  ⏸ 审批步骤化 (approval_steps 表)
  ⏸ Streaming SSE 端点
```

---

## 3. 文件级改动（全部核实现有代码后确定）

### 新增 (3 个文件)

```
gateway_ai_assistant/
├── agent/
│   ├── langgraph_tools.py       # [新增] LangChain Tool 定义
│   └── react_agent.py           # [新增] ReAct Agent 封装
└── prompts/
    └── system_prompt.txt        # [新增] Agent System Prompt
```

### 修改 (5 个文件)

| 文件 | 改什么 | 为什么 |
|------|--------|--------|
| `requirements.txt` | +5 行依赖 | `langgraph`, `langchain`, `langchain-core`, `langchain-openai`, `openai` |
| `config.py` | +2 字段 | `agent_max_iterations: int = 10`, `agent_recursion_limit: int = 25` |
| `models.py` | +1 类 | `AgentChatRequest(question, session_id)` |
| `service.py` | +1 property, +2 方法 | `react_agent` property, `chat_agent()`, `chat_agent_resume()` |
| `api/agent_routes.py` | +2 端点 | `/assistant/agent/chat`, `/assistant/agent/resume` |

### 不修改的文件

- `llm_client.py` — 保留不动。ReAct Agent 内部用 `langchain_openai.ChatOpenAI` 直连 LLM，不影响 V1 的问答链路。
- `router.py` — 保留不动。关键词路由是 Agent 不可用时的降级链路。
- `agent/planner.py`, `agent/executor.py`, `agent/tool_registry.py` — 保留不动。V1 Agent 路径继续可用。
- `sql/assistant_db.py` — 保留不动。所有表结构和方法已经满足需求。
- `api/assistant_routes.py` — 保留不动。`/assistant/chat` 不改签名，内部增加 Agent 优先 + V1 降级逻辑。

---

## 4. 核心代码（对齐真实代码）

### 4.1 `agent/langgraph_tools.py`

每个 Tool 必须：
- 参数用 Pydantic `BaseModel` + `Field(description=...)`，LLM function calling 以此理解
- 函数体调用 service 上真实存在的方法
- 返回 `dict`（统一 `{"ok": bool, ...}` 格式）

```python
# agent/langgraph_tools.py

from __future__ import annotations

from typing import Optional
from pydantic import BaseModel, Field
from langchain_core.tools import tool


# ──── 参数模型 ────

class DeviceStatusInput(BaseModel):
    device_ref: str = Field(description="设备标识: 数字如'1'，或名称如'sensor-01'/'DEVICE_001'")

class SensorHistoryInput(BaseModel):
    device_ref: str = Field(description="设备标识")
    limit: int = Field(default=5, description="返回条数 1-20")

class SystemEventsInput(BaseModel):
    limit: int = Field(default=10, description="返回条数 1-100")

class OtaRiskCheckInput(BaseModel):
    device_id: int = Field(description="设备数字 ID")
    firmware_id: str = Field(description="固件 ID，如'esp32_v2.1.0'")

class OtaRequestCreateInput(BaseModel):
    device_id: int = Field(description="设备数字 ID")
    firmware_id: str = Field(description="固件 ID")
    transport: str = Field(default="serial")
    target: str = Field(default="127.0.0.1:19090")

class DiagnoseFaultInput(BaseModel):
    device_id: int = Field(description="设备数字 ID")
    limit: int = Field(default=10, description="查询最近 N 条记录，1-20")

class RAGQueryInput(BaseModel):
    question: str = Field(description="需从项目文档中检索的具体问题")

class SQLQueryInput(BaseModel):
    question: str = Field(description="描述需统计/查询的数据，如'统计 OTA 失败数量'或'最近传感器上报'")

class OtaBatchPlanInput(BaseModel):
    device_ids: list[int] = Field(description="设备 ID 列表，如 [1, 2, 3]")
    firmware_id: str = Field(description="固件 ID")
    batch_size: int = Field(default=2, description="每批设备数")

class ApproveOtaInput(BaseModel):
    approval_id: str = Field(description="审批 ID")
    approved_by: str = Field(default="operator", description="批准人")

class PauseBatchInput(BaseModel):
    batch_run_id: str = Field(description="批量运行 ID")
    operator: str = Field(default="operator")

class RetryFailedInput(BaseModel):
    approval_id: str = Field(description="审批 ID")
    operator: str = Field(default="operator")


# ──── 工具创建函数 ────

def create_readonly_tools(service):
    """创建只读工具集 (10 个)。LLM 可自由调用，无人工拦截。"""
    from ..rag.retriever import retrieve

    @tool(args_schema=DeviceStatusInput)
    def get_device_status(device_ref: str) -> dict:
        """查询设备当前在线状态和最近 OTA 状态。当用户问'XX 设备状态'、'在线吗'时使用。"""
        return service.runtime_gateway.get_device_status(device_ref)

    @tool(args_schema=SensorHistoryInput)
    def get_sensor_history(device_ref: str, limit: int = 5) -> dict:
        """查询设备最近传感器上报记录(温度/湿度/灯/WiFi 信号等)。当需要设备实际工作数据时使用。"""
        return service.runtime_gateway.get_sensor_history(device_ref, limit)

    @tool(args_schema=SystemEventsInput)
    def get_system_events(limit: int = 10) -> dict:
        """查询网关最近系统事件(设备上下线/心跳超时/CRC 错误等)。用于排查系统层面问题。"""
        return service.runtime_gateway.get_system_events(limit)

    @tool
    def get_database_summary() -> dict:
        """获取网关数据库概况: 各表行数、时间跨度、设备数。用于了解整体数据规模。"""
        return service.runtime_gateway.get_database_summary()

    @tool(args_schema=RAGQueryInput)
    def search_documents(question: str) -> dict:
        """从项目文档(协议说明/OTA流程/API文档)中检索相关知识。当用户问'XX是什么'、'XX协议格式'时使用。"""
        chunks = retrieve(question, service.settings, service.assistant_db, top_k=3)
        return {
            "ok": True,
            "retrieved_chunks": [
                {"citation": c["citation"], "text": c["chunk_text"], "score": c["score"]}
                for c in chunks
            ],
            "count": len(chunks),
        }

    @tool(args_schema=SQLQueryInput)
    def query_structured_data(question: str) -> dict:
        """执行结构化数据查询。用于统计/计数/最近N条记录。如'统计OTA状态'、'最近事件'。先搜文档确定概念，再调用本工具查数据。"""
        from ..router import classify_question
        decision = classify_question(question)
        if decision.route_type == "SQL":
            result = service.route_sql(question)
            return result.to_dict() if hasattr(result, 'to_dict') else {"ok": True, "result": str(result)}
        if decision.route_type == "Hybrid":
            result = service.route_hybrid_sql(question)
            return result if isinstance(result, dict) else {"ok": True, "result": result}
        return {"ok": False, "message": "该问题不适用结构化查询，请用 search_documents。"}

    @tool(args_schema=OtaRiskCheckInput)
    def ota_risk_check(device_id: int, firmware_id: str) -> dict:
        """OTA 升级前风险检查: 检查设备在线、固件存在、无未完成任务、历史无大量失败。调用后根据结果决定是否继续。"""
        return service.workflow_ota_risk_check(device_id, firmware_id)

    @tool(args_schema=DiagnoseFaultInput)
    def diagnose_fault(device_id: int, limit: int = 10) -> dict:
        """基于规则诊断设备故障: 检查离线/无传感器数据/心跳超时/CRC错误/反复OTA失败。返回发现列表和严重程度。"""
        return service.workflow_diagnose_fault(device_id, limit)

    @tool
    def list_approvals() -> dict:
        """列出所有 OTA 审批请求(含待审批/已批准/已拒绝)。用于查看审批状态。"""
        return service.list_approval_requests()

    @tool
    def list_reports() -> dict:
        """列出已生成的故障报告和测试报告。"""
        return service.list_reports()

    return [
        get_device_status,
        get_sensor_history,
        get_system_events,
        get_database_summary,
        search_documents,
        query_structured_data,
        ota_risk_check,
        diagnose_fault,
        list_approvals,
        list_reports,
    ]


def create_write_tools(service):
    """创建写操作工具集 (4 个)。这些工具会被 HITL middleware 审批。"""
    @tool(args_schema=OtaRequestCreateInput)
    def ota_create_request(device_id: int, firmware_id: str, transport: str = "serial", target: str = "127.0.0.1:19090") -> dict:
        """创建 OTA 审批请求(不直接下发任务)。创建后需人工批准。调用前必须已完成风险检查且无阻塞项。"""
        return service.workflow_ota_request_create(device_id, firmware_id, transport, target)

    @tool(args_schema=ApproveOtaInput)
    def approve_ota_request(approval_id: str, approved_by: str = "operator") -> dict:
        """批准 OTA 审批请求，批准后自动创建真实 OTA 任务。此操作会触发固件下发，必须确认审批 ID 正确。"""
        return service.workflow_ota_request_confirm(approval_id, approved_by)

    @tool(args_schema=PauseBatchInput)
    def pause_batch(batch_run_id: str, operator: str = "operator") -> dict:
        """暂停进行中的 OTA 灰度批次，后续批次不会被自动下发。"""
        return service.pause_batch_run(batch_run_id, operator)

    @tool(args_schema=RetryFailedInput)
    def retry_failed_devices(approval_id: str, operator: str = "operator") -> dict:
        """重试批量 OTA 中失败的设备，避免重跑整批。"""
        return service.retry_batch_failed_devices(approval_id, operator)

    return [
        ota_create_request,
        approve_ota_request,
        pause_batch,
        retry_failed_devices,
    ]
```

### 4.2 `agent/react_agent.py`

```python
# agent/react_agent.py

from __future__ import annotations

from datetime import datetime, timezone
from typing import Any

from langgraph.checkpoint.memory import MemorySaver
from langgraph.types import Command
from langchain.agents import create_agent
from langchain.agents.middleware import HumanInTheLoopMiddleware
from langchain_core.messages import HumanMessage, AIMessage

from .langgraph_tools import create_readonly_tools, create_write_tools


SYSTEM_PROMPT = """你是边缘网关智能运维助手。运行在 Linux 网关上，负责设备运维、故障诊断和 OTA 升级管理。

## 你的工具
系统提供两类工具：
- **只读工具** (可直接调用): get_device_status, get_sensor_history, get_system_events, get_database_summary, search_documents, query_structured_data, ota_risk_check, diagnose_fault, list_approvals, list_reports
- **写操作工具** (需人工确认才能执行): ota_create_request, approve_ota_request, pause_batch, retry_failed_devices

## 工作原则
1. **循序渐进**: 诊断设备问题按 查状态→查历史→查事件→诊断 的顺序，不要跳步。
2. **数据优先**: 回答引用实际查询结果，区分"数据库显示的事实"和"基于事实的推断"。
3. **明确告知**: 如果推理需要写操作，先总结当前掌握的信息，明确说明**为什么**需要那个操作、**有什么风险**，请求用户确认。用户同意后你可以调用写工具。
4. **工具选择**: 结构化统计用 query_structured_data，文档/协议/流程用 search_documents，设备状态用 get_device_status。
5. **查不到就说**: 如果多次查询都没有结果，如实告知用户，不要编造数据。
6. **中文回答**: 始终用中文回复。

## OTA 操作的特殊要求
- 升级前必须先做 ota_risk_check，检查通过才能继续。
- 如果风险检查有阻塞项，必须向用户说明原因，不能强行创建审批。
- approve_ota_request 会真正下发固件，调用前必须确认审批 ID 和用户意图。
"""

# 写工具名称列表，用于 HITL 审批
WRITE_TOOL_NAMES = [
    "ota_create_request",
    "approve_ota_request",
    "pause_batch",
    "retry_failed_devices",
]


class ReActAgent:
    """LangGraph ReAct Agent 封装。作为 AssistantService 的单例依赖。"""

    def __init__(self, settings, assistant_db, service):
        self.settings = settings
        self.assistant_db = assistant_db
        self.service = service

        # 构建工具: 只读 + 写操作
        self.readonly_tools = create_readonly_tools(service)
        self.write_tools = create_write_tools(service)
        self.all_tools = self.readonly_tools + self.write_tools

        # LLM (langchain_openai 直连，不影响 V1 的 LLMClient)
        from langchain_openai import ChatOpenAI
        self.llm = ChatOpenAI(
            model=settings.llm_model,
            api_key=settings.llm_api_key,
            base_url=settings.llm_base_url,
            temperature=0.2,
            timeout=30,
        )

        # Checkpointer: 单例 service 内共享，通过 thread_id 隔离 session
        self._checkpointer = MemorySaver()

        # 官方推荐方案: create_agent + HumanInTheLoopMiddleware
        self._agent = create_agent(
            model=self.llm,
            tools=self.all_tools,
            checkpointer=self._checkpointer,
            middleware=[
                HumanInTheLoopMiddleware(
                    interrupt_on={
                        "ota_create_request": {"allowed_decisions": ["approve", "reject"]},
                        "approve_ota_request": {"allowed_decisions": ["approve", "reject"]},
                        "pause_batch": {"allowed_decisions": ["approve", "reject"]},
                        "retry_failed_devices": {"allowed_decisions": ["approve", "reject"]},
                    },
                )
            ],
            system_prompt=SYSTEM_PROMPT,
        )

    def invoke(self, question: str, session_id: str) -> dict[str, Any]:
        """同步调用 Agent。session_id 即 LangGraph thread_id。"""
        config = {"configurable": {"thread_id": session_id}}
        messages = [HumanMessage(content=question)]

        started_at = datetime.now(timezone.utc)
        tool_trace: list[dict[str, Any]] = []

        try:
            result = self._agent.invoke(
                {"messages": messages},
                config=config,
                version="v2",
            )

            final_messages = result.get("messages", [])
            answer = ""
            for msg in final_messages:
                if isinstance(msg, AIMessage):
                    if msg.content and not msg.tool_calls:
                        answer = msg.content
                    if msg.tool_calls:
                        for tc in msg.tool_calls:
                            tool_trace.append({
                                "tool": tc.get("name", ""),
                                "args": tc.get("args", {}),
                                "step": len(tool_trace) + 1,
                            })

            interrupt_requests = result.get("interrupts", [])
            interrupted = bool(interrupt_requests)
            if interrupted:
                action_requests = interrupt_requests[0].get("action_requests", [])
                first_action = action_requests[0] if action_requests else {}
                answer = (
                    "Agent 请求执行高风险写操作，已暂停等待人工审批。\n"
                    f"工具: {first_action.get('name', '')}\n"
                    f"参数: {first_action.get('args', {})}\n"
                    "请调用 /assistant/agent/resume 继续。"
                )

            finished_at = datetime.now(timezone.utc).isoformat()

            # 写入治理日志: 对齐 QueryLogRecord 真实字段
            self.assistant_db.log_query(
                QueryLogRecord(
                    question=question,
                    route_type="react_agent",
                    sql_used="",
                    answer_summary=answer[:200] if answer else "",
                )
            )

            return {
                "ok": True,
                "session_id": session_id,
                "answer": answer,
                "tool_trace": tool_trace,
                "tool_call_count": len(tool_trace),
                "interrupted": interrupted,
                "mode": "react_agent",
                "started_at": started_at.isoformat(),
                "finished_at": finished_at,
            }

        except Exception as exc:
            return {
                "ok": False,
                "session_id": session_id,
                "error": f"{type(exc).__name__}: {exc}",
                "tool_trace": tool_trace,
                "mode": "react_agent",
            }

    def resume(self, session_id: str) -> dict[str, Any]:
        """恢复被 HITL 暂停的 Agent 执行。当前 V2a 仅支持 approve/reject。"""
        config = {"configurable": {"thread_id": session_id}}

        try:
            result = self._agent.invoke(
                Command(resume={"decisions": [{"type": "approve"}]}),
                config=config,
                version="v2",
            )

            final_messages = result.get("messages", [])
            answer = ""
            for msg in final_messages:
                if isinstance(msg, AIMessage) and msg.content and not msg.tool_calls:
                    answer = msg.content

            return {
                "ok": True,
                "session_id": session_id,
                "answer": answer,
                "mode": "react_agent_resumed",
            }
        except Exception as exc:
            return {
                "ok": False,
                "session_id": session_id,
                "error": f"{type(exc).__name__}: {exc}",
            }

    def get_state(self, session_id: str) -> dict[str, Any]:
        """获取 session 当前状态（用于 debug/审计）。"""
        config = {"configurable": {"thread_id": session_id}}
        try:
            state = self._agent.get_state(config)
            return {"ok": True, "session_id": session_id, "state": str(state)}
        except Exception as exc:
            return {"ok": False, "error": f"{type(exc).__name__}: {exc}"}


from ..sql.assistant_db import QueryLogRecord
```

### 4.3 `service.py` 改动（增量，不删不改现有代码）

在 `AssistantService.__init__()` 末尾加一行：

```python
# service.py __init__ 末尾，L87 之后追加:
        self._react_agent = None  # 延迟初始化
```

在 `AssistantService` 类中新增以下方法（追加到文件末尾即可）：

```python
# service.py 新增方法

    @property
    def react_agent(self):
        """延迟初始化 V2a ReAct Agent。
        仅在 LLM_API_KEY 已配置时可用；否则抛异常，调用方应降级到 V1 路由。
        单例模式: 同一 service 实例多次访问返回同一 agent，MemorySaver 保持 session 记忆。
        """
        if self._react_agent is None:
            if not self.settings.llm_api_key:
                raise RuntimeError("LLM_API_KEY not configured")
            from .agent.react_agent import ReActAgent
            self._react_agent = ReActAgent(
                settings=self.settings,
                assistant_db=self.assistant_db,
                service=self,
            )
        return self._react_agent

    def chat_agent(self, question: str, session_id: str = "") -> dict[str, Any]:
        """V2a Agent 对话入口。session_id 持久化多轮记忆。"""
        session_id = session_id or f"agent-{uuid4().hex[:12]}"
        return self.react_agent.invoke(question, session_id)

    def chat_agent_resume(self, session_id: str) -> dict[str, Any]:
        """恢复被 HITL middleware 暂停的 Agent 执行。"""
        return self.react_agent.resume(session_id)

    def chat(self, question: str, session_id_override: str = "") -> dict[str, Any]:
        """原有 chat 方法增强: Agent 优先 + V1 降级。
        当 LLM_API_KEY 已配置时默认走 ReAct Agent；
        当 Agent 不可用或出错时回退到 V1 关键词路由。
        """
        # 尝试 V2a Agent
        if self.settings.llm_api_key:
            try:
                result = self.chat_agent(question, session_id_override)
                if result.get("ok"):
                    return result
            except Exception:
                pass  # Agent 失败 → 降级到 V1

        # V1 路由逻辑 (原有 chat 方法体, 字段完全不变)
        question = question.strip()
        decision = classify_question(question)
        session_id = session_id_override or self._new_session_id()
        # ... 后续与当前 service.py L129-198 完全一致 ...
```

> **注意**: 上面 `chat()` 的 V1 降级部分省略了详细代码。实际落地时保留原 L129-198 不变，仅在顶部加 Agent 优先逻辑。

### 4.4 `models.py` 改动

```python
# models.py 新增一个请求模型
class AgentChatRequest(BaseModel):
    question: str
    session_id: str = ""  # 空字符串表示新 session
```

### 4.5 `api/agent_routes.py` 改动

```python
# api/agent_routes.py, 在 create_agent_router 内新增 2 个端点

from ..models import AgentRequest, AgentChatRequest  # AgentChatRequest 为新增

def create_agent_router(service: AssistantService) -> APIRouter:
    router = APIRouter()

    # ──── 保留现有 3 个端点不变 ────

    @router.post("/assistant/agent/plan")
    def agent_plan(payload: AgentRequest) -> dict:
        return service.agent_plan(payload.question)

    @router.post("/assistant/agent/execute")
    def agent_execute(payload: AgentRequest) -> dict:
        return service.agent_execute(payload.question)

    @router.get("/assistant/agent/sessions/{session_id}")
    def agent_session(session_id: str) -> dict:
        return service.get_agent_session(session_id)

    # ──── V2a 新增 ────

    @router.post("/assistant/agent/chat")
    def agent_chat(payload: AgentChatRequest) -> dict:
        """V2a ReAct Agent 对话。LLM 自主选择工具，支持多步推理。"""
        try:
            return service.chat_agent(payload.question, payload.session_id)
        except RuntimeError:
            from fastapi import HTTPException
            raise HTTPException(
                status_code=503,
                detail="LLM_API_KEY not configured. Agent mode requires a valid API key.",
            )

    @router.post("/assistant/agent/resume")
    def agent_resume(payload: AgentChatRequest) -> dict:
        """恢复因写操作拦截而暂停的 Agent 执行。"""
        if not payload.session_id:
            from fastapi import HTTPException
            raise HTTPException(status_code=400, detail="session_id is required to resume")
        return service.chat_agent_resume(payload.session_id)

    return router
```

### 4.6 `config.py` 改动

```python
# config.py Settings dataclass 追加 2 个字段 (在 llm_model 之后)
    agent_max_iterations: int = 10
    agent_recursion_limit: int = 25

# get_settings() factory 中追加:
    agent_max_iterations=int(os.getenv("AGENT_MAX_ITERATIONS", "10")),
    agent_recursion_limit=int(os.getenv("AGENT_RECURSION_LIMIT", "25")),
```

### 4.7 `requirements.txt` 改动

```diff
 fastapi>=0.115,<1
 uvicorn>=0.30,<1
 pydantic>=2,<3
 streamlit>=1.45,<2
 sentence-transformers>=3,<4
+langgraph>=0.2,<1
+langchain>=1,<2
+langchain-core>=0.3,<1
+langchain-openai>=0.2,<1
+openai>=1.0,<2
```

---

## 5. 会话记忆设计（对齐单例 service 模式）

```
app.py 启动
  │
  ├── service = AssistantService(settings)    # 模块级单例
  │     └── service._react_agent = None
  │
  ├── app.include_router(create_assistant_router(service))
  │     └── /assistant/chat → service.chat()
  │                              │
  │                   LLM_API_KEY 已配置?
  │                    ├── YES → service.chat_agent(question, session_id)
  │                    │           └── service.react_agent (延迟初始化)
  │                    │                 └── ReActAgent._checkpointer = MemorySaver()  # 单例
  │                    │                       │
  │                    │              thread_id = session_id (客户端传入)
  │                    │              └── LangGraph 自动用 thread_id 隔离 session
  │                    │                    │
  │                    │    请求1: session_id="abc" → 记忆A
  │                    │    请求2: session_id="abc" → 复用记忆A (多轮对话生效)
  │                    │    请求3: session_id="xyz" → 记忆B (独立 session)
  │                    │
  │                    └── NO → V1 关键词路由 (原有逻辑)
  │
  └── app.include_router(create_agent_router(service))
        ├── /assistant/agent/chat  → service.chat_agent()
        └── /assistant/agent/resume → service.chat_agent_resume()
```

**关键点**:
1. `ReActAgent` 是 `service` 的延迟属性，同一 service 实例只创建一次。
2. `MemorySaver` 在 `ReActAgent.__init__` 中创建一次，全局共享。
3. 每个 session 通过 LangGraph 的 `config["configurable"]["thread_id"]` 隔离。
4. `session_id` 由客户端生成并传入（`AgentChatRequest.session_id`），空字符串则服务端新建。

---

## 6. 写操作安全：HumanInTheLoopMiddleware 硬拦截

### 拦截流程

```
LLM 决定调用 approve_ota_request
  │
  ▼
HumanInTheLoopMiddleware 检查: 工具名在 interrupt_on 中?
  │
  ├── YES → 暂停执行
  │        将待审批 tool call 写入 checkpoint
  │        返回 interrupts 给客户端 (interrupted=true)
  │        等待 POST /assistant/agent/resume
  │          │
  │          ├── resume → Command(resume={"decisions": [{"type": "approve"}]})
  │          │            → 继续执行 tool call
  │          └── 不 resume → checkpoint 保留，session 可后续查询
  │
  └── NO (未在 interrupt_on 中列出) → 直接执行
```

### 在代码中的体现

```python
# react_agent.py __init__
self._agent = create_agent(
    model=self.llm,
    tools=self.all_tools,
    checkpointer=self._checkpointer,
    middleware=[
        HumanInTheLoopMiddleware(
            interrupt_on={
                "ota_create_request": {"allowed_decisions": ["approve", "reject"]},
                "approve_ota_request": {"allowed_decisions": ["approve", "reject"]},
                "pause_batch": {"allowed_decisions": ["approve", "reject"]},
                "retry_failed_devices": {"allowed_decisions": ["approve", "reject"]},
            },
        )
    ],
    system_prompt=SYSTEM_PROMPT,
)
```

这里采用官方推荐的 HITL middleware。未列出的工具默认不触发拦截。审批恢复时使用 `Command(resume={"decisions": [{"type": "approve"}]})`，和 LangChain v1 文档一致。

---

## 7. 治理日志路径

```
ReAct Agent 调用工具
  │
  ├── LangGraph/HITL 层:
  │     - tool_call / interrupt request 保存在 checkpoint state
  │     - 可通过 get_state(session_id) 查询当前暂停点
  │
  ├── react_agent.invoke():
  │     └── assistant_db.log_query(QueryLogRecord(
  │           question=..., route_type="react_agent",
  │           sql_used="", answer_summary=answer[:200]))
  │           # ↑ 对齐 QueryLogRecord 真实字段
  │
  └── tool_call_logs:
        V2a 默认不承诺“全量复用 _run_tool”。
        原因: LangChain Tool 被 agent 直接调用，默认不会自动经过 service._run_tool()。
        若要与 V1 完全一致，需在每个工具函数内部显式包装:
        service._run_tool(session_id, question, tool_name, tool_input, fn)
```

> **V2a 审计边界**: 先保证 `query_logs` 正确落库，并依赖 LangGraph checkpoint 保存 tool_call / interrupt 状态。`tool_call_logs` 的 V1 风格全量落库放到 V2b，再统一做 `service._run_tool()` 包装适配。

---

## 8. 实施步骤（完全对齐真实代码）

### Phase 1: 基础设施 (预计 2 小时)

```bash
# 1. 安装依赖
pip install langgraph langchain langchain-core langchain-openai openai

# 2. 更新 requirements.txt (加 5 行)
# 3. config.py 加 agent_max_iterations / agent_recursion_limit
# 4. models.py 加 AgentChatRequest
```

验证: `python3 -c "from langchain.agents import create_agent; from langchain.agents.middleware import HumanInTheLoopMiddleware; print('ok')"`

### Phase 2: 核心代码 (预计 3 小时)

1. 写 `prompts/system_prompt.txt`
2. 写 `agent/langgraph_tools.py` (先 10 个只读工具，每个函数体已验证 service 上方法存在)
3. 写 `agent/react_agent.py` (导入真实 `QueryLogRecord`，接入 HITL middleware)
4. 在 `service.py` 加 `react_agent` property 和 `chat_agent()` 方法 (不改现有方法)

验证: 
```python
# 在 python REPL 中
from gateway_ai_assistant.service import AssistantService
from gateway_ai_assistant.config import get_settings
svc = AssistantService(get_settings())
result = svc.chat_agent("DEVICE_001 当前状态正常吗？")
assert result["ok"]
assert len(result["tool_trace"]) > 0  # 应该调用了 get_device_status
```

### Phase 3: API 接入 + Session 记忆 + 降级 (预计 2 小时)

1. `api/agent_routes.py` 加 `/assistant/agent/chat` 和 `/assistant/agent/resume`
2. `service.py` 的 `chat()` 加 Agent 优先 + V1 降级逻辑
3. 验证 session 记忆: 同一 `session_id` 两次请求，第二次应能引用第一次上下文

验证:
```bash
# 启动服务
python3 -m uvicorn gateway_ai_assistant.app:app --port 8010

# 测试 Agent 模式
curl -X POST http://127.0.0.1:8010/assistant/agent/chat \
  -H "Content-Type: application/json" \
  -d '{"question": "DEVICE_001 状态如何？", "session_id": "test-session-1"}'

# 测试多轮记忆 (第二次请求用同一 session_id)
curl -X POST http://127.0.0.1:8010/assistant/agent/chat \
  -H "Content-Type: application/json" \
  -d '{"question": "它最近的传感器数据呢？", "session_id": "test-session-1"}'

# 测试 V1 降级 (不设 LLM_API_KEY)
LLM_API_KEY="" python3 -m uvicorn gateway_ai_assistant.app:app --port 8010
curl -X POST http://127.0.0.1:8010/assistant/chat \
  -H "Content-Type: application/json" \
  -d '{"question": "AA55 协议帧格式是什么？"}'
```

### Phase 4: 写工具 + HITL 审批验证 (预计 1.5 小时)

1. 补全 4 个写工具
2. 测试 OTA 审批完整流程:

```bash
# Step 1: 风险检查
curl -X POST http://127.0.0.1:8010/assistant/agent/chat \
  -H "Content-Type: application/json" \
  -d '{"question": "给设备1做 esp32_v2.1.0 的 OTA 升级", "session_id": "ota-test-1"}'

# Agent 应先调用 ota_risk_check，然后可能调用 ota_create_request
# 如果调用 ota_create_request，会被 HITL middleware 拦截，返回 interrupted=true / interrupts 非空

# Step 2: 人工确认继续
curl -X POST http://127.0.0.1:8010/assistant/agent/resume \
  -H "Content-Type: application/json" \
  -d '{"question": "确认", "session_id": "ota-test-1"}'
```

---

## 9. 当前 V1 代码的不变部分

以下文件/方法**完全不动**:

| 文件 | 保留原因 |
|------|---------|
| `llm_client.py` | V1 RAG/SQL/Hybrid 路由的 LLM 调用继续用 |
| `router.py` | Agent 不可用时的降级路由 |
| `agent/planner.py` | V1 agent_plan/agent_execute 继续可用 |
| `agent/executor.py` | V1 agent_plan/agent_execute 继续可用 |
| `agent/tool_registry.py` | V1 executor 依赖 |
| `api/assistant_routes.py` | `/assistant/chat` 内部加降级逻辑，但 API 签名不变 |
| `api/workflow_routes.py` | 直接调用 service 方法，不经过 Agent |
| `sql/assistant_db.py` | 所有表结构和方法满足 V2a 需求 |
| `sql/query_templates.py` | SQL 模板继续被 `route_sql` 使用 |
| `rag/` 全部 | RAG 被 Agent 的 `search_documents` 工具复用 |
| `diagnostics/` | 被 Agent 的 `diagnose_fault` 工具复用 |
| `workflows/` | 被 Agent 的工具函数复用 |
| `tools/` | `RuntimeGateway` 和 `OtaTools` 被工具函数复用 |

---

## 10. 改动汇总

```
新增:
  agent/langgraph_tools.py     ~200 行
  agent/react_agent.py         ~180 行
  prompts/system_prompt.txt    ~30 行

修改 (增量, 不删不改现有代码):
  requirements.txt             +5 行
  config.py                    +2 字段 + 2 行 factory
  models.py                    +1 模型类
  service.py                   +1 property + 2 方法 + chat() 顶部加 Agent 优先
  api/agent_routes.py          +2 端点

总计: ~450 行新增, ~20 行修改
```

---

## 11. V2b 规划：从 7/10 → 8.5/10

### 当前进度（2026-06-30）

- `P0 / HITL 决策补全`：已完成。`/assistant/agent/resume` 已支持 `approve / reject / edit`。
- `P1 / Streaming SSE`：已完成。已新增 `/assistant/agent/stream`，支持 `token / tool_start / tool_end / final_answer / interrupt / error / done` 事件。
- `P1 / Eval Framework`：已完成最小骨架。已拆为 `golden regression` 与可选 `live eval smoke`。
- `P2 / Context Management`：已完成轻量版观测，不做危险的摘要改写。`get_agent_session()` 已返回 `agent_context` 与 `context_warning`，可观测消息数与粗略上下文体积。
- `P2 / Prompt 增强`：已完成首轮 few-shot 示例补充。
- `P3 / Multi-Agent`：已完成第一阶段落地。后端已新增只读 `DiagnosisTeam`，并接入 `workflow_diagnose_fault()` 与 `workflow_generate_report(report_type=\"fault_ticket\")`；当前剩余工作主要是前端展示和是否继续扩展到更多 team。

### 短板分布

```
                   当前(7.0/10)         目标(8.5/10)
Agent 架构        ███████▌ 7.5/10     ████████▌ 8.5/10
LLM & Prompt      ██████▌  6.5/10     ████████  8.0/10
工程健壮性        ████████ 8.0/10     █████████ 9.0/10
完整度与生产化    █████▌   5.5/10     ████████▌ 8.5/10
```

### 优先级排序

```
P0 (必须先补):  HITL 决策补全 + Resume 语义统一
P1 (生产体验):  Streaming SSE + 稳定版 Eval Framework
P2 (审慎推进):  轻量 Context Management + Prompt 增强
P3 (可选探索):  Multi-Agent 初探
```

---

### P0: HITL 决策补全（预计 1 天）

**问题**：当前 `resume()` 只支持 `approve`。而且 native HITL 与 manual fallback 是两条不同分支，恢复语义还不统一。

当前代码现状：

- `ReActAgent.resume()` 仅发送 `Command(resume={"decisions": [{"type": "approve"}]})`
- native HITL 走 `HumanInTheLoopMiddleware`
- 非 OpenAI 兼容模型走 `_pending_manual_actions` 本地兜底
- `/assistant/agent/resume` 仍复用 `AgentChatRequest`

这意味着当前最先要补的是“拒绝 / 修改参数 / 统一状态回传”，而不是先做流式输出。

**方案**：新增专用 resume 请求模型，把 `approve / reject / edit` 三种决策显式化，并统一 native HITL 与 manual fallback 的返回结构。

```python
# models.py 新增
class AgentResumeRequest(BaseModel):
    session_id: str
    decision: str = "approve"   # approve | reject | edit
    edit_args: dict = Field(default_factory=dict)

# react_agent.py resume() 修改
def resume(self, session_id: str, decision: str = "approve", edit_args: dict | None = None) -> dict:
    ...
    if decision == "approve":
        resume_cmd = Command(resume={"decisions": [{"type": "approve"}]})
    elif decision == "reject":
        resume_cmd = Command(resume={"decisions": [{"type": "reject", "message": "审批人已拒绝"}]})
    elif decision == "edit":
        resume_cmd = Command(resume={"decisions": [{"type": "edit", "editedAction": edit_args or {}}]})
    else:
        return {"ok": False, "error": "unsupported decision"}
```

**涉及文件**：
- `models.py` — +`AgentResumeRequest`
- `agent/react_agent.py` — `resume()` 扩展
- `api/agent_routes.py` — `/resume` 改接收 `AgentResumeRequest`
- `service.py` — `chat_agent_resume()` 透传决策参数

**面试价值**：
> 这不是“演示型改进”，而是把高风险写操作的审批闭环补完整。面试时能说清 approve/reject/edit 三种恢复语义，比单纯展示 SSE 更有说服力。

---

### P1: Streaming SSE（预计 1 天）

**问题**：当前 `invoke()` 全程同步阻塞。用户等 5-10 秒才看到第一个字。

**方案**：在 `ReActAgent` 中加 `astream()` 方法，用 `astream_events()` 推送 SSE，但事件协议必须一次定义完整，不能只推 token。

```python
async def astream(self, question: str, session_id: str) -> AsyncIterator[dict]:
    config = {"configurable": {"thread_id": session_id}}
    async for event in self._agent.astream_events(
        {"messages": [HumanMessage(content=question)]},
        config=config,
        version="v2",
    ):
        kind = event.get("event", "")
        if kind == "on_chat_model_stream":
            ...
            yield {"type": "token", "content": "..."}
        elif kind == "on_tool_start":
            yield {"type": "tool_start", "tool": "...", "input": {...}}
        elif kind == "on_tool_end":
            yield {"type": "tool_end", "tool": "...", "output_ok": True}
    yield {"type": "done", "session_id": session_id}
```

**补充约束**：实际实现时必须再补 3 类事件，否则前端无法闭环：

- `interrupt`：进入审批等待
- `error`：中途异常
- `final_answer`：最终答案或最终摘要

**涉及文件**：
- `agent/react_agent.py` — +`astream()` 方法
- `api/agent_routes.py` — +`/assistant/agent/stream` SSE 端点
- `service.py` — +`chat_agent_stream()` 方法

**面试价值**：
> 能说出 `token / tool_start / tool_end / interrupt / error / done` 这套事件分层，说明你不是只做了一个“会吐字”的 demo。

---

### P1: Agent Evaluation Framework（预计 1.5 天）

**问题**：没有量化指标知道 Agent 表现。

**方案**：建一个最小 eval 套件，但必须拆成“稳定回归”和“真实模型观测”两层，不能把“不 mock LLM”直接当默认测试原则。

**评估维度**：

| 维度 | 指标 | 测试方式 |
|------|------|---------|
| 工具选择准确率 | `correct / total` | 对比实际 tool trace 与期望工具 |
| 中断触发率 | `interrupted / total_write_calls` | 检查写工具是否被正确拦截 |
| 答案含参考率 | `has_ref / total` | 检查答案是否引用工具结果 |
| 端到端成功率 | `ok / total` | `result.get("ok") == True` |

**拆层建议**：

- `golden regression`：mock LLM 或固定 tool call，保证 CI 稳定可重复
- `live eval`：真实模型单独跑，输出 summary，不作为硬门禁

```python
# tests/test_agent_eval.py
AGENT_EVAL_CASES = [
    {
        "question": "DEVICE_001 当前状态正常吗？",
        "expected_tools": [{"name": "get_device_status"}],
    },
    {
        "question": "给设备 1 做 stm32f407-smarthome-1.3.9-a1 的 OTA 升级",
        "expected_tools": [
            {"name": "ota_risk_check"},
            {"name": "ota_create_request"},
        ],
        "must_not_call": ["approve_ota_request"],
    },
]
```

**涉及文件**：
- `tests/test_agent_eval.py` — [新增] 评估测试集
- `tests/conftest.py` — [新增] 共享 fixture（如需要）

**面试价值**：
> 能区分“稳定回归”和“在线真实评估”，说明你知道 Agent 评估不能只靠一次 live demo。

---

### P2: Context Management（预计 1 天）

**问题**：`MemorySaver` 会随对话增长，长期需要上下文控制。

**风险说明**：这项不能直接无脑接 `SummarizationMiddleware`。当前 agent 里已经有：

- `HumanInTheLoopMiddleware`
- `_pending_interrupt_actions`
- `_pending_manual_actions`
- 基于最终消息的 deterministic resume answer / tool log 持久化

如果摘要在 interrupt 前后改写消息，最容易破坏审批恢复与日志一致性。

**更稳妥的 V2b 做法**：

1. 先加 `recursion_limit`
2. 再做最近 N 轮窗口裁剪或 message budget 控制
3. 最后才评估是否引入 `SummarizationMiddleware`

```python
config = {
    "configurable": {"thread_id": session_id},
    "recursion_limit": self.settings.agent_recursion_limit,
}
```

**涉及文件**：
- `agent/react_agent.py` — `invoke()/resume()` 增加 `recursion_limit`
- `config.py` — 如需继续细化，可补 message budget 配置项

**面试价值**：
> 重点不是“我用了摘要 middleware”，而是“我知道摘要会影响 interrupt 语义，所以先做低风险控制”。

---

### P2: System Prompt 增强（预计 0.5 天）

**问题**：当前 prompt 以原则描述为主，边界场景下顺序可能漂移。

**方案**：在 `system_prompt.txt` 追加 few-shot 示例，但只补真实高频路径，不要为了凑示例把 prompt 堆太长。

```diff
## OTA 特殊要求
- 升级前必须先做 ota_risk_check。
- 风险检查有阻塞项时，不能继续创建审批。
- approve_ota_request 会真正触发固件下发，调用前必须确认审批 ID 和用户意图。

## 示例
用户: "设备 1 怎么了？"
正确流程:
  1. get_device_status(device_ref="1")
  2. get_sensor_history(device_ref="1")
  3. get_system_events(limit=5)
  4. diagnose_fault(device_id=1)

用户: "给设备 1 升级固件 v2.0"
正确流程:
  1. ota_risk_check(device_id=1, firmware_id="v2.0")
  2. 如果有阻塞项，告知用户
  3. 如果没有阻塞项，创建审批请求
```

**涉及文件**：
- `prompts/system_prompt.txt` — +few-shot 示例

---

### P3: Multi-Agent 初探（预计 2 天，可选，已完成第一阶段）

**问题**：诊断场景存在“多路信息收集”的天然并行性，但当前收益还不足以支撑它进入主线优先级。

**现状**：第一阶段已经按这个边界落地，且仍保持为只读探索，不进入审批/写操作主链。

**已落地内容**：

- 新增 `agent/diagnosis_team.py`
- `DiagnosisTeam` 内部拆为 `StatusAgent / SensorAgent / EventAgent / OtaAgent`
- 四路只读采集并行执行，输出 `team_trace`
- 已接入 `workflow_diagnose_fault()`
- 已接入 `workflow_generate_report(report_type=\"fault_ticket\")`
- 结果中新增 `collection_mode=\"multi_agent_read_team\"`

**后续可选扩展**：

- 在前端展示 `team_trace`
- 抽象 `ReportTeam` / `KnowledgeTeam`
- 评估是否需要把 planner 层显式 team-aware，但不建议过早把审批写链路做成多 agent

**建议补入计划的后续升级方向**：

1. `ReportTeam / KnowledgeTeam` 抽象化

目标：
- 把当前 `DiagnosisTeam` 的“多路只读并行采集 + 汇总输出”模式抽成可复用 team 框架
- 让故障报告、知识检索、运行态汇总不再各自手写采集逻辑

建议范围：
- `ReportTeam`：面向 `fault_ticket` / `test_report` 的素材采集与上下文拼装
- `KnowledgeTeam`：面向文档检索、SQL 摘要、运行态事实汇总的只读协作

收益：
- 降低 workflow 内部重复采集代码
- 统一 `team_trace / collection_mode / output_key` 结构
- 为后续更多只读 agent 协作场景提供模板

边界：
- 仍限制为只读 team
- 不进入审批、OTA 下发、工单写操作链路

2. Planner / Executor 显式 Team-Aware

目标：
- 不再只把 team 封装在 workflow 内部
- 让 `planner.py` / `executor.py` 能显式表达“本步骤由哪个 team 完成”

建议范围：
- planner step 增加 `executor_type` 或 `team_name`
- executor 根据 step 元数据分发到单工具、workflow、team 三类执行器
- session/audit 中保留 team 级执行轨迹，而不只是最终 workflow 输出

收益：
- 计划层可以表达更清晰的执行语义
- Agent timeline 能区分单点工具调用和 team 协作步骤
- 后续若扩展多 team 编排，不需要继续把复杂性都塞进 `service.py`

边界：
- 第一阶段只做只读场景 team-aware
- 不建议当前就把审批中断、写操作恢复、多 agent 编排混到一起

**方案**：继续保持探索项，不放进写操作主线。先限制在诊断类只读场景，不碰审批/写操作。

```python
# agent/diagnosis_team.py (可选新增)
"""
主 Agent 收到诊断请求后，派发三个只读子任务并行:
  - SensorAgent: 查传感器历史
  - EventAgent:  查系统事件
  - OtaAgent:    查 OTA 状态
最后汇总结论。
"""
```

**约束**：

- 不进入 OTA 写操作链路
- 不和审批中断逻辑混用
- 先做实验分支，不直接替换主 Agent

---

### 实施顺序建议

```
周次       内容                         面试价值    累计评分
─────────────────────────────────────────────────────────
第1天      P0 HITL 决策补全               ⭐⭐⭐    7.8
第2天      P1 Streaming SSE              ⭐⭐⭐    8.1
第3-4天    P1 Eval Framework             ⭐⭐      8.3
第5天      P2 Context + Prompt           ⭐⭐      8.5
第6-7天    P3 Multi-Agent (可选冲刺)      ⭐⭐⭐    9.0
```

### 改动汇总

```
V2b 新增/修改:
  agent/react_agent.py            +50~90 行 (resume 扩展, astream, recursion_limit)
  api/agent_routes.py             +15~25 行 (/resume 扩展, /stream SSE)
  service.py                      +15~30 行 (chat_agent_resume/chat_agent_stream 扩展)
  models.py                       +8~12 行 (AgentResumeRequest)
  tests/test_agent_eval.py        +80~140 行 (golden regression + live eval)
  prompts/system_prompt.txt       +10~20 行 (few-shot 示例)

总计: ~220~320 行新增/修改
```
