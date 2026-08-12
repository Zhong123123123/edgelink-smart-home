from __future__ import annotations

from typing import Any


DEFAULT_AGENT_EVAL_CASES = [
    {
        "question": "DEVICE_001 当前状态正常吗？",
        "expected_tools": [{"name": "get_device_status"}],
        "description": "简单设备状态查询",
    },
    {
        "question": "查询 sensor-01 最近 5 条传感器历史数据",
        "expected_tools": [{"name": "get_sensor_history"}],
        "description": "传感器历史查询",
    },
    {
        "question": "给设备 1 做 stm32f407-smarthome-1.3.9-a1 的 OTA 升级",
        "expected_tools": [
            {"name": "ota_risk_check"},
            {"name": "ota_create_request"},
        ],
        "must_not_call": ["approve_ota_request"],
        "description": "OTA 风险检查后创建审批",
    },
]


def evaluate_agent_case(case: dict[str, Any], result: dict[str, Any]) -> dict[str, Any]:
    expected_tools = [str(item.get("name", "")) for item in case.get("expected_tools", []) if str(item.get("name", ""))]
    forbidden_tools = [str(item) for item in case.get("must_not_call", []) if str(item)]
    actual_tools = _extract_tool_names(result)
    actual_tool_set = set(actual_tools)

    missing_tools = [name for name in expected_tools if name not in actual_tool_set]
    forbidden_called = [name for name in forbidden_tools if name in actual_tool_set]
    order_ok = _contains_subsequence(actual_tools, expected_tools) if expected_tools else True
    ok = bool(result.get("ok", False))
    interrupted = bool(result.get("interrupted", False))
    failure_reasons: list[str] = []
    if not ok:
        failure_reasons.append("agent_run_failed")
    if missing_tools:
        failure_reasons.append(f"missing_tools:{','.join(missing_tools)}")
    if forbidden_called:
        failure_reasons.append(f"forbidden_tools:{','.join(forbidden_called)}")
    if not order_ok:
        failure_reasons.append("tool_order_mismatch")
    if not str(result.get("answer", "")).strip():
        failure_reasons.append("empty_answer")

    return {
        "question": str(case.get("question", "")),
        "ok": ok and not missing_tools and not forbidden_called and order_ok,
        "agent_ok": ok,
        "interrupted": interrupted,
        "expected_tools": expected_tools,
        "actual_tools": actual_tools,
        "missing_tools": missing_tools,
        "forbidden_called": forbidden_called,
        "tool_order_ok": order_ok,
        "answer_present": bool(str(result.get("answer", "")).strip()),
        "failure_reasons": failure_reasons,
    }


def summarize_agent_eval_results(results: list[dict[str, Any]]) -> dict[str, Any]:
    total = len(results)
    passed = sum(1 for item in results if item.get("ok"))
    answer_present = sum(1 for item in results if item.get("answer_present"))
    interrupted = sum(1 for item in results if item.get("interrupted"))
    return {
        "total": total,
        "passed": passed,
        "failed": total - passed,
        "pass_rate": (passed / total) if total else 0.0,
        "answer_present_rate": (answer_present / total) if total else 0.0,
        "interrupt_rate": (interrupted / total) if total else 0.0,
    }


def run_live_agent_eval(
    cases: list[dict[str, Any]],
    runner: Any,
    session_prefix: str = "agent-live-eval",
) -> dict[str, Any]:
    results: list[dict[str, Any]] = []
    for index, case in enumerate(cases, start=1):
        question = str(case.get("question", "")).strip()
        session_id = f"{session_prefix}-{index}"
        try:
            run_result = runner(question, session_id)
        except Exception as exc:
            run_result = {
                "ok": False,
                "answer": "",
                "tool_trace": [],
                "interrupted": False,
                "error": f"{type(exc).__name__}: {exc}",
            }
        evaluation = evaluate_agent_case(case, run_result)
        evaluation["session_id"] = session_id
        evaluation["description"] = str(case.get("description", ""))
        evaluation["result"] = run_result
        results.append(evaluation)
    return {
        "cases": results,
        "summary": summarize_agent_eval_results(results),
    }


def compare_agent_eval_runs(
    current_cases: list[dict[str, Any]],
    baseline_cases: list[dict[str, Any]] | None,
    baseline_run_id: str = "",
) -> dict[str, Any]:
    if not baseline_cases:
        return {
            "has_baseline": False,
            "baseline_run_id": baseline_run_id,
            "regressions": [],
            "improvements": [],
            "unchanged": len(current_cases),
        }

    baseline_by_question = {
        str(item.get("question", "")).strip(): item
        for item in baseline_cases
        if str(item.get("question", "")).strip()
    }
    regressions: list[dict[str, Any]] = []
    improvements: list[dict[str, Any]] = []
    unchanged = 0

    for item in current_cases:
        question = str(item.get("question", "")).strip()
        baseline = baseline_by_question.get(question)
        if baseline is None:
            unchanged += 1
            continue
        current_ok = bool(item.get("ok", False))
        baseline_ok = bool(baseline.get("ok", False))
        if baseline_ok and not current_ok:
            regressions.append(
                {
                    "question": question,
                    "previous_ok": baseline_ok,
                    "current_ok": current_ok,
                    "current_failure_reasons": list(item.get("failure_reasons", [])),
                }
            )
        elif not baseline_ok and current_ok:
            improvements.append(
                {
                    "question": question,
                    "previous_ok": baseline_ok,
                    "current_ok": current_ok,
                }
            )
        else:
            unchanged += 1

    return {
        "has_baseline": True,
        "baseline_run_id": baseline_run_id,
        "regressions": regressions,
        "improvements": improvements,
        "unchanged": unchanged,
    }


def _extract_tool_names(result: dict[str, Any]) -> list[str]:
    names: list[str] = []
    for item in result.get("tool_trace", []):
        if not isinstance(item, dict):
            continue
        name = str(item.get("tool", "") or item.get("tool_name", "")).strip()
        if name:
            names.append(name)
    return names


def _contains_subsequence(actual: list[str], expected: list[str]) -> bool:
    if not expected:
        return True
    idx = 0
    for item in actual:
        if item == expected[idx]:
            idx += 1
            if idx == len(expected):
                return True
    return False
