# Gateway AI Assistant Demo Script

## 1. 演示目标

用 5 到 8 分钟展示下面 4 件事：

1. 这是在真实嵌入式网关项目上扩出来的 AI 运维助手，不是纯聊天 demo
2. 系统能查运行态、历史数据、OTA、日志和文档
3. 批量 OTA 已经具备审批、分批推进、失败重试、暂停和终止能力
4. Agent 不只是回答问题，还能执行治理动作，并支持流式输出、审批中断恢复和 session 轨迹回放

---

## 2. 启动方式

### 2.1 FastAPI

```bash
python3 -m uvicorn gateway_ai_assistant.app:app --host 127.0.0.1 --port 8010
```

### 2.2 Streamlit

```bash
python3 -m streamlit run gateway_ai_assistant/streamlit_app.py
```

---

## 3. 推荐演示顺序

### Step 1. 总览页

打开 `总览` 页，先讲 20 秒：

- 系统总览里能看到待审批、批量执行中、开放工单、报告总数
- 这说明它已经不是单点问答，而是一个带治理状态的运维 PoC

### Step 2. 自然语言问答

在 `问答演示` 页输入：

```text
DEVICE_001 当前状态正常吗？
```

再输入：

```text
最近 OTA 失败任务变多，结合文档分析可能原因
```

讲解重点：

- 第一类问题走 Runtime / SQL
- 第二类问题会走 Hybrid，把 SQL 结果和文档引用组合起来
- 页面里能看到 tool trace，不是黑盒回答

### Step 3. 批量 OTA 计划与审批

去 `Workflow 演示 -> 批量 OTA`

输入设备：

```text
1,2,3
```

点击：

1. `生成批量计划`
2. `创建批量审批`
3. `批准并执行批量下发`

讲解重点：

- 不是直接一次性推给所有设备
- 先做风险检查和灰度分批
- 审批后只启动第一批
- 系统会生成 `batch_run_id`

### Step 4. 批量执行中心

去 `治理中心 -> 批量执行中心`

输入刚才的 `batch_run_id`，依次点击：

1. `加载批量执行详情`
2. `刷新任务进度`
3. `启动下一批`

如果想演示治理动作，再点：

1. `暂停批量推进`
2. `终止后续批次`

讲解重点：

- 这里可以看到设备级状态，不只是审批状态
- 已支持 `queued / waiting_batch / task_created / running / success / failed / cancelled`
- 已支持批次级 SLA 指标：运行时长、待执行批次数、完成比例、最近更新时间
- “终止”只会终止后续批次，不会强行杀掉已发出的网关 OTA 任务

### Step 5. 故障诊断与工单

去 `Workflow 演示 -> 诊断与报告`

点击：

1. `执行规则诊断`
2. `生成报告`
3. `根据当前报告创建工单`

再去 `治理中心 -> 工单中心`

演示：

1. 状态筛选
2. 指派工单
3. 关闭工单
4. 重开工单

讲解重点：

- 报告和工单已经形成闭环
- 诊断和 `fault_ticket` 报告现在都走只读 `DiagnosisTeam` 并行采集
- 页面里能直接看到 `collection_mode` 和 `team_trace`
- 工单动作会记审计日志

### Step 6. Agent 动作

去 `Agent 演示`

先输入：

```text
请对 device_ids=1,2,3 做批量 OTA 风险检查并创建审批 firmware_id=fw-1
```

然后再输入：

```text
请启动下一批 OTA batch_run_id=batch-run-xxx operator=tester
```

讲解重点：

- Agent 现在不只会查，还会做治理动作
- 既能 `plan / execute`，也能直接走 `chat / stream`
- 每一步的执行轨迹、审批中断和 session 上下文都能追踪

### Step 7. Agent Streaming 与 Resume

在 `Agent 演示` 页点击 `流式执行`，可用：

```text
请给设备 1 升级固件 fw-1
```

如果命中写操作审批，继续演示：

1. 页面出现 `interrupt`
2. 选择 `approve / reject / edit`
3. 提交 resume

讲解重点：

- 写操作不会被模型直接执行，会先进入 HITL
- `resume` 已支持 `approve / reject / edit`
- 流式事件不是简单吐 token，而是包含 `tool_start / tool_end / interrupt / final_answer / done`

---

## 4. 面试时建议讲法

### 4.1 一句话概括

```text
我是在原有嵌入式网关项目上新增了一个独立的 AI 运维编排层，复用现有 SQLite、OTA、日志和 API，把自然语言查询、故障诊断、报告工单、OTA 审批和批量灰度治理串成了一套可运行的 Agent PoC。
```

### 4.2 强调点

- 没改设备端，不侵入 STM32 / ESP32 逻辑
- 不是让模型直接瞎调系统，关键动作都经过审批或治理接口
- LangGraph Agent 的高风险写动作会先进入 HITL，再由人工 `resume`
- 批量 OTA 不是一次性群发，而是有 `batch_run` 的分批推进模型
- 有测试、有 API、有前端、有审计，比较接近正式系统雏形

---

## 5. 备用问法

如果现场数据不理想，可以用这些问题：

```text
你会干什么？
```

```text
最近有哪些系统事件？
```

```text
请诊断 device_id=1 最近 10 条数据
```

```text
请启动下一批 OTA batch_run_id=batch-run-xxx operator=tester
```

---

## 6. 注意事项

- 如果没有真实 LLM key，系统会退化成 mock answer，但流程仍可演示
- 批量执行状态依赖现有网关 OTA task 状态刷新
- “暂停/终止”是治理层动作，不代表一定能中断已经发出的底层任务
- 串口不可用时，assistant 会拦住串口 OTA；这不影响 `tcp_binary` / `mqtt` 链路演示
