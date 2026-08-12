from __future__ import annotations

import asyncio
import json
from pathlib import Path

import streamlit as st

from gateway_ai_assistant.evals import DEFAULT_AGENT_EVAL_CASES
from gateway_ai_assistant.rag.embedding import embedding_backend_name
from gateway_ai_assistant.service import AssistantService, DEMO_QUESTIONS


service = AssistantService()
health = service.health()

st.set_page_config(page_title="边缘网关智能运维与数据分析助手", layout="wide")
st.title("边缘网关智能运维与数据分析助手")
st.caption("Runtime + RAG + SQL + Hybrid + Workflow + Governance on top of serial-gateway")


STATE_DEFAULTS = {
    "question": "",
    "last_result": None,
    "ota_risk_result": None,
    "ota_plan_result": None,
    "ota_request_result": None,
    "ota_confirm_result": None,
    "ota_batch_plan_result": None,
    "ota_batch_request_result": None,
    "ota_batch_retry_result": None,
    "batch_run_detail_result": None,
    "approval_id": "",
    "batch_approval_id": "",
    "approval_detail_result": None,
    "report_result": None,
    "diagnosis_result": None,
    "ticket_create_result": None,
    "ticket_assign_result": None,
    "ticket_close_result": None,
    "ticket_reopen_result": None,
    "agent_plan_result": None,
    "agent_execute_result": None,
    "agent_session_result": None,
    "agent_stream_result": None,
    "agent_resume_result": None,
    "agent_live_eval_result": None,
    "agent_eval_suite_result": None,
    "agent_replay_result": None,
    "agent_memory_result": None,
    "realtime_event_result": None,
    "selected_approval_id": "",
    "selected_report_id": "",
    "selected_ticket_id": "",
    "selected_batch_run_id": "",
    "selected_eval_run_id": "",
    "replay_session_id": "",
    "selected_memory_id": "",
    "selected_candidate_id": "",
    "selected_alert_id": "",
    "selected_decision_id": "",
}
for key, value in STATE_DEFAULTS.items():
    if key not in st.session_state:
        st.session_state[key] = value


def run_question(question: str) -> None:
    st.session_state.question = question
    st.session_state.last_result = service.chat(question)


def available_firmware_ids() -> list[str]:
    firmware_root = service.ota_tools.firmware_root
    if not firmware_root.exists():
        return []
    items = []
    for manifest_path in sorted(firmware_root.glob("*/manifest.json")):
        items.append(manifest_path.parent.name)
    return items


def parse_device_ids(raw: str) -> list[int]:
    result: list[int] = []
    for item in raw.replace("\n", ",").split(","):
        value = item.strip()
        if value.isdigit():
            result.append(int(value))
    return result


def render_tool_trace(trace: list[dict]) -> None:
    if not trace:
        st.info("当前没有工具调用轨迹。")
        return
    for idx, item in enumerate(trace, start=1):
        title = f"{idx}. {item.get('tool_name', 'unknown')} | {item.get('tool_status', 'unknown')}"
        with st.expander(title):
            st.json(item, expanded=False)


def render_team_trace(team_trace: list[dict]) -> None:
    if not team_trace:
        st.info("当前没有 Multi-Agent 采集轨迹。")
        return
    for idx, item in enumerate(team_trace, start=1):
        title = f"{idx}. {item.get('agent_name', 'unknown')} -> {item.get('tool_name', 'unknown')}"
        with st.expander(title):
            st.write(
                {
                    "output_key": item.get("output_key", ""),
                    "status": item.get("status", ""),
                    "mode": item.get("mode", ""),
                }
            )
            st.json(item, expanded=False)


def render_agent_timeline(step_results: list[dict]) -> None:
    if not step_results:
        st.info("当前没有 Agent step timeline。")
        return
    for item in step_results:
        title = f"{item.get('step_id', '?')}. {item.get('action', 'unknown')} | {item.get('status', 'unknown')}"
        with st.expander(title, expanded=item.get("status") == "error"):
            st.write(
                {
                    "reason": item.get("reason", ""),
                    "description": item.get("description", ""),
                    "mode": item.get("mode", ""),
                    "started_at": item.get("started_at", ""),
                    "finished_at": item.get("finished_at", ""),
                    "failure_reason": item.get("failure_reason", ""),
                }
            )
            st.json(item.get("output", {}), expanded=False)


def render_agent_context(agent_context: dict | None) -> None:
    if not agent_context:
        st.info("当前没有 Agent 上下文观测数据。")
        return
    metrics = st.columns(4)
    metrics[0].metric("消息数", agent_context.get("message_count", 0))
    metrics[1].metric("粗略字符量", agent_context.get("approx_char_count", 0))
    metrics[2].metric("待审批动作", agent_context.get("pending_action_count", 0))
    metrics[3].metric("上下文预警", "是" if agent_context.get("context_warning") else "否")
    st.write(
        {
            "message_types": agent_context.get("message_types", {}),
            "pending_interrupt": agent_context.get("pending_interrupt", False),
            "warning_thresholds": agent_context.get("warning_thresholds", {}),
        }
    )


def render_memory_snapshot(memory_payload: dict | None) -> None:
    if not memory_payload or not memory_payload.get("ok"):
        st.info("当前没有 Memory 数据。")
        return
    session_memory = memory_payload.get("session_memory")
    related_session_memories = memory_payload.get("related_session_memories", [])
    long_term_memories = memory_payload.get("long_term_memories", [])
    memory_audits = memory_payload.get("memory_audits", [])
    mem_col1, mem_col2, mem_col3, mem_col4 = st.columns([1, 1, 1, 1])
    with mem_col1:
        st.subheader("Session Memory")
        if session_memory:
            st.write(
                {
                    "session_id": session_memory.get("session_id", ""),
                    "updated_at": session_memory.get("updated_at", ""),
                    "last_question": session_memory.get("last_question", ""),
                }
            )
            st.write(session_memory.get("summary", ""))
            st.json(session_memory.get("key_facts", {}), expanded=False)
        else:
            st.info("当前 session 还没有摘要记忆。")
    with mem_col2:
        st.subheader("Related Sessions")
        if related_session_memories:
            st.dataframe(related_session_memories, use_container_width=True)
        else:
            st.info("当前没有命中的相关历史 session。")
    with mem_col3:
        st.subheader("Long-Term Memories")
        if long_term_memories:
            st.dataframe(long_term_memories, use_container_width=True)
        else:
            st.info("当前没有长期偏好记忆。")
    with mem_col4:
        st.subheader("Memory Audits")
        if memory_audits:
            st.dataframe(memory_audits, use_container_width=True)
        else:
            st.info("当前没有 memory audit 记录。")


def render_replay_timeline(replay_payload: dict | None) -> None:
    if not replay_payload or not replay_payload.get("ok"):
        st.info("当前没有 Replay 数据。")
        return
    timeline = replay_payload.get("timeline", [])
    if not timeline:
        st.info("当前 session timeline 为空。")
        return
    for idx, item in enumerate(timeline, start=1):
        with st.expander(f"{idx}. {item.get('kind', 'unknown')} | {item.get('name', 'unknown')} | {item.get('status', '')}"):
            st.write({"created_at": item.get("created_at", "")})
            st.json(item.get("payload", {}), expanded=False)


def render_eval_run_detail(payload: dict | None) -> None:
    if not payload:
        st.info("当前没有 Eval 结果。")
        return
    run = payload.get("run", {})
    summary = payload.get("summary", {}) or run.get("summary_payload", {})
    comparison = payload.get("comparison", {}) or summary.get("comparison", {})
    metrics = st.columns(6)
    metrics[0].metric("总用例", summary.get("total", run.get("case_count", 0)))
    metrics[1].metric("通过", summary.get("passed", 0))
    metrics[2].metric("失败", summary.get("failed", 0))
    metrics[3].metric("通过率", f"{summary.get('pass_rate', 0.0):.0%}")
    metrics[4].metric("Regression", len(comparison.get("regressions", [])))
    metrics[5].metric("Improvement", len(comparison.get("improvements", [])))
    if run:
        st.caption(
            f"run_id={run.get('run_id', '')} | baseline={run.get('baseline_run_id', '') or 'none'} | created_at={run.get('created_at', '')}"
        )
    regressions = comparison.get("regressions", [])
    if regressions:
        st.warning("检测到回归用例")
        st.dataframe(regressions, use_container_width=True)
    improvements = comparison.get("improvements", [])
    if improvements:
        st.success("检测到改进用例")
        st.dataframe(improvements, use_container_width=True)
    cases = payload.get("cases", [])
    if cases:
        st.dataframe(
            [
                {
                    "case_index": item.get("case_index", item.get("session_id", "")),
                    "question": item.get("question", ""),
                    "ok": item.get("ok", False),
                    "session_id": item.get("session_id", ""),
                    "actual_tools": ",".join(item.get("actual_tools", [])),
                    "missing_tools": ",".join(item.get("missing_tools", [])),
                    "failure_reasons": ",".join(item.get("failure_reasons", [])),
                }
                for item in cases
            ],
            use_container_width=True,
        )


def run_agent_stream(question: str, session_id: str) -> dict:
    rendered_text = ""
    events: list[dict] = []
    interrupt_actions: list[dict] = []
    error_message = ""

    async def _consume() -> None:
        nonlocal rendered_text, error_message, interrupt_actions
        async for event in service.chat_agent_stream(question, session_id):
            events.append(event)
            event_type = event.get("type", "")
            if event_type == "token":
                rendered_text += str(event.get("content", ""))
            elif event_type == "final_answer":
                rendered_text = str(event.get("content", "")).strip() or rendered_text
            elif event_type == "interrupt":
                interrupt_actions = event.get("actions", []) if isinstance(event.get("actions", []), list) else []
            elif event_type == "error":
                error_message = str(event.get("error", ""))

    asyncio.run(_consume())
    return {
        "session_id": session_id,
        "events": events,
        "rendered_text": rendered_text,
        "interrupt_actions": interrupt_actions,
        "error": error_message,
    }


def render_stream_events(events: list[dict]) -> None:
    if not events:
        st.info("当前没有流式事件。")
        return
    for idx, event in enumerate(events, start=1):
        with st.expander(f"{idx}. {event.get('type', 'unknown')}"):
            st.json(event, expanded=False)


def render_approval_history(detail_payload: dict | None) -> None:
    if not detail_payload:
        return
    approval = detail_payload.get("approval", detail_payload if detail_payload.get("approval_id") else {})
    if not isinstance(approval, dict):
        return
    st.write(
        {
            "approval_id": approval.get("approval_id", ""),
            "status": approval.get("status", ""),
            "approved_by": approval.get("approved_by", ""),
            "approved_at": approval.get("approved_at", ""),
            "decision_note": approval.get("decision_note", ""),
        }
    )
    result_payload = approval.get("result_payload", {})
    if isinstance(result_payload, dict) and result_payload:
        retry_history = result_payload.get("retry_history", [])
        if retry_history:
            st.subheader("重试历史")
            st.dataframe(retry_history, use_container_width=True)
        st.subheader("结果载荷")
        st.json(result_payload, expanded=False)


def approval_status_choices() -> list[str]:
    return ["", "pending", "approved", "execute_failed", "partial_failed", "rejected", "cancelled"]


def render_batch_summary(batch_result: dict | None) -> None:
    if not batch_result:
        return
    st.write(
        {
            "batch_count": batch_result.get("batch_count"),
            "ready_device_ids": batch_result.get("ready_device_ids", []),
            "blocked_device_ids": batch_result.get("blocked_device_ids", []),
            "risk_levels": batch_result.get("risk_levels", {}),
        }
    )


def render_ticket_table(items: list[dict]) -> None:
    if items:
        st.dataframe(items, use_container_width=True)
    else:
        st.info("当前没有工单记录。")


def build_overview_snapshot() -> dict:
    approvals = service.list_approval_requests(limit=100)
    tickets = service.list_tickets(limit=100)
    batch_runs = service.list_batch_runs(limit=100)
    reports = service.list_reports(limit=100)
    approval_items = approvals.get("items", [])
    ticket_items = tickets.get("items", [])
    batch_items = batch_runs.get("items", [])
    return {
        "approval_total": approvals.get("count", 0),
        "approval_pending": len([item for item in approval_items if item.get("status") == "pending"]),
        "batch_run_total": batch_runs.get("count", 0),
        "batch_run_active": len([item for item in batch_items if item.get("status") in {"created", "running", "waiting_next_batch", "partial_failed"}]),
        "ticket_open": len([item for item in ticket_items if item.get("status") == "open"]),
        "report_total": reports.get("count", 0),
    }


firmware_choices = available_firmware_ids()
default_firmware = firmware_choices[-1] if firmware_choices else "stm32f407-smarthome-1.3.9-a1"


with st.sidebar:
    st.subheader("运行状态")
    st.write(
        {
            "rag_backend": embedding_backend_name(),
            "history_db": health["gateway_history_db_exists"],
            "ota_db": health["gateway_ota_db_exists"],
            "assistant_db": Path(health["assistant_db"]).name,
        }
    )
    st.subheader("你可以这样问")
    st.caption("设备状态、历史数据、系统事件、OTA 状态、协议文档、故障分析、审批、工单都支持。")
    st.subheader("Demo Questions")
    for item in DEMO_QUESTIONS:
        if st.button(item, use_container_width=True):
            run_question(item)
    if st.button("重建索引", use_container_width=True):
        rebuild = service.rebuild_index()
        st.success(f"索引已重建，chunks={rebuild['total_chunks']}")


question = st.text_input(
    "输入问题",
    value=st.session_state.question,
    placeholder="例如：你会干什么？ / DEVICE_001 当前状态正常吗？ / 对 device_ids=1,2,3 做批量 OTA 风险检查并创建审批 firmware_id=fw-1",
)
if st.button("发送", type="primary", use_container_width=True):
    run_question(question)

overview_tab, chat_tab, workflow_tab, agent_tab, eval_tab, governance_tab = st.tabs(
    ["总览", "问答演示", "Workflow 演示", "Agent 演示", "Eval / Replay / Memory", "治理中心"]
)

with overview_tab:
    st.subheader("系统总览")
    snapshot = build_overview_snapshot()
    m1, m2, m3, m4, m5, m6 = st.columns(6)
    m1.metric("待审批", snapshot["approval_pending"])
    m2.metric("审批总数", snapshot["approval_total"])
    m3.metric("批量执行中", snapshot["batch_run_active"])
    m4.metric("批量执行总数", snapshot["batch_run_total"])
    m5.metric("开放工单", snapshot["ticket_open"])
    m6.metric("报告总数", snapshot["report_total"])

    overview_col1, overview_col2 = st.columns([1, 1])
    with overview_col1:
        st.subheader("最近批量执行")
        latest_runs = service.list_batch_runs(limit=5)
        if latest_runs.get("items"):
            st.dataframe(latest_runs["items"], use_container_width=True)
        else:
            st.info("当前没有批量执行记录。")
    with overview_col2:
        st.subheader("最近审批")
        latest_approvals = service.list_approval_requests(limit=5)
        if latest_approvals.get("items"):
            st.dataframe(latest_approvals["items"], use_container_width=True)
        else:
            st.info("当前没有审批记录。")

with chat_tab:
    result = st.session_state.last_result
    if result:
        k1, k2, k3 = st.columns(3)
        with k1:
            st.metric("Route", result["route_type"])
        with k2:
            st.metric("LLM Mode", result["llm_mode"])
        with k3:
            st.metric("RAG Backend", embedding_backend_name())

        col1, col2 = st.columns([2, 1])
        with col1:
            st.subheader("回答")
            st.write(result["answer"])
        with col2:
            st.subheader("执行信息")
            st.write(
                {
                    "route_type": result["route_type"],
                    "llm_mode": result["llm_mode"],
                    "response_source": result.get("response_source", ""),
                    "retrieved_chunks": len(result.get("retrieved_chunks", [])),
                }
            )
            st.caption(result.get("route_reason", ""))

        if result.get("analysis_facts"):
            st.subheader("事实摘要")
            for item in result["analysis_facts"]:
                st.markdown(f"- {item}")

        st.subheader("工具调用轨迹")
        render_tool_trace(result.get("tool_trace", []))

        if result.get("sql_result"):
            sql_result = result["sql_result"]
            st.subheader("SQL 结果")
            if sql_result.get("note"):
                st.warning(sql_result["note"])
            st.code(sql_result.get("sql", ""), language="sql")
            columns = sql_result.get("columns", [])
            rows = sql_result.get("rows", [])
            table_data = [dict(zip(columns, row)) for row in rows] if columns else rows
            if table_data:
                st.dataframe(table_data, use_container_width=True)
            else:
                st.info("当前没有查询到结果。")

        if result.get("runtime_result"):
            st.subheader("Runtime 结果")
            st.json(result["runtime_result"], expanded=False)

        if result.get("retrieved_chunks"):
            st.subheader("RAG 引用片段")
            for item in result["retrieved_chunks"]:
                with st.expander(f"{item['source_path']} | score={item['score']}"):
                    st.text(item["chunk_text"])


with workflow_tab:
    single_tab, batch_tab, report_tab = st.tabs(["单设备 OTA", "批量 OTA", "诊断与报告"])

    with single_tab:
        st.subheader("单设备 OTA 风险检查与审批")
        wf_col1, wf_col2 = st.columns([1, 1])
        with wf_col1:
            with st.form("ota_workflow_form"):
                device_id = st.number_input("设备 ID", min_value=1, step=1, value=1)
                if firmware_choices:
                    firmware_id = st.selectbox("固件 ID", firmware_choices, index=len(firmware_choices) - 1)
                else:
                    firmware_id = st.text_input("固件 ID", value=default_firmware)
                transport = st.selectbox("传输方式", ["serial", "tcp_binary"], index=0)
                target = st.text_input("目标地址", value="127.0.0.1:19090")
                col_risk, col_plan, col_request = st.columns(3)
                risk_submit = col_risk.form_submit_button("风险检查", use_container_width=True)
                plan_submit = col_plan.form_submit_button("生成计划", use_container_width=True)
                request_submit = col_request.form_submit_button("创建审批请求", use_container_width=True)

            if risk_submit:
                st.session_state.ota_risk_result = service.workflow_ota_risk_check(int(device_id), str(firmware_id))
            if plan_submit:
                st.session_state.ota_plan_result = service.workflow_ota_plan(int(device_id), str(firmware_id))
            if request_submit:
                st.session_state.ota_request_result = service.workflow_ota_request_create(
                    int(device_id),
                    str(firmware_id),
                    str(transport),
                    str(target),
                )
                approval_id = st.session_state.ota_request_result.get("approval_id", "")
                if approval_id:
                    st.session_state.approval_id = approval_id
                    st.session_state.selected_approval_id = approval_id

        with wf_col2:
            with st.form("ota_confirm_form"):
                approval_id = st.text_input("审批 ID", value=st.session_state.approval_id)
                approved_by = st.text_input("确认人", value="operator")
                approval_note = st.text_area("审批备注", value="人工确认后执行单设备 OTA。", height=80)
                confirm_submit = st.form_submit_button("确认并创建 OTA 任务", use_container_width=True)
            if confirm_submit:
                st.session_state.ota_confirm_result = service.approve_request(
                    approval_id.strip(),
                    approved_by.strip() or "operator",
                    approval_note.strip(),
                )

        result_col1, result_col2 = st.columns([1, 1])
        with result_col1:
            if st.session_state.ota_risk_result:
                st.subheader("风险检查结果")
                st.json(st.session_state.ota_risk_result, expanded=False)
            if st.session_state.ota_plan_result:
                st.subheader("升级计划")
                st.json(st.session_state.ota_plan_result, expanded=False)
        with result_col2:
            if st.session_state.ota_request_result:
                st.subheader("审批请求")
                st.json(st.session_state.ota_request_result, expanded=False)
            if st.session_state.ota_confirm_result:
                st.subheader("审批确认结果")
                st.json(st.session_state.ota_confirm_result, expanded=False)

    with batch_tab:
        st.subheader("批量 OTA / 灰度发布演示")
        batch_col1, batch_col2 = st.columns([1, 1])
        with batch_col1:
            with st.form("ota_batch_workflow_form"):
                batch_device_ids_raw = st.text_area("设备 ID 列表", value="1,2,3", height=100)
                if firmware_choices:
                    batch_firmware_id = st.selectbox("批量固件 ID", firmware_choices, index=len(firmware_choices) - 1)
                else:
                    batch_firmware_id = st.text_input("批量固件 ID", value=default_firmware)
                batch_transport = st.selectbox("批量传输方式", ["serial", "tcp_binary"], index=0)
                batch_target = st.text_input("批量目标地址", value="127.0.0.1:19090")
                batch_size = st.slider("灰度批大小", min_value=1, max_value=10, value=2)
                col_plan, col_request = st.columns(2)
                batch_plan_submit = col_plan.form_submit_button("生成批量计划", use_container_width=True)
                batch_request_submit = col_request.form_submit_button("创建批量审批", use_container_width=True)

            batch_device_ids = parse_device_ids(batch_device_ids_raw)
            if batch_plan_submit and batch_device_ids:
                st.session_state.ota_batch_plan_result = service.workflow_ota_batch_plan(
                    batch_device_ids,
                    str(batch_firmware_id),
                    str(batch_transport),
                    str(batch_target),
                    int(batch_size),
                )
            if batch_request_submit and batch_device_ids:
                st.session_state.ota_batch_request_result = service.workflow_ota_batch_request_create(
                    batch_device_ids,
                    str(batch_firmware_id),
                    str(batch_transport),
                    str(batch_target),
                    int(batch_size),
                )
                approval_id = st.session_state.ota_batch_request_result.get("approval_id", "")
                if approval_id:
                    st.session_state.batch_approval_id = approval_id
                    st.session_state.selected_approval_id = approval_id
            if (batch_plan_submit or batch_request_submit) and not batch_device_ids:
                st.warning("请输入至少一个合法的设备 ID。")

        with batch_col2:
            with st.form("ota_batch_confirm_form"):
                batch_approval_id = st.text_input("批量审批 ID", value=st.session_state.batch_approval_id)
                batch_operator = st.text_input("批量确认人", value="operator")
                batch_note = st.text_area("批量审批备注", value="人工确认后按 ready device 集合下发 OTA。", height=80)
                col_confirm, col_retry = st.columns(2)
                batch_confirm_submit = col_confirm.form_submit_button("批准并执行批量下发", use_container_width=True)
                batch_retry_submit = col_retry.form_submit_button("重试失败设备", use_container_width=True)
            if batch_confirm_submit:
                st.session_state.ota_confirm_result = service.approve_request(
                    batch_approval_id.strip(),
                    batch_operator.strip() or "operator",
                    batch_note.strip(),
                )
                if st.session_state.ota_confirm_result.get("batch_run_id"):
                    st.session_state.selected_batch_run_id = st.session_state.ota_confirm_result["batch_run_id"]
            if batch_retry_submit:
                st.session_state.ota_batch_retry_result = service.retry_batch_failed_devices(
                    batch_approval_id.strip(),
                    batch_operator.strip() or "operator",
                    batch_note.strip(),
                )

        batch_result_col1, batch_result_col2 = st.columns([1, 1])
        with batch_result_col1:
            if st.session_state.ota_batch_plan_result:
                st.subheader("批量计划摘要")
                render_batch_summary(st.session_state.ota_batch_plan_result)
                st.json(st.session_state.ota_batch_plan_result, expanded=False)
        with batch_result_col2:
            if st.session_state.ota_batch_request_result:
                st.subheader("批量审批请求")
                st.json(st.session_state.ota_batch_request_result, expanded=False)
            if st.session_state.ota_batch_retry_result:
                st.subheader("批量失败重试结果")
                st.json(st.session_state.ota_batch_retry_result, expanded=False)
            elif st.session_state.ota_confirm_result and st.session_state.ota_confirm_result.get("per_device_results"):
                st.subheader("批量执行结果")
                st.json(st.session_state.ota_confirm_result, expanded=False)

    with report_tab:
        st.subheader("规则诊断与报告/工单演示")
        report_col1, report_col2 = st.columns([1, 1])
        with report_col1:
            with st.form("report_workflow_form"):
                report_type = st.selectbox("报告类型", ["fault_ticket", "test_report"], index=0)
                report_device_id = st.number_input("报告设备 ID", min_value=1, step=1, value=1)
                report_task_id = st.text_input("关联 OTA Task ID", value="")
                report_limit = st.slider("采样条数", min_value=3, max_value=10, value=5)
                report_submit = st.form_submit_button("生成报告", use_container_width=True)
            if report_submit:
                st.session_state.report_result = service.workflow_generate_report(
                    report_type=report_type,
                    device_id=int(report_device_id),
                    task_id=report_task_id.strip(),
                    limit=int(report_limit),
                )
                if st.session_state.report_result.get("report_id"):
                    st.session_state.selected_report_id = st.session_state.report_result["report_id"]

            with st.form("diagnosis_form"):
                diagnosis_device_id = st.number_input("诊断设备 ID", min_value=1, step=1, value=1)
                diagnosis_limit = st.slider("诊断采样条数", min_value=5, max_value=20, value=10)
                diagnosis_submit = st.form_submit_button("执行规则诊断", use_container_width=True)
            if diagnosis_submit:
                st.session_state.diagnosis_result = service.workflow_diagnose_fault(
                    device_id=int(diagnosis_device_id),
                    limit=int(diagnosis_limit),
                )

        with report_col2:
            if st.session_state.report_result:
                st.write(
                    {
                        "report_id": st.session_state.report_result.get("report_id"),
                        "report_type": st.session_state.report_result.get("report_type"),
                        "title": st.session_state.report_result.get("title"),
                        "summary": st.session_state.report_result.get("summary"),
                        "collection_mode": st.session_state.report_result.get("collection_mode", ""),
                    }
                )
                if st.button("根据当前报告创建工单", use_container_width=True):
                    st.session_state.ticket_create_result = service.create_ticket(
                        report_id=st.session_state.report_result.get("report_id", ""),
                        severity="high",
                    )
            if st.session_state.diagnosis_result:
                st.write(
                    {
                        "overall_severity": st.session_state.diagnosis_result.get("overall_severity"),
                        "finding_count": st.session_state.diagnosis_result.get("finding_count"),
                        "summary": st.session_state.diagnosis_result.get("summary"),
                        "collection_mode": st.session_state.diagnosis_result.get("collection_mode", ""),
                    }
                )
            if st.session_state.ticket_create_result:
                st.success(f"工单已创建: {st.session_state.ticket_create_result.get('ticket_id', '')}")

        if st.session_state.report_result:
            st.subheader("Markdown 报告")
            st.code(st.session_state.report_result.get("markdown", ""), language="markdown")
            st.subheader("报告采集轨迹")
            render_team_trace(st.session_state.report_result.get("team_trace", []))
        if st.session_state.diagnosis_result:
            st.subheader("诊断结果")
            st.json(st.session_state.diagnosis_result, expanded=False)
            st.subheader("诊断采集轨迹")
            render_team_trace(st.session_state.diagnosis_result.get("team_trace", []))

    st.subheader("Workflow 工具轨迹")
    workflow_trace: list[dict] = []
    for item in (
        st.session_state.ota_risk_result,
        st.session_state.ota_plan_result,
        st.session_state.ota_request_result,
        st.session_state.ota_confirm_result,
        st.session_state.ota_batch_plan_result,
        st.session_state.ota_batch_request_result,
        st.session_state.ota_batch_retry_result,
        st.session_state.report_result,
        st.session_state.diagnosis_result,
    ):
        if item and isinstance(item, dict):
            workflow_trace.extend(item.get("tool_trace", []))
            if item.get("risk_result"):
                workflow_trace.extend(item["risk_result"].get("tool_trace", []))
            if item.get("plan"):
                workflow_trace.extend(item["plan"].get("tool_trace", []))
            if item.get("batch_plan"):
                workflow_trace.extend(item["batch_plan"].get("tool_trace", []))
    render_tool_trace(workflow_trace)


with agent_tab:
    st.subheader("Agent Planner / Executor 演示")
    with st.form("agent_form"):
        agent_question = st.text_area(
            "Agent 问题",
            value="请对 device_ids=1,2,3 做批量 OTA 风险检查并创建审批 firmware_id=fw-1",
            height=100,
        )
        agent_session_id = st.text_input("Session ID（可留空自动生成）", value="")
        agent_col1, agent_col2, agent_col3 = st.columns(3)
        plan_submit = agent_col1.form_submit_button("生成计划", use_container_width=True)
        execute_submit = agent_col2.form_submit_button("执行计划", use_container_width=True)
        stream_submit = agent_col3.form_submit_button("流式执行", use_container_width=True)
    if plan_submit:
        st.session_state.agent_plan_result = service.agent_plan(agent_question.strip())
    if execute_submit:
        st.session_state.agent_execute_result = service.agent_execute(agent_question.strip())
        session_id = st.session_state.agent_execute_result.get("session_id", "")
        if session_id:
            st.session_state.agent_session_result = service.get_agent_session(session_id)
    if stream_submit:
        normalized_session_id = agent_session_id.strip() or ""
        st.session_state.agent_stream_result = run_agent_stream(agent_question.strip(), normalized_session_id)
        session_id = st.session_state.agent_stream_result.get("session_id", "")
        if session_id:
            st.session_state.agent_session_result = service.get_agent_session(session_id)

    resume_col1, resume_col2 = st.columns([1, 1])
    with resume_col1:
        with st.form("agent_resume_form"):
            resume_session_id = st.text_input(
                "Resume Session ID",
                value=(st.session_state.agent_session_result or {}).get("session_id", ""),
            )
            resume_decision = st.selectbox("Resume 决策", ["approve", "reject", "edit"], index=0)
            resume_edit_args_raw = st.text_area(
                "Edit Args(JSON，仅 decision=edit 使用)",
                value='{"approved_by": "operator"}',
                height=80,
            )
            resume_submit = st.form_submit_button("继续/拒绝/修改后继续", use_container_width=True)
        if resume_submit and resume_session_id.strip():
            edit_args = {}
            if resume_decision == "edit":
                try:
                    parsed = json.loads(resume_edit_args_raw.strip() or "{}")
                    if isinstance(parsed, dict):
                        edit_args = parsed
                except json.JSONDecodeError as exc:
                    st.error(f"Edit Args JSON 无法解析: {exc}")
            st.session_state.agent_resume_result = service.chat_agent_resume(
                resume_session_id.strip(),
                decision=resume_decision,
                edit_args=edit_args,
            )
            st.session_state.agent_session_result = service.get_agent_session(resume_session_id.strip())
    with resume_col2:
        if st.session_state.agent_resume_result:
            st.subheader("Resume 结果")
            st.json(st.session_state.agent_resume_result, expanded=False)

    eval_col1, eval_col2 = st.columns([1, 1])
    with eval_col1:
        if st.button("执行 Live Eval Smoke", use_container_width=True):
            st.session_state.agent_live_eval_result = service.run_agent_live_eval()
    with eval_col2:
        if st.button("执行 Eval Suite 并落库", use_container_width=True):
            st.session_state.agent_eval_suite_result = service.run_agent_eval_suite()
        st.caption(f"默认 live eval case 数: {len(DEFAULT_AGENT_EVAL_CASES)}")

    if st.session_state.agent_plan_result:
        st.subheader("Plan")
        st.json(st.session_state.agent_plan_result, expanded=False)
    if st.session_state.agent_execute_result:
        st.subheader("Execution")
        summary = st.session_state.agent_execute_result.get("execution_summary", {})
        st.write(
            {
                "session_id": st.session_state.agent_execute_result.get("session_id", ""),
                "goal": st.session_state.agent_execute_result.get("plan", {}).get("goal", ""),
                "final_status": summary.get("final_status"),
                "completed_steps": summary.get("completed_steps"),
                "tool_call_count": summary.get("tool_call_count"),
                "failure_reason": summary.get("failure_reason", ""),
            }
        )
        render_agent_timeline(st.session_state.agent_execute_result.get("execution", {}).get("step_results", []))
    if st.session_state.agent_stream_result:
        st.subheader("Streaming")
        st.write(
            {
                "session_id": st.session_state.agent_stream_result.get("session_id", ""),
                "error": st.session_state.agent_stream_result.get("error", ""),
                "interrupt_action_count": len(st.session_state.agent_stream_result.get("interrupt_actions", [])),
            }
        )
        rendered_text = st.session_state.agent_stream_result.get("rendered_text", "")
        if rendered_text:
            st.markdown(rendered_text)
        render_stream_events(st.session_state.agent_stream_result.get("events", []))
    if st.session_state.agent_live_eval_result:
        st.subheader("Live Eval")
        eval_result = st.session_state.agent_live_eval_result
        st.write(eval_result.get("summary", {}))
        if eval_result.get("cases"):
            st.dataframe(
                [
                    {
                        "question": item.get("question", ""),
                        "ok": item.get("ok", False),
                        "session_id": item.get("session_id", ""),
                        "actual_tools": ",".join(item.get("actual_tools", [])),
                        "missing_tools": ",".join(item.get("missing_tools", [])),
                        "forbidden_called": ",".join(item.get("forbidden_called", [])),
                    }
                    for item in eval_result.get("cases", [])
                ],
                use_container_width=True,
            )
        else:
            st.warning(eval_result.get("message", "当前没有 live eval 结果。"))
    if st.session_state.agent_session_result:
        st.subheader("Agent Session")
        session = st.session_state.agent_session_result
        st.write(
            {
                "session_id": session.get("session_id", ""),
                "action_count": session.get("action_count", 0),
                "tool_call_count": session.get("tool_call_count", 0),
                "context_warning": session.get("context_warning", False),
            }
        )
        render_agent_context(session.get("agent_context"))
        session_tab1, session_tab2, session_tab3 = st.tabs(["Action Audits", "Tool Calls", "Memory"])
        with session_tab1:
            st.dataframe(session.get("action_audits", []), use_container_width=True)
        with session_tab2:
            st.dataframe(session.get("tool_calls", []), use_container_width=True)
        with session_tab3:
            render_memory_snapshot(session.get("memory"))


with eval_tab:
    st.subheader("Eval Dashboard / Replay Viewer / Memory Browser")
    eval_dash_col1, eval_dash_col2 = st.columns([1, 1])
    with eval_dash_col1:
        if st.button("刷新 Eval Runs", use_container_width=True):
            pass
        recent_runs = service.list_agent_eval_runs(limit=10)
        if recent_runs.get("runs"):
            st.dataframe(recent_runs.get("runs", []), use_container_width=True)
        else:
            st.info("当前还没有评测历史。")
    with eval_dash_col2:
        with st.form("eval_run_detail_form"):
            selected_eval_run_id = st.text_input("Eval Run ID", value=st.session_state.selected_eval_run_id)
            load_eval_run = st.form_submit_button("加载 Eval Run", use_container_width=True)
        if load_eval_run and selected_eval_run_id.strip():
            st.session_state.selected_eval_run_id = selected_eval_run_id.strip()
            st.session_state.agent_eval_suite_result = service.get_agent_eval_run(selected_eval_run_id.strip())

    eval_result_payload = st.session_state.agent_eval_suite_result
    if eval_result_payload:
        st.subheader("Eval Detail")
        if eval_result_payload.get("run"):
            render_eval_run_detail(eval_result_payload)
        elif eval_result_payload.get("run_id"):
            pseudo_payload = {
                "run": {
                    "run_id": eval_result_payload.get("run_id", ""),
                    "baseline_run_id": eval_result_payload.get("baseline_run_id", ""),
                    "created_at": "",
                    "case_count": len(eval_result_payload.get("cases", [])),
                },
                "summary": eval_result_payload.get("summary", {}),
                "comparison": eval_result_payload.get("comparison", {}),
                "cases": eval_result_payload.get("cases", []),
            }
            render_eval_run_detail(pseudo_payload)
        else:
            st.json(eval_result_payload, expanded=False)

    replay_col1, replay_col2 = st.columns([1, 1])
    with replay_col1:
        with st.form("replay_form"):
            replay_session_id = st.text_input(
                "Replay Session ID",
                value=st.session_state.replay_session_id or (st.session_state.agent_session_result or {}).get("session_id", ""),
            )
            replay_submit = st.form_submit_button("加载 Replay", use_container_width=True)
        if replay_submit and replay_session_id.strip():
            st.session_state.replay_session_id = replay_session_id.strip()
            st.session_state.agent_replay_result = service.export_agent_session_replay(replay_session_id.strip())
            st.session_state.agent_memory_result = service.get_agent_memory_snapshot(replay_session_id.strip())
    with replay_col2:
        if st.button("查看全局 Memory", use_container_width=True):
            st.session_state.agent_memory_result = service.list_agent_memories(limit=20)

    if st.session_state.agent_replay_result:
        st.subheader("Replay Timeline")
        st.write(
            {
                "session_id": st.session_state.agent_replay_result.get("session_id", ""),
                "timeline_count": st.session_state.agent_replay_result.get("timeline_count", 0),
            }
        )
        render_replay_timeline(st.session_state.agent_replay_result)

    if st.session_state.agent_memory_result:
        st.subheader("Memory Browser")
        memory_payload = st.session_state.agent_memory_result
        if memory_payload.get("session_memories") is not None:
            mem_metrics = st.columns(4)
            mem_metrics[0].metric("Session Memories", memory_payload.get("session_count", 0))
            mem_metrics[1].metric("Long-Term Memories", memory_payload.get("long_term_count", 0))
            mem_metrics[2].metric("Pending Candidates", memory_payload.get("candidate_count", 0))
            mem_metrics[3].metric("Memory Audits", memory_payload.get("memory_audit_count", 0))
            mem_tab1, mem_tab2, mem_tab3, mem_tab4 = st.tabs(["Session Memories", "Long-Term Memories", "Candidates", "Memory Audits"])
            with mem_tab1:
                st.dataframe(memory_payload.get("session_memories", []), use_container_width=True)
            with mem_tab2:
                st.dataframe(memory_payload.get("long_term_memories", []), use_container_width=True)
            with mem_tab3:
                st.dataframe(memory_payload.get("memory_candidates", []), use_container_width=True)
            with mem_tab4:
                st.dataframe(memory_payload.get("memory_audits", []), use_container_width=True)
        else:
            render_memory_snapshot(memory_payload)

    memory_admin_col1, memory_admin_col2 = st.columns([1, 1])
    with memory_admin_col1:
        with st.form("memory_admin_session_form"):
            session_memory_id = st.text_input("编辑 Session Memory", value=st.session_state.replay_session_id)
            session_memory_actor = st.text_input("Session Memory Actor", value="operator")
            session_summary = st.text_area("Summary", value="", height=100)
            session_key_facts = st.text_area("Key Facts(JSON)", value="{}", height=100)
            session_last_question = st.text_input("Last Question", value="")
            session_last_answer = st.text_area("Last Answer", value="", height=80)
            session_save = st.form_submit_button("保存 Session Memory", use_container_width=True)
            session_delete = st.form_submit_button("删除 Session Memory", use_container_width=True)
        if session_save and session_memory_id.strip():
            try:
                parsed_facts = json.loads(session_key_facts.strip() or "{}")
                if not isinstance(parsed_facts, dict):
                    parsed_facts = {}
                st.session_state.agent_memory_result = service.upsert_agent_session_memory(
                    session_memory_id.strip(),
                    actor=session_memory_actor.strip() or "operator",
                    summary=session_summary,
                    key_facts=parsed_facts,
                    last_question=session_last_question,
                    last_answer=session_last_answer,
                )
            except json.JSONDecodeError as exc:
                st.error(f"Key Facts JSON 无法解析: {exc}")
        if session_delete and session_memory_id.strip():
            st.session_state.agent_memory_result = service.delete_agent_session_memory(
                session_memory_id.strip(),
                actor=session_memory_actor.strip() or "operator",
            )
    with memory_admin_col2:
        with st.form("memory_admin_long_term_form"):
            memory_id = st.text_input("Long-Term Memory ID（更新/删除时填写）", value=st.session_state.selected_memory_id)
            memory_actor = st.text_input("Long-Term Memory Actor", value="approver")
            memory_key = st.text_input("Memory Key", value="preferred_transport")
            memory_content = st.text_area("Content", value="", height=100)
            memory_confidence = st.slider("Confidence", min_value=0.0, max_value=1.0, value=0.7, step=0.1)
            memory_type = st.selectbox("Memory Type", ["manual", "preference", "fact"], index=0)
            memory_provenance = st.selectbox("Provenance", ["manual_curated", "user_stated", "tool_verified", "observed", "inferred"], index=0)
            memory_status = st.selectbox("Status", ["active", "inactive"], index=0)
            memory_pinned = st.checkbox("Pinned", value=False)
            memory_expires_at = st.text_input("Expires At(ISO8601，可留空)", value="")
            long_term_create = st.form_submit_button("创建/覆盖 Long-Term Memory", use_container_width=True)
            long_term_update = st.form_submit_button("更新 Long-Term Memory", use_container_width=True)
            long_term_delete = st.form_submit_button("删除 Long-Term Memory", use_container_width=True)
        if long_term_create and memory_key.strip():
            st.session_state.agent_memory_result = service.create_or_update_long_term_memory(
                memory_key=memory_key.strip(),
                content=memory_content,
                confidence=memory_confidence,
                memory_type=memory_type,
                provenance=memory_provenance,
                status=memory_status,
                is_pinned=memory_pinned,
                expires_at=memory_expires_at.strip(),
                actor=memory_actor.strip() or "approver",
            )
        if long_term_update and memory_id.strip().isdigit():
            st.session_state.selected_memory_id = memory_id.strip()
            st.session_state.agent_memory_result = service.update_long_term_memory(
                int(memory_id.strip()),
                content=memory_content,
                confidence=memory_confidence,
                memory_type=memory_type,
                provenance=memory_provenance,
                status=memory_status,
                is_pinned=memory_pinned,
                expires_at=memory_expires_at.strip(),
                actor=memory_actor.strip() or "approver",
            )
        if long_term_delete and memory_id.strip().isdigit():
            st.session_state.selected_memory_id = memory_id.strip()
            st.session_state.agent_memory_result = service.delete_long_term_memory(
                int(memory_id.strip()),
                actor="admin" if (memory_actor.strip() or "approver") == "admin" else memory_actor.strip() or "approver",
            )

    st.subheader("Memory Candidates Inbox")
    candidate_list = service.list_memory_candidates(limit=20, status="review_queue")
    if candidate_list.get("items"):
        st.dataframe(candidate_list.get("items", []), use_container_width=True)
    else:
        st.info("当前没有待审批的 memory candidates。")
    with st.form("memory_candidate_review_form"):
        candidate_id = st.text_input("Candidate ID", value=st.session_state.selected_candidate_id)
        candidate_reviewer = st.text_input("Reviewer", value="approver")
        candidate_note = st.text_area("Decision Note", value="人工审核候选记忆。", height=80)
        candidate_col1, candidate_col2 = st.columns(2)
        candidate_approve = candidate_col1.form_submit_button("批准入库", use_container_width=True)
        candidate_reject = candidate_col2.form_submit_button("驳回候选", use_container_width=True)
    if candidate_approve and candidate_id.strip():
        st.session_state.selected_candidate_id = candidate_id.strip()
        st.session_state.agent_memory_result = service.approve_memory_candidate(
            candidate_id.strip(),
            reviewed_by=candidate_reviewer.strip() or "operator",
            decision_note=candidate_note.strip(),
        )
    if candidate_reject and candidate_id.strip():
        st.session_state.selected_candidate_id = candidate_id.strip()
        st.session_state.agent_memory_result = service.reject_memory_candidate(
            candidate_id.strip(),
            reviewed_by=candidate_reviewer.strip() or "operator",
            decision_note=candidate_note.strip(),
        )


with governance_tab:
    realtime_tab, approval_tab, batch_run_tab, report_center_tab, ticket_tab, audit_tab = st.tabs(
        ["实时决策中心", "审批中心", "批量执行中心", "报告中心", "工单中心", "审计中心"]
    )

    with realtime_tab:
        decision_metrics_payload = service.decision_metrics()
        metrics = decision_metrics_payload.get("metrics", {})
        metric_cols = st.columns(6)
        metric_cols[0].metric("Events", metrics.get("total_events", 0))
        metric_cols[1].metric("Alerts", metrics.get("total_alerts", 0))
        metric_cols[2].metric("Open Alerts", metrics.get("open_alerts", 0))
        metric_cols[3].metric("Decisions", metrics.get("total_decisions", 0))
        metric_cols[4].metric("Exec Success", f"{metrics.get('execution_success_rate', 0.0):.0%}")
        metric_cols[5].metric("Rollback Rate", f"{metrics.get('rollback_rate', 0.0):.0%}")

        realtime_col1, realtime_col2 = st.columns([1, 1])
        with realtime_col1:
            with st.form("realtime_event_ingest_form"):
                event_type = st.selectbox("Event Type", ["device_offline", "sensor_upload_gap", "ota_failure_spike"], index=0)
                event_source = st.text_input("Source", value="manual_console")
                event_severity = st.selectbox("Severity", ["info", "medium", "warning", "high", "critical"], index=2)
                event_device_id = st.text_input("Device ID(可留空)", value="")
                event_payload = st.text_area("Payload(JSON)", value="{}", height=100)
                ingest_submit = st.form_submit_button("写入实时事件", use_container_width=True)
            if ingest_submit:
                try:
                    payload = json.loads(event_payload.strip() or "{}")
                    normalized_device_id = int(event_device_id.strip()) if event_device_id.strip().isdigit() else None
                    st.session_state.realtime_event_result = service.ingest_realtime_event(
                        event_type,
                        source=event_source.strip() or "manual_console",
                        severity=event_severity,
                        device_id=normalized_device_id,
                        payload=payload if isinstance(payload, dict) else {},
                    )
                except json.JSONDecodeError as exc:
                    st.error(f"Payload JSON 无法解析: {exc}")
            if st.session_state.realtime_event_result:
                st.json(st.session_state.realtime_event_result, expanded=False)
            if st.button("Capture Metric Snapshot", use_container_width=True):
                st.session_state.realtime_event_result = service.snapshot_decision_metrics(reason="streamlit_manual_capture")
        with realtime_col2:
            with st.form("alert_action_form"):
                alert_id = st.text_input("Alert ID", value=st.session_state.selected_alert_id)
                alert_actor = st.text_input("Alert Actor", value="operator")
                suppress_minutes = st.number_input("Suppress Minutes", min_value=1, step=5, value=30)
                suppress_submit = st.form_submit_button("Suppress Alert", use_container_width=True)
                ack_submit = st.form_submit_button("Acknowledge Alert", use_container_width=True)
                resolve_submit = st.form_submit_button("Resolve Alert", use_container_width=True)
            if suppress_submit and alert_id.strip():
                st.session_state.selected_alert_id = alert_id.strip()
                st.session_state.realtime_event_result = service.suppress_alert(
                    alert_id.strip(),
                    actor=alert_actor.strip() or "operator",
                    minutes=int(suppress_minutes),
                    reason="streamlit suppression",
                )
            if ack_submit and alert_id.strip():
                st.session_state.selected_alert_id = alert_id.strip()
                st.session_state.realtime_event_result = service.acknowledge_alert(alert_id.strip(), actor=alert_actor.strip() or "operator")
            if resolve_submit and alert_id.strip():
                st.session_state.selected_alert_id = alert_id.strip()
                st.session_state.realtime_event_result = service.resolve_alert(alert_id.strip(), actor=alert_actor.strip() or "operator")
            with st.form("decision_action_form"):
                decision_id = st.text_input("Decision ID", value=st.session_state.selected_decision_id)
                decision_actor = st.text_input("Decision Actor", value="admin")
                decision_note = st.text_area("Decision Note", value="manual execution", height=80)
                force_execute = st.checkbox("Force Execute", value=False)
                execute_submit = st.form_submit_button("Execute Decision", use_container_width=True)
                rollback_submit = st.form_submit_button("Rollback Decision", use_container_width=True)
            if execute_submit and decision_id.strip():
                st.session_state.selected_decision_id = decision_id.strip()
                st.session_state.realtime_event_result = service.execute_decision(
                    decision_id.strip(),
                    actor=decision_actor.strip() or "operator",
                    decision_note=decision_note.strip(),
                    force_execute=force_execute,
                )
            if rollback_submit and decision_id.strip():
                st.session_state.selected_decision_id = decision_id.strip()
                st.session_state.realtime_event_result = service.rollback_decision_execution(
                    decision_id.strip(),
                    actor=decision_actor.strip() or "approver",
                    reason=decision_note.strip(),
                )
            alerts = service.list_alert_records(limit=20)
            decisions = service.list_decision_records(limit=20)
            events = service.list_realtime_events(limit=20)
            executions = service.list_execution_records(limit=20)
            rollbacks = service.list_rollback_records(limit=20)
            policies = service.list_decision_policies()
            notifications = service.list_notification_records(limit=20)
            metric_history = service.list_decision_metric_snapshots(limit=30)
            rt_tab1, rt_tab2, rt_tab3, rt_tab4, rt_tab5, rt_tab6, rt_tab7, rt_tab8 = st.tabs(["Events", "Alerts", "Decisions", "Executions", "Rollbacks", "Metrics", "Policies", "Notifications"])
            with rt_tab1:
                st.dataframe(events.get("items", []), use_container_width=True)
            with rt_tab2:
                st.dataframe(alerts.get("items", []), use_container_width=True)
            with rt_tab3:
                st.dataframe(decisions.get("items", []), use_container_width=True)
            with rt_tab4:
                st.dataframe(executions.get("items", []), use_container_width=True)
            with rt_tab5:
                st.dataframe(rollbacks.get("items", []), use_container_width=True)
            with rt_tab6:
                overview_col1, overview_col2 = st.columns(2)
                with overview_col1:
                    st.write({"blocked_reason_counts": metrics.get("blocked_reason_counts", {}), "risk_counts": metrics.get("risk_counts", {})})
                with overview_col2:
                    st.write({"execution_status_counts": metrics.get("execution_status_counts", {}), "history_count": metric_history.get("count", 0)})
                st.dataframe(metric_history.get("items", []), use_container_width=True)
            with rt_tab7:
                if st.button("Reload Policies", use_container_width=True):
                    st.session_state.realtime_event_result = service.reload_decision_policies()
                st.dataframe(policies.get("items", []), use_container_width=True)
            with rt_tab8:
                metric_export = service.export_decision_metrics_csv(limit=30)
                st.dataframe(notifications.get("items", []), use_container_width=True)
                st.text_area("Metrics CSV Export", value=metric_export.get("csv", ""), height=180)

    with approval_tab:
        approval_filter_col, approval_detail_col = st.columns([1, 1])
        with approval_filter_col:
            approval_status = st.selectbox("审批状态过滤", approval_status_choices(), index=0)
            approval_list = service.list_approval_requests(limit=20, status=approval_status)
            st.write({"count": approval_list.get("count", 0)})
            approval_items = approval_list.get("items", [])
            if approval_items:
                st.dataframe(approval_items, use_container_width=True)
            else:
                st.info("当前没有审批记录。")
        with approval_detail_col:
            with st.form("approval_action_form"):
                selected_approval_id = st.text_input("审批 ID", value=st.session_state.selected_approval_id)
                approval_operator = st.text_input("审批操作人", value="operator")
                approval_note = st.text_area("审批/治理备注", value="人工复核后执行。", height=80)
                action_col1, action_col2 = st.columns(2)
                action_col3, action_col4 = st.columns(2)
                load_submit = action_col1.form_submit_button("加载详情", use_container_width=True)
                approve_submit = action_col2.form_submit_button("批准并执行", use_container_width=True)
                reject_submit = action_col3.form_submit_button("驳回", use_container_width=True)
                cancel_submit = action_col4.form_submit_button("取消审批", use_container_width=True)
                retry_submit = st.form_submit_button("重试批量失败设备", use_container_width=True)

            selected_id = selected_approval_id.strip()
            if load_submit and selected_id:
                st.session_state.approval_detail_result = service.get_approval(selected_id)
                st.session_state.selected_approval_id = selected_id
            if approve_submit and selected_id:
                st.session_state.ota_confirm_result = service.approve_request(
                    selected_id,
                    approval_operator.strip() or "operator",
                    approval_note.strip(),
                )
                st.session_state.approval_detail_result = service.get_approval(selected_id)
                st.session_state.selected_approval_id = selected_id
            if reject_submit and selected_id:
                st.session_state.approval_detail_result = service.reject_approval_request(
                    selected_id,
                    approval_operator.strip() or "operator",
                    approval_note.strip(),
                )
                st.session_state.selected_approval_id = selected_id
            if cancel_submit and selected_id:
                st.session_state.approval_detail_result = service.cancel_approval_request(
                    selected_id,
                    approval_operator.strip() or "operator",
                    approval_note.strip(),
                )
                st.session_state.selected_approval_id = selected_id
            if retry_submit and selected_id:
                st.session_state.ota_batch_retry_result = service.retry_batch_failed_devices(
                    selected_id,
                    approval_operator.strip() or "operator",
                    approval_note.strip(),
                )
                st.session_state.approval_detail_result = service.get_approval(selected_id)
                st.session_state.selected_approval_id = selected_id

            if st.session_state.approval_detail_result:
                st.subheader("审批详情")
                render_approval_history(st.session_state.approval_detail_result)
                st.json(st.session_state.approval_detail_result, expanded=False)

    with batch_run_tab:
        run_col1, run_col2 = st.columns([1, 1])
        with run_col1:
            batch_runs = service.list_batch_runs(limit=20)
            st.write({"count": batch_runs.get("count", 0)})
            if batch_runs.get("items"):
                st.dataframe(batch_runs["items"], use_container_width=True)
            else:
                st.info("当前没有批量执行记录。")
        with run_col2:
            with st.form("batch_run_action_form"):
                batch_run_id = st.text_input("Batch Run ID", value=st.session_state.selected_batch_run_id)
                load_run_submit = st.form_submit_button("加载批量执行详情", use_container_width=True)
                refresh_run_submit = st.form_submit_button("刷新任务进度", use_container_width=True)
                start_next_submit = st.form_submit_button("启动下一批", use_container_width=True)
                pause_submit = st.form_submit_button("暂停批量推进", use_container_width=True)
                terminate_submit = st.form_submit_button("终止后续批次", use_container_width=True)
            selected_run_id = batch_run_id.strip()
            if load_run_submit and selected_run_id:
                st.session_state.batch_run_detail_result = service.get_batch_run(selected_run_id)
                st.session_state.selected_batch_run_id = selected_run_id
            if refresh_run_submit and selected_run_id:
                st.session_state.batch_run_detail_result = service.refresh_batch_run(selected_run_id)
                st.session_state.selected_batch_run_id = selected_run_id
            if start_next_submit and selected_run_id:
                st.session_state.ota_confirm_result = service.start_next_batch(selected_run_id, "operator", "manual next batch start")
                st.session_state.batch_run_detail_result = service.get_batch_run(selected_run_id)
                st.session_state.selected_batch_run_id = selected_run_id
            if pause_submit and selected_run_id:
                st.session_state.ota_confirm_result = service.pause_batch_run(selected_run_id, "operator", "manual pause")
                st.session_state.batch_run_detail_result = service.get_batch_run(selected_run_id)
                st.session_state.selected_batch_run_id = selected_run_id
            if terminate_submit and selected_run_id:
                st.session_state.ota_confirm_result = service.terminate_batch_run(selected_run_id, "operator", "manual terminate")
                st.session_state.batch_run_detail_result = service.get_batch_run(selected_run_id)
                st.session_state.selected_batch_run_id = selected_run_id
            if st.session_state.batch_run_detail_result:
                detail = st.session_state.batch_run_detail_result
                if detail.get("ok"):
                    batch_run = detail["batch_run"]
                    st.write(batch_run)
                    sla = batch_run.get("summary_payload", {}).get("sla", {})
                    if sla:
                        s1, s2, s3, s4 = st.columns(4)
                        s1.metric("Run Age(s)", sla.get("run_age_sec"))
                        s2.metric("Pending Batches", sla.get("pending_batch_count"))
                        s3.metric("Completion", sla.get("completion_ratio"))
                        s4.metric("Last Update(s)", sla.get("since_last_update_sec"))
                        if sla.get("batch_metrics"):
                            st.dataframe(sla["batch_metrics"], use_container_width=True)
                    st.dataframe(detail.get("items", []), use_container_width=True)
                else:
                    st.warning(detail.get("message", "读取批量执行详情失败。"))

    with report_center_tab:
        report_col1, report_col2 = st.columns([1, 1])
        with report_col1:
            report_list = service.list_reports(limit=20)
            st.write({"count": report_list.get("count", 0)})
            if report_list.get("items"):
                st.dataframe(report_list["items"], use_container_width=True)
            else:
                st.info("当前没有报告记录。")
        with report_col2:
            selected_report_id = st.text_input("报告 ID", value=st.session_state.selected_report_id)
            if st.button("查看报告正文", use_container_width=True):
                st.session_state.selected_report_id = selected_report_id.strip()
            if st.button("由报告生成工单", use_container_width=True) and selected_report_id.strip():
                st.session_state.ticket_create_result = service.create_ticket(
                    report_id=selected_report_id.strip(),
                    severity="high",
                )
            if st.session_state.selected_report_id:
                markdown_result = service.get_report_markdown(st.session_state.selected_report_id)
                if markdown_result.get("ok"):
                    st.code(markdown_result.get("markdown", ""), language="markdown")
                else:
                    st.warning(markdown_result.get("message", "读取报告失败。"))
            if st.session_state.ticket_create_result:
                st.success(f"工单已创建: {st.session_state.ticket_create_result.get('ticket_id', '')}")

    with ticket_tab:
        ticket_col1, ticket_col2 = st.columns([1, 1])
        with ticket_col1:
            ticket_filter_col1, ticket_filter_col2, ticket_filter_col3 = st.columns(3)
            ticket_status_filter = ticket_filter_col1.selectbox("工单状态", ["", "open", "closed"], index=0)
            ticket_severity_filter = ticket_filter_col2.selectbox("工单级别", ["", "low", "medium", "high"], index=0)
            ticket_assignee_filter = ticket_filter_col3.text_input("负责人过滤", value="")
            ticket_list = service.list_tickets(
                limit=20,
                status=ticket_status_filter,
                severity=ticket_severity_filter,
                assignee=ticket_assignee_filter.strip(),
            )
            st.write({"count": ticket_list.get("count", 0)})
            render_ticket_table(ticket_list.get("items", []))
        with ticket_col2:
            with st.form("ticket_action_form"):
                ticket_id = st.text_input("工单 ID", value=st.session_state.selected_ticket_id)
                assignee = st.text_input("指派给", value="alice")
                create_title = st.text_input("新工单标题", value="")
                create_device_id = st.number_input("新工单设备 ID", min_value=1, step=1, value=1)
                create_desc = st.text_area("新工单描述", value="", height=80)
                ticket_col_a, ticket_col_b = st.columns(2)
                create_submit = ticket_col_a.form_submit_button("创建工单", use_container_width=True)
                assign_submit = ticket_col_b.form_submit_button("指派工单", use_container_width=True)
                ticket_col_c, ticket_col_d = st.columns(2)
                close_submit = ticket_col_c.form_submit_button("关闭工单", use_container_width=True)
                reopen_submit = ticket_col_d.form_submit_button("重开工单", use_container_width=True)
            if create_submit:
                st.session_state.ticket_create_result = service.create_ticket(
                    title=create_title.strip(),
                    severity="medium",
                    device_id=int(create_device_id),
                    description=create_desc.strip(),
                )
                st.session_state.selected_ticket_id = st.session_state.ticket_create_result.get("ticket_id", "")
            if assign_submit and ticket_id.strip():
                st.session_state.ticket_assign_result = service.assign_ticket(ticket_id.strip(), assignee.strip())
                st.session_state.selected_ticket_id = ticket_id.strip()
            if close_submit and ticket_id.strip():
                st.session_state.ticket_close_result = service.close_ticket(ticket_id.strip())
                st.session_state.selected_ticket_id = ticket_id.strip()
            if reopen_submit and ticket_id.strip():
                st.session_state.ticket_reopen_result = service.reopen_ticket(ticket_id.strip())
                st.session_state.selected_ticket_id = ticket_id.strip()

            for item in (
                st.session_state.ticket_create_result,
                st.session_state.ticket_assign_result,
                st.session_state.ticket_close_result,
                st.session_state.ticket_reopen_result,
            ):
                if item:
                    st.json(item, expanded=False)

    with audit_tab:
        audit_col1, audit_col2 = st.columns([1, 1])
        with audit_col1:
            tool_logs = service.list_tool_call_logs(limit=20)
            st.write({"tool_calls": tool_logs.get("count", 0)})
            if tool_logs.get("items"):
                st.dataframe(tool_logs["items"], use_container_width=True)
            else:
                st.info("当前没有 tool call 记录。")
        with audit_col2:
            action_logs = service.list_action_audits(limit=20)
            st.write({"actions": action_logs.get("count", 0)})
            if action_logs.get("items"):
                st.dataframe(action_logs["items"], use_container_width=True)
            else:
                st.info("当前没有 action audit 记录。")
