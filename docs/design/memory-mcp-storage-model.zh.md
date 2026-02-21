# Memory MCP 存储模型设计（ZH）

## 1. 设计目标（What / Why）

Memory MCP 的存储模型要同时满足三类约束：
1. 写入后可立即参与检索，保证交互系统的短路径可用性。
2. 稀疏索引、稠密索引和正文数据保持一致语义，避免“可查不可读”或“可读不可排”。
3. 在内存预算受限时，仍能通过回收和压缩维持服务稳定性。

该模型围绕一个核心原则：**文档、索引、事件投影在逻辑上同属一份记录状态**。

## 2. 系统边界与组件职责（How）

端到端路径由四层组成：
1. MCP 层（`McpDispatcher`）：协议解码、方法分发、结果编码。
2. 引擎层（`MemoryEngine`）：记录生命周期、持久化编排、容量治理。
3. 检索层（`HybridRetriever`）：索引内存视图、召回与重排。
4. 存储层（`IStoreAdapter`）：键值落盘、批写、扫描、容量估算。

关键源码入口：
- `src/mcp/mcp_dispatcher.cpp`
- `src/core/memory_engine.cpp`
- `src/core/retriever.cpp`
- `src/store/eloqstore_adapter.cpp`
- `src/store/in_memory_store_adapter.cpp`

## 3. EloqStore 逻辑模型（namespace / key / value / routing）

| Namespace | Key 模式 | Value 核心内容 | Routing Key |
|---|---|---|---|
| `mem_docs` | `ws/{workspace}/doc/{doc_id}` | 正文、metadata、pin、ttl、时间戳 | `workspace + doc_id` |
| `mem_term_dict` | `ws/{workspace}/term/{term}` | df/cf/max_tf/block_refs/updated_at | `workspace + term` |
| `mem_posting_blk` | `ws/{workspace}/term/{term}/b/{bucket}/blk/{blk_id}` | posting block（doc/tf/flags） | `workspace + term + bucket` |
| `mem_vec_code` | `ws/{workspace}/vec/{bucket}/{doc_id}` | 量化向量码、norm、model_id | `workspace + bucket` |
| `mem_vec_fp` | `ws/{workspace}/vecfp/{doc_id}` | 全精度向量 | `workspace + doc_id` |
| `mem_route_meta` | `ws/{workspace}` | bucket_count/hot_level/shard_hint | `workspace` |
| `ds_events` | `ws/{workspace}/ts/{ts}/seq/{seq}` | write/pin/compact 事件日志 | `workspace + seq` |
| `ds_projection_ckpt` | `ws/{workspace}` | 事件回放检查点 | `workspace` |

路由意图：
- 文档与向量围绕 `workspace + doc_id` 聚合。
- 词项与 posting 围绕 `workspace + term` 聚合。
- 事件与 checkpoint 按 workspace 线性推进。

## 4. 写入事务单元（文档、索引、事件、检查点）

`memory.write` 在引擎层构建一个批次写入集合，单次 `BatchWrite` 提交：
1. `mem_docs`：正文记录行。
2. `mem_term_dict`：词项统计更新。
3. `mem_posting_blk`：posting block 追加或更新。
4. `mem_vec_code` + `mem_vec_fp`：稠密向量持久化。
5. `ds_events`：事件日志追加。
6. `ds_projection_ckpt`：回放位置推进。
7. `mem_route_meta`：workspace 路由元信息更新。

事务粒度目标：
- 读路径看到的数据要么是“上一个稳定状态”，要么是“下一个完整状态”。
- 避免正文和索引跨版本错位。

## 5. 读路径与索引可见性

读路径分成两个层面：
1. 存储层可见性：`Scan/Get` 提供 namespace 级读取。
2. 检索层可见性：`HybridRetriever` 持有内存索引快照并执行召回。

可见性规则：
- tombstone 或过期记录不会进入最终命中。
- 命中结果需要能回读到 `mem_docs` 主体字段。
- 向量命中与词项命中在 merge 阶段按同一 `memory_id` 归并。

## 6. pin / ttl / tombstone / compact 底层语义

### 6.1 pin
- pin 表示“保留优先级”，影响排序和淘汰选择。
- pin 受 `pin_quota_per_workspace` 与 `max_pinned_ratio` 约束。

### 6.2 ttl
- `ttl_s > 0` 时按更新时间判断过期。
- 过期记录在查询输出阶段被过滤。

### 6.3 tombstone
- 逻辑删除采用 tombstone 标记，先保证可回放一致性。
- tombstone 记录不参与检索召回。

### 6.4 compact
- compact 负责把 tombstone 和冷数据做进一步整理。
- 输出包含回收计数和 compact 前后容量指标。

## 7. 内存预算与回收机制

预算由 `MemoryEngineOptions` 驱动：
- `mem_budget_mb`
- `disk_budget_gb`
- resident 相关阈值

回收关键机制：
1. 估算每条记录 resident charge。
2. 当 `resident_used_bytes` 超阈值时选择可淘汰候选。
3. 优先保留 pinned 与热点记录。
4. 更新 `resident_evicted_count` 与 `capacity_blocked`。

`memory.stats` 暴露的关键容量字段：
- `resident_used_bytes`
- `resident_limit_bytes`
- `resident_evicted_count`
- `capacity_blocked`

## 8. 故障与恢复（BatchWrite / checkpoint / replay）

### 8.1 BatchWrite 失败
- 当前批次不应被视为成功写入。
- 写接口返回 `ok=false` 或告警信息。

### 8.2 checkpoint 缺失
- 回放阶段可退化为基于事件日志的全量扫描恢复。

### 8.3 事件回放
- `Warmup` 读取 `ds_projection_ckpt` 定位起点。
- 逐条应用 `ds_events` 到内存投影与检索索引。

恢复目标：
- 重启后达到可查询的一致状态。
- 避免“事件已写入但索引未恢复”的永久分叉。

## 9. 性能权衡（写放大 / fan-out / 热点拆桶）

### 9.1 写放大
- 单次写入同时更新正文、词典、posting、向量、事件和检查点。
- 代价是写放大，收益是检索一致性与恢复能力。

### 9.2 查询 fan-out
- 通过 routing key 降低全局扫描概率。
- 词项路径和向量路径各自控制候选规模后再 merge。

### 9.3 热 workspace 拆桶
- 热点 workspace 可通过 bucket 扩展写入并行度。
- 查询 fan-out 限制在目标 workspace 的 bucket 集，避免全局广播。
