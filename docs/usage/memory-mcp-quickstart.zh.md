# Memory MCP 快速使用（ZH）

## 1. 启动

### 1.1 构建与启动

```bash
scripts/install.sh
scripts/start.sh --backend eloqstore
```

### 1.2 健康检查

```bash
curl -sS http://127.0.0.1:8765/health
```

期望返回：
```json
{"status":"ok"}
```

### 1.3 读取机器契约（推荐）

```bash
# 默认精简契约（不含 examples）
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 100,
    "method": "memory.describe",
    "params": {}
  }'
```

```bash
# 调试/代码生成时可打开 examples
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 101,
    "method": "memory.describe",
    "params": {"include_examples": true}
  }'
```

## 2. 最小写入 + 查询

### 2.1 写入

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 1,
    "method": "memory.write",
    "params": {
      "workspace_id": "demo",
      "session_id": "s1",
      "records": [
        {
          "source": "turn",
          "content": "remember retry with exponential backoff",
          "tags": ["retry", "ops"],
          "pin": false
        }
      ]
    }
  }'
```

期望 `result.ok=true`，并返回 `stored_ids`。

### 2.2 查询

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 2,
    "method": "memory.query",
    "params": {
      "workspace_id": "demo",
      "query": "retry backoff",
      "top_k": 3,
      "token_budget": 1200,
      "debug": true
    }
  }'
```

期望返回：
- `hits` 非空
- `hits[0].memory_id` 为字符串
- `debug_stats` 包含分阶段耗时

## 3. pin / stats / compact 实操

### 3.1 pin 与 unpin

```bash
# pin
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 3,
    "method": "memory.pin",
    "params": {
      "workspace_id": "demo",
      "memory_id": "m-REPLACE-WITH-ID",
      "pin": true
    }
  }'

# unpin
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 4,
    "method": "memory.pin",
    "params": {
      "workspace_id": "demo",
      "memory_id": "m-REPLACE-WITH-ID",
      "pin": false
    }
  }'
```

### 3.2 查看统计

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 5,
    "method": "memory.stats",
    "params": {
      "workspace_id": "demo",
      "window": "5m"
    }
  }'
```

重点字段：
- `write_ack_mode`
- `effective_backend`
- `resident_used_bytes`
- `resident_limit_bytes`
- `index_stats`

### 3.3 执行 compact

```bash
curl -sS http://127.0.0.1:8765/mcp \
  -X POST -H 'Content-Type: application/json' \
  -d '{
    "id": 6,
    "method": "memory.compact",
    "params": {
      "workspace_id": "demo"
    }
  }'
```

重点字段：
- `triggered`
- `capacity_blocked`
- `mem_before_bytes` / `mem_after_bytes`
- `disk_before_bytes` / `disk_after_bytes`

## 4. 常见错误与定位

### 4.1 `400 invalid JSON`

现象：请求体 JSON 非法。

排查：
1. 用 `jq .` 校验请求体。
2. 确认字符串引号和逗号位置。

### 4.2 `-32601 method not found`

现象：方法名不在公开集合。

排查：
1. 只使用 `memory.describe/write/query/pin/stats/compact`。
2. 先调用 `memory.describe` 获取实时方法清单。

### 4.3 `memory.pin` 返回 `ok=false`

现象：目标记录不存在或 pin 治理约束触发。

排查：
1. 先执行 `memory.query` 确认 `memory_id`。
2. 查看 `error` 字段具体原因。

### 4.4 `eloqstore` 启动失败（io_uring）

现象：进程启动即退出，日志包含 `io_uring unavailable for eloqstore backend`。

排查：
1. 检查宿主机内核与容器权限。
2. 确认当前运行环境允许 io_uring。

## 5. 生产建议（网关 / 鉴权 / 资源预算）

1. 将 `pgmemd` 放在 API 网关后（Nginx/Caddy/Ingress）。
2. 在网关层强制 TLS 与鉴权（JWT/API Key/mTLS）。
3. 为每个租户分配独立 `workspace_id`，避免逻辑混用。
4. 按峰值容量配置 `--mem-budget-mb` 与 `--disk-budget-gb`。
5. 定期采集 `memory.stats` 指标并设置告警阈值。
