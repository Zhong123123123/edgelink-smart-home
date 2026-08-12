from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Any, Callable


@dataclass(frozen=True)
class DiagnosisSubAgent:
    agent_name: str
    output_key: str
    tool_name: str
    tool_input_builder: Callable[[int, int], dict[str, Any]]
    runner_builder: Callable[[int, int], Callable[[], dict[str, Any]]]


class DiagnosisTeam:
    """Readonly multi-agent team for device diagnosis."""

    def __init__(self, service: Any) -> None:
        self.service = service
        self.sub_agents = [
            DiagnosisSubAgent(
                agent_name="StatusAgent",
                output_key="device_status",
                tool_name="get_device_status",
                tool_input_builder=lambda device_id, _limit: {"device_ref": str(device_id)},
                runner_builder=lambda device_id, _limit: lambda: self.service.runtime_gateway.get_device_status(str(device_id)),
            ),
            DiagnosisSubAgent(
                agent_name="SensorAgent",
                output_key="sensor_history",
                tool_name="get_sensor_history",
                tool_input_builder=lambda device_id, limit: {"device_ref": str(device_id), "limit": limit},
                runner_builder=lambda device_id, limit: lambda: self.service.runtime_gateway.get_sensor_history(str(device_id), limit=limit),
            ),
            DiagnosisSubAgent(
                agent_name="EventAgent",
                output_key="system_events",
                tool_name="get_system_events",
                tool_input_builder=lambda _device_id, limit: {"limit": limit},
                runner_builder=lambda _device_id, limit: lambda: self.service.runtime_gateway.get_system_events(limit=limit),
            ),
            DiagnosisSubAgent(
                agent_name="OtaAgent",
                output_key="recent_ota_tasks",
                tool_name="list_recent_ota_tasks_for_device",
                tool_input_builder=lambda device_id, limit: {"device_id": device_id, "limit": limit},
                runner_builder=lambda device_id, limit: lambda: self.service.ota_tools.list_recent_ota_tasks_for_device(device_id, limit=limit),
            ),
        ]

    def collect_runtime_inputs(
        self,
        session_id: str,
        question: str,
        device_id: int,
        normalized_limit: int,
    ) -> dict[str, Any]:
        def run_sub_agent(spec: DiagnosisSubAgent) -> tuple[str, dict[str, Any], dict[str, Any], dict[str, Any]]:
            tool_input = spec.tool_input_builder(device_id, normalized_limit)
            result, trace_item = self.service._run_tool(
                session_id,
                question,
                spec.tool_name,
                tool_input,
                spec.runner_builder(device_id, normalized_limit),
            )
            team_item = {
                "agent_name": spec.agent_name,
                "output_key": spec.output_key,
                "tool_name": spec.tool_name,
                "tool_status": trace_item["tool_status"],
            }
            return spec.output_key, result, trace_item, team_item

        collected: dict[str, dict[str, Any]] = {}
        trace_by_key: dict[str, dict[str, Any]] = {}
        team_trace_by_key: dict[str, dict[str, Any]] = {}
        with ThreadPoolExecutor(max_workers=len(self.sub_agents)) as executor:
            futures = [executor.submit(run_sub_agent, spec) for spec in self.sub_agents]
            for future in futures:
                output_key, result, trace_item, team_item = future.result()
                collected[output_key] = result
                trace_by_key[output_key] = trace_item
                team_trace_by_key[output_key] = team_item

        ordered_tool_trace = [trace_by_key[spec.output_key] for spec in self.sub_agents]
        ordered_team_trace = [team_trace_by_key[spec.output_key] for spec in self.sub_agents]
        return {
            "device_status": collected["device_status"],
            "sensor_history": collected["sensor_history"],
            "system_events": collected["system_events"],
            "recent_ota_tasks": collected["recent_ota_tasks"],
            "tool_trace": ordered_tool_trace,
            "team_trace": ordered_team_trace,
            "collection_mode": "multi_agent_read_team",
        }
