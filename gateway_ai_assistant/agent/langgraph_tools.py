from __future__ import annotations

from pydantic import BaseModel, Field
from langchain_core.tools import tool


class DeviceStatusInput(BaseModel):
    device_ref: str = Field(description="设备标识: 数字如'1'，或名称如'sensor-01'或'DEVICE_001'")


class SensorHistoryInput(BaseModel):
    device_ref: str = Field(description="设备标识")
    limit: int = Field(default=5, description="返回条数 1-20")


class SystemEventsInput(BaseModel):
    limit: int = Field(default=10, description="返回条数 1-100")


class OtaRiskCheckInput(BaseModel):
    device_id: int = Field(description="设备数字 ID")
    firmware_id: str = Field(description="固件 ID")


class OtaRequestCreateInput(BaseModel):
    device_id: int = Field(description="设备数字 ID")
    firmware_id: str = Field(description="固件 ID")
    transport: str = Field(default="serial")
    target: str = Field(default="127.0.0.1:19090")


class DiagnoseFaultInput(BaseModel):
    device_id: int = Field(description="设备数字 ID")
    limit: int = Field(default=10, description="查询最近 N 条记录")


class RAGQueryInput(BaseModel):
    question: str = Field(description="需要从项目文档中检索的具体问题")


class SQLQueryInput(BaseModel):
    question: str = Field(description="结构化查询问题，例如统计 OTA 失败数量")


class ApproveOtaInput(BaseModel):
    approval_id: str = Field(description="审批 ID")
    approved_by: str = Field(default="operator", description="批准人")


class PauseBatchInput(BaseModel):
    batch_run_id: str = Field(description="批量运行 ID")
    operator: str = Field(default="operator")


class RetryFailedInput(BaseModel):
    approval_id: str = Field(description="审批 ID")
    operator: str = Field(default="operator")


def create_readonly_tools(service):
    from ..rag.retriever import retrieve
    from ..router import classify_question

    @tool(args_schema=DeviceStatusInput)
    def get_device_status(device_ref: str) -> dict:
        """查询设备当前在线状态和最近 OTA 状态。"""
        return service.runtime_gateway.get_device_status(device_ref)

    @tool(args_schema=SensorHistoryInput)
    def get_sensor_history(device_ref: str, limit: int = 5) -> dict:
        """查询设备最近传感器上报记录。"""
        return service.runtime_gateway.get_sensor_history(device_ref, limit)

    @tool(args_schema=SystemEventsInput)
    def get_system_events(limit: int = 10) -> dict:
        """查询网关最近系统事件。"""
        return service.runtime_gateway.get_system_events(limit)

    @tool
    def get_database_summary() -> dict:
        """获取网关数据库概况。"""
        return service.runtime_gateway.get_database_summary()

    @tool(args_schema=RAGQueryInput)
    def search_documents(question: str) -> dict:
        """从项目文档中检索相关知识。"""
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
        """执行结构化数据查询。"""
        decision = classify_question(question)
        if decision.route_type == "SQL":
            return service.route_sql(question).to_dict()
        if decision.route_type == "Hybrid":
            result = service.route_hybrid_sql(question)
            return result if isinstance(result, dict) else {"ok": True, "result": result}
        return {"ok": False, "message": "该问题不适用结构化查询，请用 search_documents。"}

    @tool(args_schema=OtaRiskCheckInput)
    def ota_risk_check(device_id: int, firmware_id: str) -> dict:
        """OTA 升级前风险检查。"""
        return service.workflow_ota_risk_check(device_id, firmware_id)

    @tool(args_schema=DiagnoseFaultInput)
    def diagnose_fault(device_id: int, limit: int = 10) -> dict:
        """基于规则诊断设备故障。"""
        return service.workflow_diagnose_fault(device_id, limit)

    @tool
    def list_approvals() -> dict:
        """列出审批请求。"""
        return service.list_approval_requests()

    @tool
    def list_reports() -> dict:
        """列出已生成报告。"""
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


def create_write_tools(service, approval_mode: str = "execute"):
    def _manual_approval_payload(action_name: str, args: dict) -> dict:
        return {
            "ok": True,
            "requires_approval": True,
            "action_name": action_name,
            "action_args": args,
            "message": "High-risk write action requires explicit approval before execution.",
        }

    @tool(args_schema=OtaRequestCreateInput)
    def ota_create_request(
        device_id: int,
        firmware_id: str,
        transport: str = "serial",
        target: str = "127.0.0.1:19090",
    ) -> dict:
        """创建 OTA 审批请求。"""
        if approval_mode == "manual":
            return _manual_approval_payload(
                "ota_create_request",
                {
                    "device_id": device_id,
                    "firmware_id": firmware_id,
                    "transport": transport,
                    "target": target,
                },
            )
        return service.workflow_ota_request_create(device_id, firmware_id, transport, target)

    @tool(args_schema=ApproveOtaInput)
    def approve_ota_request(approval_id: str, approved_by: str = "operator") -> dict:
        """批准 OTA 审批请求。"""
        if approval_mode == "manual":
            return _manual_approval_payload(
                "approve_ota_request",
                {"approval_id": approval_id, "approved_by": approved_by},
            )
        return service.workflow_ota_request_confirm(approval_id, approved_by)

    @tool(args_schema=PauseBatchInput)
    def pause_batch(batch_run_id: str, operator: str = "operator") -> dict:
        """暂停进行中的 OTA 灰度批次。"""
        if approval_mode == "manual":
            return _manual_approval_payload(
                "pause_batch",
                {"batch_run_id": batch_run_id, "operator": operator},
            )
        return service.pause_batch_run(batch_run_id, operator)

    @tool(args_schema=RetryFailedInput)
    def retry_failed_devices(approval_id: str, operator: str = "operator") -> dict:
        """重试批量 OTA 中失败的设备。"""
        if approval_mode == "manual":
            return _manual_approval_payload(
                "retry_failed_devices",
                {"approval_id": approval_id, "operator": operator},
            )
        return service.retry_batch_failed_devices(approval_id, operator)

    return [
        ota_create_request,
        approve_ota_request,
        pause_batch,
        retry_failed_devices,
    ]
