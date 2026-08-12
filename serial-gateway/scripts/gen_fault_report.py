#!/usr/bin/env python3
import argparse
import datetime as dt
import os


def read_text(path: str) -> str:
    if not os.path.exists(path):
        return ""
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        return f.read().strip()


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--input", required=True)
    p.add_argument("--output", required=True)
    args = p.parse_args()

    cases = []
    for name in sorted(os.listdir(args.input)):
        cdir = os.path.join(args.input, name)
        if not os.path.isdir(cdir):
            continue
        result = read_text(os.path.join(cdir, "result.txt")) or "UNKNOWN"
        status = read_text(os.path.join(cdir, "status.txt"))
        events = read_text(os.path.join(cdir, "events.txt"))
        last_event = ""
        if events:
            lines = [ln for ln in events.splitlines() if ln.strip()]
            if lines:
                last_event = lines[-1]
        cases.append((name, result, status, last_event))

    now = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    lines = []
    lines.append("# STM32 OTA 故障注入报告")
    lines.append("")
    lines.append(f"生成时间: {now}")
    lines.append("")
    lines.append("| Case | Result | Status | Last Event |")
    lines.append("|---|---|---|---|")
    for name, result, status, last_event in cases:
        s = status.replace("|", "\\|")
        e = last_event.replace("|", "\\|")
        lines.append(f"| {name} | {result} | {s} | {e} |")

    lines.append("")
    lines.append("## 明细")
    lines.append("")
    for name, _, status, _ in cases:
        lines.append(f"### {name}")
        lines.append("")
        lines.append("```text")
        lines.append(status or "(no status)")
        lines.append("```")
        lines.append("")

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
