# Memory MCP API 契约（ZH）

## 1. 目标与范围

本文档定义 Memory MCP 的正式接口契约，包括请求格式、响应格式、错误模型和运行语义。

适用对象：
- MCP 客户端实现者
- 网关与平台集成方
- 需要按字段级别对接的工程团队

服务端公开方法集合固定为：
- `memory.describe`
- `memory.write`
- `memory.query`
- `memory.pin`
- `memory.stats`
- `memory.compact`

## 2. 协议与传输模型

### 2.1 HTTP 入口

- 健康检查：`GET /health`
- MCP 调用：`POST /mcp`
- 契约描述：`GET /mcp/describe`

### 2.2 JSON-RPC 入口

`POST /mcp` 支持 JSON-RPC 2.0：
- `initialize`
- `tools/list`
- `tools/call`
- 直接方法调用（`memory.*`）

### 2.3 响应封装

直接 MCP 调用响应：
```json
{"id": 1, "result": {...}}
```
或
```json
{"id": 1, "error": {"code": -32601, "message": "method not found"}}
```

JSON-RPC 调用响应：
```json
{"jsonrpc": "2.0", "id": 1, "result": {...}}
```
或
```json
{"jsonrpc": "2.0", "id": 1, "error": {"code": -32602, "message": "invalid params"}}
```

## 3. 通用类型系统与约定

### 3.1 JSON 类型

- `string`：UTF-8 文本
- `integer`：JSON 数值整型语义（非字符串）
- `number`：JSON 浮点数值（非字符串）
- `boolean`：`true/false`
- `object`：键值映射
- `array<T>`：同构数组

### 3.2 通用字段约定

- `workspace_id`：逻辑隔离主键
- `session_id`：会话维度标识，默认 `default`
- `updated_at_ms`：Unix epoch 毫秒
- `token_budget`：响应内容预算（近似 token 估算）

### 3.3 排序与稳定性

- `memory.query` 的 `hits` 按 `scores.final` 降序返回。
- 过滤和预算截断在排序之后执行。

## 4. 公开方法集合

| 方法 | 说明 |
|---|---|
| `memory.describe` | 返回服务能力、方法与字段 schema |
| `memory.write` | 写入记录并更新索引 |
| `memory.query` | 稀疏+稠密混合检索 |
| `memory.pin` | 设置或取消记录 pin 状态 |
| `memory.stats` | 返回运行时与索引统计 |
| `memory.compact` | 执行 compact 与清理 |

## 5. 方法规范

### 5.1 `memory.describe`

#### 作用
返回可机器消费的正式契约，用于客户端自动校验参数、生成调用结构和统一错误处理。

#### 请求字段

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `include_examples` | boolean | 否 | `false` | `true` 时返回每个方法的 `examples` |

#### 响应字段（核心）
- `describe_version`：契约主版本（客户端解析器兼容闸门）
- `schema_revision`：字段修订号（同主版本下的变更标识）
- `generated_at_ms`：服务端生成时间戳（毫秒）
- `server`：服务元信息
- `method_names`：公开方法名数组
- `methods`：方法契约字典（扁平 key，例如 `methods["memory.write"]`）
- `errors.http[]`：HTTP 错误模型
- `errors.jsonrpc[]`：JSON-RPC 错误模型

`methods[<method_name>]` 固定包含：

| 字段 | 类型 | 说明 |
|---|---|---|
| `summary` | string | 方法摘要 |
| `input_schema` | object | 标准 JSON Schema 风格输入定义 |
| `output_schema` | object | 标准 JSON Schema 风格输出定义 |
| `semantics` | object | 运行语义（排序、预算、幂等、约束） |
| `errors` | array<string> | 方法级错误引用（指向 `errors` 条目类别） |
| `examples` | array<object> | 可选，仅 `include_examples=true` 返回 |

#### 成功示例（默认不含 examples）
```json
{
  "id": 1,
  "result": {
    "describe_version": "2.0.0",
    "schema_revision": "2026-02-19.1",
    "generated_at_ms": 1771503385237,
    "server": {
      "name": "poorguy-mem",
      "kind": "memory-server",
      "protocol": "jsonrpc-2.0",
      "transport": "http",
      "endpoint": "/mcp",
      "describe_endpoint": "/mcp/describe",
      "sync_routes_available": false,
      "write_ack_mode": "durable",
      "supported_backends": ["eloqstore", "inmemory"]
    },
    "method_names": [
      "memory.describe",
      "memory.write",
      "memory.query",
      "memory.pin",
      "memory.stats",
      "memory.compact"
    ],
    "methods": {
      "memory.describe": {
        "summary": "Return machine-readable server contract and method schemas.",
        "input_schema": {
          "type": "object",
          "properties": {
            "include_examples": {"type": "boolean", "default": false}
          }
        },
        "output_schema": {"type": "object"},
        "semantics": {
          "payload_size": "Examples are omitted unless include_examples=true."
        },
        "errors": ["jsonrpc.method_not_found", "jsonrpc.invalid_params"]
      }
    },
    "errors": {
      "http": [
        {"name": "invalid_json_http", "status": 400, "message": "invalid JSON", "when": "malformed request body"}
      ],
      "jsonrpc": [
        {"name": "method_not_found", "code": -32601, "message": "method not found", "when": "unknown method on POST /mcp"}
      ]
    }
  }
}
```

#### 成功示例（`include_examples=true`）
```json
{
  "id": 1,
  "result": {
    "methods": {
      "memory.write": {
        "examples": [
          {
            "name": "write_success",
            "request": {"method": "memory.write", "params": {"workspace_id": "demo"}},
            "response": {"result": {"ok": true}}
          }
        ]
      }
    }
  }
}
```

#### 失败示例（参数类型错误）
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {"code": -32602, "message": "invalid params"}
}
```
说明：例如把 `include_examples` 传成非布尔值时，客户端或网关可据 `input_schema` 拦截，服务端也会返回参数错误。

#### `/mcp/describe` 对齐规则
- `GET /mcp/describe` 的返回对象与 `memory.describe` 的 `result` 完全同构。
- 建议客户端优先使用 `memory.describe`（便于走统一 JSON-RPC 通道）；只读场景可用 `/mcp/describe`。

#### 客户端消费建议
1. 启动时调用一次 `memory.describe`，校验 `describe_version` 与 `schema_revision`。
2. 按 `methods[method_name].input_schema` 做请求参数校验。
3. 按 `methods[method_name].output_schema` 做响应解码与类型断言。
4. 按 `errors.http/jsonrpc` 构建统一错误映射表。
5. 日常请求使用 `include_examples=false`；仅调试或代码生成时打开 `include_examples=true`。

### 5.2 `memory.write`

#### 作用
把记录写入存储并更新检索索引。

#### 请求字段

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `workspace_id` | string | 是 | - | 工作区标识 |
| `session_id` | string | 否 | `default` | 会话标识 |
| `records` | array<object> | 是 | - | 写入记录列表 |
| `write_mode` | string | 否 | `upsert` | `upsert` 或 `append` |

`records[i]` 字段：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `record_id` | string | 否 | 自动生成 | 记录 ID |
| `source` | string | 否 | `turn` | 来源类型 |
| `content` | string | 是 | - | 主体内容 |
| `tags` | array<string> | 否 | `[]` | 标签 |
| `importance` | number | 否 | `1.0` | 重要度 |
| `ttl_s` | integer | 否 | `0` | TTL 秒数 |
| `pin` | boolean | 否 | `false` | 是否 pin |
| `dedup_key` | string | 否 | `""` | 去重键 |
| `metadata` | object<string,string> | 否 | `{}` | 扩展元数据 |

#### 响应字段

| 字段 | 类型 | 说明 |
|---|---|---|
| `ok` | boolean | 写入流程是否成功 |
| `stored_ids` | array<string> | 实际写入 ID 列表 |
| `deduped_ids` | array<string> | 去重命中 ID 列表 |
| `index_generation` | integer | 索引代次 |
| `warnings` | array<string> | 非致命告警 |

#### 成功示例
```json
{
  "id": 2,
  "result": {
    "ok": true,
    "stored_ids": ["m-00000001771503385237-00000000000000000000"],
    "deduped_ids": [],
    "index_generation": 1,
    "warnings": []
  }
}
```

#### 失败示例
```json
{
  "id": 2,
  "result": {
    "ok": false,
    "stored_ids": [],
    "deduped_ids": [],
    "index_generation": 0,
    "warnings": ["workspace_id is required"]
  }
}
```

### 5.3 `memory.query`

#### 作用
执行混合召回与重排，返回候选命中。

#### 请求字段

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `workspace_id` | string | 是 | - | 工作区标识 |
| `query` | string | 是 | - | 查询文本 |
| `top_k` | integer | 否 | `8` | 返回条数 |
| `token_budget` | integer | 否 | `2048` | 内容预算 |
| `filters` | object | 否 | `{}` | 过滤器 |
| `recall` | object | 否 | `{}` | 召回参数 |
| `rerank` | object | 否 | `{}` | 重排权重 |
| `debug` | boolean | 否 | `false` | 是否返回调试统计 |

`filters`：`session_id/sources/tags_any/updated_after_ms/updated_before_ms/pinned_only`。

`recall`：`sparse_k/dense_k/oversample`。

`rerank`：`w_sparse/w_dense/w_freshness/w_pin`。

#### 响应字段

| 字段 | 类型 | 说明 |
|---|---|---|
| `hits` | array<object> | 命中列表 |
| `hits[].memory_id` | string | 记录 ID |
| `hits[].content` | string | 内容 |
| `hits[].scores` | object | 分数组件 |
| `hits[].scores.final` | number | 最终分 |
| `debug_stats` | object | 调试统计 |
| `debug_stats.latency_ms` | object | 子阶段耗时 |

#### 成功示例
```json
{
  "id": 3,
  "result": {
    "hits": [
      {
        "memory_id": "m-00000001771503385237-00000000000000000000",
        "content": "remember retry with exponential backoff",
        "scores": {"sparse": 1.0, "dense": 1.0, "freshness": 0.99, "pin": 0.0, "final": 0.94},
        "updated_at_ms": 1771503385237,
        "pinned": false
      }
    ],
    "debug_stats": {
      "sparse_candidates": 1,
      "dense_candidates": 1,
      "merged_candidates": 1,
      "latency_ms": {"sparse": 0.01, "dense": 0.04, "rerank": 0.01, "total": 0.09}
    }
  }
}
```

#### 失败示例
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "error": {"code": -32602, "message": "invalid params: params must be object"}
}
```
说明：JSON-RPC 调用 `memory.query` 时若 `params` 不是对象，返回 `-32602`。

### 5.4 `memory.pin`

#### 作用
设置记录 pin 或取消 pin。

#### 请求字段

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `workspace_id` | string | 是 | - | 工作区标识 |
| `memory_id` | string | 是 | - | 记录 ID |
| `pin` | boolean | 否 | `true` | `true` 为 pin，`false` 为 unpin |

#### 响应字段

| 字段 | 类型 | 说明 |
|---|---|---|
| `ok` | boolean | 操作结果 |
| `error` | string | 失败原因（仅 `ok=false`） |

#### 成功示例
```json
{
  "id": 4,
  "result": {"ok": true}
}
```

#### 失败示例
```json
{
  "id": 4,
  "result": {"ok": false, "error": "record not found"}
}
```

### 5.5 `memory.stats`

#### 作用
返回运行时容量、延迟和索引统计。

#### 请求字段

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `workspace_id` | string | 否 | `""` | 为空表示全局统计 |
| `window` | string | 否 | `5m` | 统计窗口提示 |

#### 响应字段（核心）

| 字段 | 类型 | 说明 |
|---|---|---|
| `p95_read_ms` | number | 读 p95 延迟 |
| `p95_write_ms` | number | 写 p95 延迟 |
| `mem_used_bytes` | integer | 估算内存占用 |
| `disk_used_bytes` | integer | 估算磁盘占用 |
| `resident_used_bytes` | integer | 常驻内存占用 |
| `resident_limit_bytes` | integer | 常驻内存上限 |
| `resident_evicted_count` | integer | 常驻淘汰次数 |
| `disk_fallback_search_count` | integer | 磁盘回退查询次数 |
| `write_ack_mode` | string | 写入确认模式 |
| `effective_backend` | string | 当前后端 |
| `index_stats` | object | 索引统计对象 |

#### 成功示例
```json
{
  "id": 5,
  "result": {
    "p95_read_ms": 0.1,
    "p95_write_ms": 40.0,
    "mem_used_bytes": 5199,
    "disk_used_bytes": 4295020544,
    "resident_used_bytes": 1621,
    "resident_limit_bytes": 536870912,
    "resident_evicted_count": 0,
    "disk_fallback_search_count": 0,
    "write_ack_mode": "durable",
    "effective_backend": "eloqstore",
    "index_stats": {"segment_count": 1, "posting_terms": 5, "vector_count": 1}
  }
}
```

#### 失败示例
```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "error": {"code": -32602, "message": "invalid params: params must be object"}
}
```

### 5.6 `memory.compact`

#### 作用
执行 compact/GC 过程并返回回收统计。

#### 请求字段

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `workspace_id` | string | 否 | `""` | 为空表示全局 compact |

#### 响应字段（核心）

| 字段 | 类型 | 说明 |
|---|---|---|
| `triggered` | boolean | 是否触发 |
| `capacity_blocked` | boolean | compact 后是否仍容量受限 |
| `mem_before_bytes` | integer | compact 前内存 |
| `disk_before_bytes` | integer | compact 前磁盘 |
| `mem_after_bytes` | integer | compact 后内存 |
| `disk_after_bytes` | integer | compact 后磁盘 |
| `tombstoned_count` | integer | 新墓碑数量 |
| `deleted_count` | integer | 物理删除数量 |

#### 成功示例
```json
{
  "id": 6,
  "result": {
    "triggered": true,
    "capacity_blocked": false,
    "mem_before_bytes": 5199,
    "disk_before_bytes": 4295020544,
    "mem_after_bytes": 5246,
    "disk_after_bytes": 4295032832,
    "tombstoned_count": 1,
    "deleted_count": 0
  }
}
```

#### 失败示例
```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "error": {"code": -32602, "message": "invalid params: params must be object"}
}
```

## 6. 错误模型

### 6.1 HTTP 层

| 场景 | 状态码 | 返回 |
|---|---|---|
| 请求体 JSON 解析失败 | `400` | `{"error":"invalid JSON"}` |
| 未注册路由（如 `/sync/push`） | `404` | `{"error":"route not found"}` |

### 6.2 JSON-RPC 层

| code | 含义 | 典型触发 |
|---|---|---|
| `-32600` | invalid request | `jsonrpc` 字段非法、`method` 缺失 |
| `-32601` | method not found | 未知方法 |
| `-32602` | invalid params | `params/arguments` 结构非法 |
| `-32001` | store compact busy | `store.compact` 异步任务忙 |

## 7. 语义规则

### 7.1 幂等与写入语义

- `memory.write` 在 `write_mode=upsert` 且命中 `dedup_key` 时返回 `deduped_ids`。
- `write_mode=append` 不做 dedup 合并。

### 7.2 查询语义

- `memory.query` 按 `final` 分数排序。
- `token_budget` 为硬截断条件；`0` 表示不限制。

### 7.3 Pin 约束语义

- `pin` 受配额与比例治理参数约束。
- 超限时 `memory.pin` 返回 `ok=false`；`memory.write(pin=true)` 可能返回 warning。

### 7.4 类型稳定性

- 对外响应采用严格 JSON 类型：数值字段返回 number/integer，布尔字段返回 boolean。
- 客户端不应依赖“数字字符串”形式。

## 8. 内部运维接口（非公开 MCP 方法集合）

内部直调方法：`store.compact`。

返回字段：
- `triggered`
- `noop`
- `busy`
- `async`
- `partition_count`
- `message`

约束：
- 不出现在 `tools/list`。
- 不纳入公开 `memory.*` 方法集合契约。
